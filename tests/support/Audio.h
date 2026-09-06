#ifndef TEST_SUPPORT_AUDIO_H_
#define TEST_SUPPORT_AUDIO_H_

#include "Arduino.h"
#include "AudioStream.h"

#include <string>
#include <vector>

#define AUDIO_INPUT_LINEIN 0
#define AUDIO_INPUT_MIC 1

namespace audio_test {

inline float totalProcessorUsage = 0.0f;
inline float totalProcessorUsageMax = 0.0f;
inline unsigned int memoryUsage = 0;
inline unsigned int memoryUsageMax = 0;
inline bool interruptsDisabled = false;
inline std::size_t processorMaxResetCount = 0;
inline std::size_t memoryMaxResetCount = 0;
inline std::vector<std::string> events;

inline void resetState()
{
  totalProcessorUsage = 0.0f;
  totalProcessorUsageMax = 0.0f;
  memoryUsage = 0;
  memoryUsageMax = 0;
  interruptsDisabled = false;
  processorMaxResetCount = 0;
  memoryMaxResetCount = 0;
  events.clear();
}

inline void record(const char *event)
{
  events.emplace_back(event);
}

inline void noInterrupts()
{
  record("interrupts-off");
  interruptsDisabled = true;
}

inline void interrupts()
{
  record("interrupts-on");
  interruptsDisabled = false;
}

inline float processorUsage()
{
  record("total-current");
  return totalProcessorUsage;
}

inline float processorUsageMax()
{
  record("total-peak");
  return totalProcessorUsageMax;
}

inline unsigned int audioMemoryUsage()
{
  record("memory-current");
  return memoryUsage;
}

inline unsigned int audioMemoryUsageMax()
{
  record("memory-peak");
  return memoryUsageMax;
}

inline void processorUsageMaxReset()
{
  record("total-reset");
  totalProcessorUsageMax = totalProcessorUsage;
  ++processorMaxResetCount;
}

inline void audioMemoryUsageMaxReset()
{
  record("memory-reset");
  memoryUsageMax = memoryUsage;
  ++memoryMaxResetCount;
}

} // namespace audio_test

#define AudioNoInterrupts() (::audio_test::noInterrupts())
#define AudioInterrupts() (::audio_test::interrupts())
#define AudioProcessorUsage() (::audio_test::processorUsage())
#define AudioProcessorUsageMax() (::audio_test::processorUsageMax())
#define AudioProcessorUsageMaxReset() \
  (::audio_test::processorUsageMaxReset())
#define AudioMemoryUsage() (::audio_test::audioMemoryUsage())
#define AudioMemoryUsageMax() (::audio_test::audioMemoryUsageMax())
#define AudioMemoryUsageMaxReset() (::audio_test::audioMemoryUsageMaxReset())

class AudioControlSGTL5000 {
public:
  enum class Operation {
    Enable,
    InputSelect,
    MicrophoneGain,
    Volume,
  };

  struct Call {
    Operation operation;
    float argument;
  };

  bool enable()
  {
    calls.push_back({Operation::Enable, 0.0f});
    return enableResult;
  }

  bool inputSelect(int input)
  {
    calls.push_back({Operation::InputSelect, static_cast<float>(input)});
    return inputSelectResult;
  }

  bool micGain(unsigned int gainDb)
  {
    calls.push_back(
      {Operation::MicrophoneGain, static_cast<float>(gainDb)});
    return microphoneGainResult;
  }

  bool volume(float outputVolume)
  {
    calls.push_back({Operation::Volume, outputVolume});
    return volumeResult;
  }

  bool enableResult = true;
  bool inputSelectResult = true;
  bool microphoneGainResult = true;
  bool volumeResult = true;
  std::vector<Call> calls;
};

#endif
