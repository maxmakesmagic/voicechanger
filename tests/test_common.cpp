#include "VoiceChangerCommon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string &message)
{
  if (!condition) {
    throw TestFailure(message);
  }
}

void testSaturatingFloatToInt16()
{
  struct TestCase {
    float input;
    int16_t expected;
  };

  const std::array<TestCase, 17> cases = {{
    {-std::numeric_limits<float>::infinity(), INT16_MIN},
    {-40000.0f, INT16_MIN},
    {-32768.0f, INT16_MIN},
    {-32767.75f, -32767},
    {-1.75f, -1},
    {-0.5f, 0},
    {-0.0f, 0},
    {0.0f, 0},
    {0.5f, 0},
    {1.75f, 1},
    {32766.75f, 32766},
    {32767.0f, INT16_MAX},
    {40000.0f, INT16_MAX},
    {std::numeric_limits<float>::infinity(), INT16_MAX},
    {std::numeric_limits<float>::quiet_NaN(), 0},
    {std::numeric_limits<float>::lowest(), INT16_MIN},
    {std::numeric_limits<float>::max(), INT16_MAX},
  }};

  for (const TestCase &testCase : cases) {
    const int16_t actual =
      voicechanger::saturatingFloatToInt16(testCase.input);
    require(actual == testCase.expected,
            "saturating conversion returned " + std::to_string(actual) +
              " instead of " + std::to_string(testCase.expected));
  }
}

void requireShieldCall(const AudioControlSGTL5000::Call &call,
                       AudioControlSGTL5000::Operation operation,
                       float argument,
                       const std::string &description)
{
  require(call.operation == operation, description + " was out of order");
  require(std::abs(call.argument - argument) < 0.000001f,
          description + " received the wrong value");
}

void requireExpectedShieldCalls(const AudioControlSGTL5000 &shield,
                                unsigned int gainDb,
                                float outputVolume)
{
  require(shield.calls.size() == 4,
          "Audio Shield setup did not attempt all four operations");
  requireShieldCall(shield.calls[0],
                    AudioControlSGTL5000::Operation::Enable,
                    0.0f,
                    "enable");
  requireShieldCall(shield.calls[1],
                    AudioControlSGTL5000::Operation::InputSelect,
                    static_cast<float>(AUDIO_INPUT_MIC),
                    "microphone selection");
  requireShieldCall(shield.calls[2],
                    AudioControlSGTL5000::Operation::MicrophoneGain,
                    static_cast<float>(gainDb),
                    "microphone gain");
  requireShieldCall(shield.calls[3],
                    AudioControlSGTL5000::Operation::Volume,
                    outputVolume,
                    "output volume");
}

void testAudioShieldConfiguration()
{
  for (const voicechanger::AudioShieldMicrophoneConfig config : {
         voicechanger::AudioShieldMicrophoneConfig{20U, 0.5f},
         voicechanger::AudioShieldMicrophoneConfig{40U, 0.625f}}) {
    AudioControlSGTL5000 shield;
    require(voicechanger::configureAudioShieldMicrophone(shield, config),
            "successful Audio Shield operations reported failure");
    requireExpectedShieldCalls(shield, config.gainDb, config.outputVolume);
  }
}

void testAudioShieldFailureAggregation()
{
  for (std::size_t failedOperation = 0; failedOperation < 4;
       ++failedOperation) {
    AudioControlSGTL5000 shield;
    switch (failedOperation) {
      case 0:
        shield.enableResult = false;
        break;
      case 1:
        shield.inputSelectResult = false;
        break;
      case 2:
        shield.microphoneGainResult = false;
        break;
      case 3:
        shield.volumeResult = false;
        break;
      default:
        throw TestFailure("invalid Audio Shield test operation");
    }

    const voicechanger::AudioShieldMicrophoneConfig config{31U, 0.75f};
    require(!voicechanger::configureAudioShieldMicrophone(shield, config),
            "a failed Audio Shield operation was not propagated");
    requireExpectedShieldCalls(shield, config.gainDb, config.outputVolume);
  }
}

class RecordingPrint final : public Print {
public:
  std::size_t write(uint8_t value) override
  {
    beforeWrite();
    text_.push_back(static_cast<char>(value));
    return 1;
  }

  const std::string &text() const
  {
    return text_;
  }

  bool printedWithInterruptsDisabled() const
  {
    return printedWithInterruptsDisabled_;
  }

private:
  void beforeWrite()
  {
    printedWithInterruptsDisabled_ =
      printedWithInterruptsDisabled_ || audio_test::interruptsDisabled;
    if (!startedPrinting_) {
      audio_test::record("print");
      startedPrinting_ = true;
    }
  }

  std::string text_;
  bool startedPrinting_ = false;
  bool printedWithInterruptsDisabled_ = false;
};

class FakeEffect {
public:
  float processorUsage()
  {
    audio_test::record("effect-current");
    return currentUsage;
  }

  float processorUsageMax()
  {
    audio_test::record("effect-peak");
    return peakUsage;
  }

  void processorUsageMaxReset()
  {
    audio_test::record("effect-reset");
    peakUsage = currentUsage;
    ++resetCount;
  }

  float currentUsage = 12.5f;
  float peakUsage = 34.75f;
  std::size_t resetCount = 0;
};

void setUsageValues()
{
  audio_test::totalProcessorUsage = 56.25f;
  audio_test::totalProcessorUsageMax = 78.5f;
  audio_test::memoryUsage = 9;
  audio_test::memoryUsageMax = 14;
}

void testUsageReporterTimingAndSnapshot()
{
  arduino_test::resetClock();
  audio_test::resetState();
  setUsageValues();
  FakeEffect effect;
  RecordingPrint output;
  voicechanger::AudioUsageReporter reporter;

  arduino_test::setMilliseconds(999UL);
  require(!reporter.reportIfDue(effect, "Chorus", 1000UL, output),
          "usage reporter ran before its interval elapsed");
  require(audio_test::events.empty() && output.text().empty(),
          "an early usage report read metrics or printed output");

  arduino_test::setMilliseconds(1000UL);
  require(reporter.reportIfDue(effect, "Chorus", 1000UL, output),
          "usage reporter did not run at its interval boundary");

  const std::vector<std::string> expectedEvents = {
    "interrupts-off",
    "effect-current",
    "effect-peak",
    "total-current",
    "total-peak",
    "memory-current",
    "memory-peak",
    "effect-reset",
    "total-reset",
    "memory-reset",
    "interrupts-on",
    "print",
  };
  require(audio_test::events == expectedEvents,
          "usage snapshot, resets, and output occurred in the wrong order");
  require(!audio_test::interruptsDisabled,
          "usage reporting left Audio Library interrupts disabled");
  require(!output.printedWithInterruptsDisabled(),
          "usage reporter printed while Audio Library interrupts were disabled");
  require(output.text() ==
            "Chorus CPU: 12.50% (peak 34.75%)  Total CPU: 56.25% "
            "(peak 78.50%)  Memory: 9 (max 14)\r\n",
          "usage reporter rendered the wrong snapshot");
  require(effect.resetCount == 1 && effect.peakUsage == effect.currentUsage,
          "effect peak usage was not reset after it was captured");
  require(audio_test::processorMaxResetCount == 1 &&
            audio_test::totalProcessorUsageMax ==
              audio_test::totalProcessorUsage,
          "total CPU peak usage was not reset after capture");
  require(audio_test::memoryMaxResetCount == 1 &&
            audio_test::memoryUsageMax == audio_test::memoryUsage,
          "memory peak usage was not reset after capture");

  arduino_test::setMilliseconds(1999UL);
  require(!reporter.reportIfDue(effect, "Chorus", 1000UL, output),
          "usage reporter did not restart its timer after reporting");
  require(effect.resetCount == 1,
          "an early second report reset the effect peak");

  setUsageValues();
  effect.peakUsage = 40.0f;
  arduino_test::setMilliseconds(2000UL);
  require(reporter.reportIfDue(effect, "Chorus", 1000UL, output),
          "usage reporter did not run at the next interval boundary");
  require(effect.resetCount == 2 &&
            audio_test::processorMaxResetCount == 2 &&
            audio_test::memoryMaxResetCount == 2,
          "the second usage report did not reset every peak counter");
  require(std::count(output.text().begin(), output.text().end(), '\n') == 2,
          "usage reporter did not emit exactly one line per report");
}

} // namespace

int main()
{
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"saturating float-to-PCM16 conversion", testSaturatingFloatToInt16},
    {"Audio Shield configuration", testAudioShieldConfiguration},
    {"Audio Shield failure aggregation", testAudioShieldFailureAggregation},
    {"usage reporter timing and snapshot",
     testUsageReporterTimingAndSnapshot},
  };

  std::size_t failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " tests passed\n";
  return 0;
}
