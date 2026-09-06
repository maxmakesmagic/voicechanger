#include "FractionalDelayLine.h"
#include "StereoChorus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Samples = std::vector<int16_t>;

struct StereoSamples {
  Samples left;
  Samples right;
};

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr float kSamplesPerMillisecond = AUDIO_SAMPLE_RATE_EXACT / 1000.0f;

static_assert((AudioEffectStereoChorus::DELAY_LINE_SAMPLES &
               (AudioEffectStereoChorus::DELAY_LINE_SAMPLES - 1U)) == 0,
              "the delay-line wrap mask requires a power-of-two size");
static_assert(AudioEffectStereoChorus::MIN_DELAY_SAMPLES >= 1.0f,
              "cubic interpolation requires a preceding sample");
static_assert(AudioEffectStereoChorus::MAX_DELAY_SAMPLES <=
                AudioEffectStereoChorus::DELAY_LINE_SAMPLES - 3U,
              "cubic interpolation requires four retained samples");

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

std::string measured(const std::string &label, double actual, double expected)
{
  std::ostringstream message;
  message << label << ": measured " << std::fixed << std::setprecision(4)
          << actual << ", expected " << expected;
  return message.str();
}

int16_t toAudioSample(float sample)
{
  if (sample >= 32767.0f) {
    return 32767;
  }
  if (sample <= -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(sample);
}

float millisecondsForSamples(float samples)
{
  return samples / kSamplesPerMillisecond;
}

StereoSamples streamEffect(AudioEffectStereoChorus &effect,
                           const Samples &input,
                           bool forceAllocationFailure = false)
{
  require(input.size() % AUDIO_BLOCK_SAMPLES == 0,
          "test input must contain whole Teensy audio blocks");

  AudioStream::resetTestState();
  AudioStream::setTestAllocationFailure(forceAllocationFailure);
  StereoSamples output;
  output.left.reserve(input.size());
  output.right.reserve(input.size());

  for (std::size_t offset = 0; offset < input.size();
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                AUDIO_BLOCK_SAMPLES,
                block.data);
    AudioStream::queueTestInput(&block);
    effect.update();

    const audio_block_t *left = AudioStream::transmittedSnapshot(0);
    const audio_block_t *right = AudioStream::transmittedSnapshot(1);
    require(left != nullptr && right != nullptr,
            "update() did not transmit both output channels");
    output.left.insert(output.left.end(),
                       std::begin(left->data),
                       std::end(left->data));
    output.right.insert(output.right.end(),
                        std::begin(right->data),
                        std::end(right->data));
    require(AudioStream::ownershipErrorCount() == 0,
            "update() violated AudioStream block ownership");
    require(!AudioStream::hasOutstandingBlocks(),
            "update() leaked an AudioStream block");
  }

  const std::size_t expectedBlocks = input.size() / AUDIO_BLOCK_SAMPLES;
  require(AudioStream::channelTransmissionCount(0) == expectedBlocks,
          "not every left block was transmitted");
  require(AudioStream::channelTransmissionCount(1) == expectedBlocks,
          "not every right block was transmitted");
  require(AudioStream::transmissionCount() == 2U * expectedBlocks,
          "unexpected total transmission count");
  AudioStream::resetTestState();
  return output;
}

Samples makeRamp(std::size_t sampleCount, int start = -12000)
{
  Samples samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    samples[i] = static_cast<int16_t>(start + static_cast<int>(i));
  }
  return samples;
}

Samples makePattern(std::size_t sampleCount)
{
  constexpr std::array<int16_t, 12> pattern = {
    std::numeric_limits<int16_t>::min(),
    -30000,
    -16384,
    -1025,
    -1,
    0,
    1,
    1024,
    16383,
    30000,
    std::numeric_limits<int16_t>::max(),
    731,
  };
  Samples samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    samples[i] = pattern[i % pattern.size()];
  }
  return samples;
}

Samples makeSine(std::size_t sampleCount,
                 double frequencyHz,
                 double amplitude)
{
  Samples samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    samples[i] = toAudioSample(static_cast<float>(
      amplitude * std::sin(kTwoPi * frequencyHz *
                           static_cast<double>(i) /
                           AUDIO_SAMPLE_RATE_EXACT)));
  }
  return samples;
}

void testFractionalDelayInterpolationAndWrap()
{
  voicechanger::FractionalDelayLine<8> delay;
  delay.push(0);
  delay.push(10);
  delay.push(40);
  delay.push(90);
  require(delay.read(1.0f) == 40.0f,
          "an integral delay did not return the exact stored sample");
  require(std::abs(delay.read(1.5f) - 22.5f) < 0.001f,
          measured("cubic fractional read", delay.read(1.5f), 22.5));

  delay.clear();
  for (int sample = 0; sample < 32; ++sample) {
    delay.push(static_cast<int16_t>(10 * sample * sample));
    if (sample >= 8) {
      const float position = static_cast<float>(sample) - 3.5f;
      const float expected = 10.0f * position * position;
      require(std::abs(delay.read(3.5f) - expected) < 0.01f,
              measured("wrapped quadratic interpolation",
                       delay.read(3.5f), expected));
    }
  }

  delay.clear();
  constexpr std::array<float, 4> fractions = {
    0.125f, 0.25f, 0.75f, 0.875f
  };
  for (int sample = 0; sample < 32; ++sample) {
    delay.push(static_cast<int16_t>(sample * sample * sample));
    if (sample >= 8) {
      for (const float fraction : fractions) {
        const float position =
          static_cast<float>(sample) - (3.0f + fraction);
        const float expected = position * position * position;
        const float actual = delay.read(3.0f + fraction);
        require(std::abs(actual - expected) < 0.02f,
                measured("fraction-sweep cubic interpolation",
                         actual, expected));
      }
    }
  }
}

void testNoInputDoesNothing()
{
  AudioStream::resetTestState();
  AudioEffectStereoChorus effect;
  effect.update();
  require(AudioStream::transmissionCount() == 0,
          "an empty update transmitted a block");
  require(AudioStream::releaseCount() == 0,
          "an empty update released a block");
  require(AudioStream::ownershipErrorCount() == 0 &&
            !AudioStream::hasOutstandingBlocks(),
          "an empty update changed AudioStream ownership");
  AudioStream::resetTestState();
}

void testSilenceIsExactlySilent()
{
  AudioEffectStereoChorus effect;
  const Samples silence(4U * AudioEffectStereoChorus::DELAY_LINE_SAMPLES, 0);
  const StereoSamples output = streamEffect(effect, silence);
  require(std::all_of(output.left.begin(), output.left.end(),
                      [](int16_t sample) { return sample == 0; }),
          "silence produced a nonzero left sample");
  require(std::all_of(output.right.begin(), output.right.end(),
                      [](int16_t sample) { return sample == 0; }),
          "silence produced a nonzero right sample");
}

void testDryBypassIsBitExact()
{
  const Samples input = makePattern(32U * AUDIO_BLOCK_SAMPLES);
  AudioEffectStereoChorus effect;
  require(effect.configure(18.0f, 4.0f, 0.35f, 0.0f),
          "valid dry configuration was rejected");
  const StereoSamples output = streamEffect(effect, input);
  require(output.left == input, "dry left output was not bit exact");
  require(output.right == input, "dry right output was not bit exact");
}

void testModulatedRampMatchesReference()
{
  constexpr std::size_t sampleCount = 128U * AUDIO_BLOCK_SAMPLES;
  constexpr float delayMs = 10.0f;
  constexpr float depthMs = 2.0f;
  constexpr float rateHz = 5.0f;
  const Samples input = makeRamp(sampleCount);

  AudioEffectStereoChorus effect;
  require(effect.configure(delayMs, depthMs, rateHz, 1.0f),
          "valid wet configuration was rejected");
  const StereoSamples output = streamEffect(effect, input);

  const double centerSamples = delayMs * kSamplesPerMillisecond;
  const double depthSamples = depthMs * kSamplesPerMillisecond;
  int maximumError = 0;
  bool channelsSeparated = false;
  for (std::size_t i = 1024; i < sampleCount; ++i) {
    const double sine = std::sin(kTwoPi * rateHz *
                                 static_cast<double>(i) /
                                 AUDIO_SAMPLE_RATE_EXACT);
    const float leftReference = static_cast<float>(
      -12000.0 + static_cast<double>(i) -
      (centerSamples + depthSamples * sine));
    const float rightReference = static_cast<float>(
      -12000.0 + static_cast<double>(i) -
      (centerSamples - depthSamples * sine));
    const int leftError = std::abs(
      static_cast<int>(output.left[i]) - toAudioSample(leftReference));
    const int rightError = std::abs(
      static_cast<int>(output.right[i]) - toAudioSample(rightReference));
    maximumError = std::max({maximumError, leftError, rightError});
    channelsSeparated = channelsSeparated || output.left[i] != output.right[i];
  }
  require(maximumError <= 2,
          measured("maximum modulated-ramp error", maximumError, 2));
  require(channelsSeparated,
          "opposite LFO phases did not produce distinct stereo outputs");
}

void testDefaultPresetProducesMaterialStereoModulation()
{
  require(AudioEffectStereoChorus::DEFAULT_RATE_HZ > 0.0f,
          "the default chorus preset has no modulation");
  const std::size_t oneCycleSamples = static_cast<std::size_t>(std::ceil(
    AUDIO_SAMPLE_RATE_EXACT / AudioEffectStereoChorus::DEFAULT_RATE_HZ));
  const std::size_t sampleCount =
    ((oneCycleSamples + AUDIO_BLOCK_SAMPLES - 1U) /
     AUDIO_BLOCK_SAMPLES) * AUDIO_BLOCK_SAMPLES;

  AudioEffectStereoChorus effect;
  const StereoSamples output = streamEffect(
    effect, makeSine(sampleCount, 440.0, 12000.0));

  double midEnergy = 0.0;
  double sideEnergy = 0.0;
  for (std::size_t i = AudioEffectStereoChorus::DELAY_LINE_SAMPLES;
       i < sampleCount; ++i) {
    const double mid = 0.5 * (static_cast<double>(output.left[i]) +
                              static_cast<double>(output.right[i]));
    const double side = 0.5 * (static_cast<double>(output.left[i]) -
                               static_cast<double>(output.right[i]));
    midEnergy += mid * mid;
    sideEnergy += side * side;
  }

  require(midEnergy > 0.0, "the default chorus produced no mid signal");
  const double sideToMidRms = std::sqrt(sideEnergy / midEnergy);
  require(sideToMidRms > 0.2,
          measured("default chorus side/mid RMS", sideToMidRms, 0.2));
}

void testFractionalImpulseAndWetMix()
{
  Samples fractionalInput(AUDIO_BLOCK_SAMPLES, 0);
  fractionalInput[0] = 32000;
  AudioEffectStereoChorus fractionalEffect;
  require(fractionalEffect.configure(millisecondsForSamples(1.5f),
                                       0.0f, 0.0f, 1.0f),
          "valid fractional-delay configuration was rejected");
  const StereoSamples fractional =
    streamEffect(fractionalEffect, fractionalInput);
  const std::array<int16_t, 4> expectedKernel = {
    -2000, 18000, 18000, -2000
  };
  for (std::size_t i = 0; i < expectedKernel.size(); ++i) {
    require(fractional.left[i] == expectedKernel[i] &&
              fractional.right[i] == expectedKernel[i],
            measured("fractional-delay impulse tap",
                     fractional.left[i], expectedKernel[i]));
  }

  constexpr std::size_t delaySamples = 256;
  Samples mixedInput(4U * AUDIO_BLOCK_SAMPLES, 0);
  mixedInput[100] = 32000;
  AudioEffectStereoChorus mixedEffect;
  require(mixedEffect.configure(millisecondsForSamples(delaySamples),
                                  0.0f, 0.0f, 0.25f),
          "valid crossfade configuration was rejected");
  const StereoSamples mixed = streamEffect(mixedEffect, mixedInput);
  Samples expected(mixedInput.size(), 0);
  expected[100] = 24000;
  expected[100 + delaySamples] = 8000;
  require(mixed.left == expected && mixed.right == expected,
          "dry/wet crossfade or fixed-delay timing was incorrect");
}

void testCubicOvershootSaturates()
{
  const auto runPattern = [](const std::array<int16_t, 4> &pattern) {
    Samples input(AUDIO_BLOCK_SAMPLES, 0);
    std::copy(pattern.begin(), pattern.end(), input.begin());
    AudioEffectStereoChorus effect;
    require(effect.configure(millisecondsForSamples(1.5f),
                             0.0f, 0.0f, 1.0f),
            "valid saturation-test configuration was rejected");
    return streamEffect(effect, input);
  };

  const StereoSamples positive = runPattern({{
    std::numeric_limits<int16_t>::min(),
    std::numeric_limits<int16_t>::max(),
    std::numeric_limits<int16_t>::max(),
    std::numeric_limits<int16_t>::min(),
  }});
  require(positive.left[3] == std::numeric_limits<int16_t>::max() &&
            positive.right[3] == std::numeric_limits<int16_t>::max(),
          "positive cubic overshoot did not saturate");

  const StereoSamples negative = runPattern({{
    std::numeric_limits<int16_t>::max(),
    std::numeric_limits<int16_t>::min(),
    std::numeric_limits<int16_t>::min(),
    std::numeric_limits<int16_t>::max(),
  }});
  require(negative.left[3] == std::numeric_limits<int16_t>::min() &&
            negative.right[3] == std::numeric_limits<int16_t>::min(),
          "negative cubic overshoot did not saturate");
}

void testDelayEndpointsAndMaximumRate()
{
  for (const std::size_t delaySamples : {
         static_cast<std::size_t>(AudioEffectStereoChorus::MIN_DELAY_SAMPLES),
         static_cast<std::size_t>(AudioEffectStereoChorus::MAX_DELAY_SAMPLES)}) {
    const std::size_t sampleCount =
      ((delaySamples + 1U + AUDIO_BLOCK_SAMPLES - 1U) /
       AUDIO_BLOCK_SAMPLES) * AUDIO_BLOCK_SAMPLES;
    Samples input(sampleCount, 0);
    input[0] = 16000;
    Samples expected(sampleCount, 0);
    expected[delaySamples] = 16000;

    AudioEffectStereoChorus effect;
    require(effect.configure(millisecondsForSamples(
                               static_cast<float>(delaySamples)),
                             0.0f, 0.0f, 1.0f),
            "an interpolation endpoint was rejected");
    const StereoSamples output = streamEffect(effect, input);
    require(output.left == expected && output.right == expected,
            "an interpolation endpoint produced the wrong impulse timing");
  }

  const float centre =
    (AudioEffectStereoChorus::MIN_DELAY_SAMPLES +
     AudioEffectStereoChorus::MAX_DELAY_SAMPLES) * 0.5f;
  const float depth =
    (AudioEffectStereoChorus::MAX_DELAY_SAMPLES -
     AudioEffectStereoChorus::MIN_DELAY_SAMPLES) * 0.5f;
  AudioEffectStereoChorus envelopeEffect;
  require(envelopeEffect.configure(millisecondsForSamples(centre),
                                     millisecondsForSamples(depth),
                                     0.0f, 1.0f),
          "the exact delay envelope bounds were rejected");
  require(envelopeEffect.configure(18.0f, 4.0f,
                                     AudioEffectStereoChorus::MAX_RATE_HZ,
                                     0.5f),
          "the documented maximum LFO rate was rejected");
}

void testSettledDcGain()
{
  constexpr std::size_t sampleCount = 32U * AUDIO_BLOCK_SAMPLES;
  constexpr std::size_t settledStart = 1536;
  for (const int16_t level : {std::numeric_limits<int16_t>::min(),
                              std::numeric_limits<int16_t>::max()}) {
    AudioEffectStereoChorus effect;
    const StereoSamples output = streamEffect(effect, Samples(sampleCount, level));
    for (std::size_t i = settledStart; i < sampleCount; ++i) {
      require(std::abs(static_cast<int>(output.left[i]) - level) <= 1,
              measured("settled left DC gain", output.left[i], level));
      require(std::abs(static_cast<int>(output.right[i]) - level) <= 1,
              measured("settled right DC gain", output.right[i], level));
    }
  }
}

void testInvalidConfigurationsAreAtomic()
{
  constexpr float baselineDelay = 12.0f;
  constexpr float baselineDepth = 2.0f;
  constexpr float baselineRate = 1.7f;
  constexpr float baselineWet = 0.63f;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float maximumDelayMs = millisecondsForSamples(
    AudioEffectStereoChorus::MAX_DELAY_SAMPLES);
  const std::array<std::array<float, 4>, 13> invalid = {{
    {{nan, baselineDepth, baselineRate, baselineWet}},
    {{baselineDelay, infinity, baselineRate, baselineWet}},
    {{baselineDelay, baselineDepth, nan, baselineWet}},
    {{baselineDelay, baselineDepth, baselineRate, infinity}},
    {{0.0f, baselineDepth, baselineRate, baselineWet}},
    {{baselineDelay, -0.1f, baselineRate, baselineWet}},
    {{baselineDelay, baselineDepth, -0.1f, baselineWet}},
    {{baselineDelay, baselineDepth,
      AudioEffectStereoChorus::MAX_RATE_HZ + 0.1f, baselineWet}},
    {{baselineDelay, baselineDepth, baselineRate, -0.1f}},
    {{baselineDelay, baselineDepth, baselineRate, 1.1f}},
    {{millisecondsForSamples(2.0f), millisecondsForSamples(2.0f),
      baselineRate, baselineWet}},
    {{maximumDelayMs, 1.0f, baselineRate, baselineWet}},
    {{std::numeric_limits<float>::max(), 0.0f,
      baselineRate, baselineWet}},
  }};

  AudioEffectStereoChorus candidate;
  AudioEffectStereoChorus reference;
  require(candidate.configure(baselineDelay, baselineDepth,
                              baselineRate, baselineWet),
          "candidate baseline configuration was rejected");
  require(reference.configure(baselineDelay, baselineDepth,
                              baselineRate, baselineWet),
          "reference baseline configuration was rejected");
  for (const auto &values : invalid) {
    require(!candidate.configure(values[0], values[1], values[2], values[3]),
            "an invalid chorus configuration was accepted");
  }

  const Samples input = makeRamp(48U * AUDIO_BLOCK_SAMPLES);
  const StereoSamples candidateOutput = streamEffect(candidate, input);
  const StereoSamples referenceOutput = streamEffect(reference, input);
  require(candidateOutput.left == referenceOutput.left &&
            candidateOutput.right == referenceOutput.right,
          "rejected configuration changed subsequent output");
}

void testResetClearsHistoryAndPhase()
{
  const Samples disturbance = makePattern(24U * AUDIO_BLOCK_SAMPLES);
  const Samples probe = makeRamp(48U * AUDIO_BLOCK_SAMPLES);
  AudioEffectStereoChorus resetEffect;
  AudioEffectStereoChorus freshEffect;
  require(resetEffect.configure(14.0f, 3.0f, 2.0f, 0.75f) &&
            freshEffect.configure(14.0f, 3.0f, 2.0f, 0.75f),
          "valid reset-test configuration was rejected");
  streamEffect(resetEffect, disturbance);
  resetEffect.reset();

  const StereoSamples afterReset = streamEffect(resetEffect, probe);
  const StereoSamples fresh = streamEffect(freshEffect, probe);
  require(afterReset.left == fresh.left && afterReset.right == fresh.right,
          "reset did not restore fresh delay and LFO state");
}

void testBypassContinuesRecordingHistoryAndPhase()
{
  constexpr float wetMix = 0.65f;
  AudioEffectStereoChorus bypassed;
  AudioEffectStereoChorus reference;
  require(bypassed.configure(14.0f, 3.0f, 2.3f, 0.0f),
          "valid bypass-history configuration was rejected");
  require(reference.configure(14.0f, 3.0f, 2.3f, wetMix),
          "valid bypass reference configuration was rejected");

  const Samples prefix = makePattern(8U * AUDIO_BLOCK_SAMPLES);
  streamEffect(bypassed, prefix);
  streamEffect(reference, prefix);

  require(bypassed.configure(14.0f, 3.0f, 2.3f, wetMix),
          "valid wet configuration was rejected after bypass");
  const Samples probe = makePattern(24U * AUDIO_BLOCK_SAMPLES);
  const StereoSamples afterBypass = streamEffect(bypassed, probe);
  const StereoSamples expected = streamEffect(reference, probe);
  require(afterBypass.left == expected.left &&
            afterBypass.right == expected.right,
          "bypass did not preserve delay history and LFO phase");
}

void testAllocationFailureFallsBackToDryAndPreservesState()
{
  constexpr float wetMix = 0.65f;
  AudioEffectStereoChorus starved;
  AudioEffectStereoChorus reference;
  require(starved.configure(14.0f, 3.0f, 2.3f, wetMix) &&
            reference.configure(14.0f, 3.0f, 2.3f, wetMix),
          "valid allocation-failure configuration was rejected");

  const Samples prefix = makePattern(AUDIO_BLOCK_SAMPLES);
  audio_block_t block{};
  std::copy(prefix.begin(), prefix.end(), std::begin(block.data));
  AudioStream::resetTestState();
  AudioStream::setTestAllocationFailure(true);
  AudioStream::queueTestInput(&block);
  starved.update();
  require(AudioStream::lastTransmitted(0) == &block &&
            AudioStream::lastTransmitted(1) == &block,
          "allocation failure did not send the dry block to both outputs");
  const audio_block_t *left = AudioStream::transmittedSnapshot(0);
  const audio_block_t *right = AudioStream::transmittedSnapshot(1);
  require(left != nullptr && right != nullptr &&
            std::equal(prefix.begin(), prefix.end(), std::begin(left->data)) &&
            std::equal(prefix.begin(), prefix.end(), std::begin(right->data)),
          "allocation fallback did not preserve the complete dry block");
  require(AudioStream::transmissionCount() == 2 &&
            AudioStream::releaseCount() == 1,
          "allocation fallback mishandled AudioStream ownership");
  require(AudioStream::ownershipErrorCount() == 0 &&
            !AudioStream::hasOutstandingBlocks(),
          "allocation fallback violated AudioStream ownership");

  streamEffect(reference, prefix);
  const Samples probe = makePattern(24U * AUDIO_BLOCK_SAMPLES);
  const StereoSamples afterFailure = streamEffect(starved, probe);
  const StereoSamples expected = streamEffect(reference, probe);
  require(afterFailure.left == expected.left &&
            afterFailure.right == expected.right,
          "allocation fallback did not preserve delay history and LFO phase");
}

} // namespace

int main()
{
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"fractional-delay interpolation and wrap",
     testFractionalDelayInterpolationAndWrap},
    {"no input", testNoInputDoesNothing},
    {"silence", testSilenceIsExactlySilent},
    {"bit-exact dry bypass", testDryBypassIsBitExact},
    {"modulated ramp reference", testModulatedRampMatchesReference},
    {"material default stereo modulation",
     testDefaultPresetProducesMaterialStereoModulation},
    {"fractional impulse and wet mix", testFractionalImpulseAndWetMix},
    {"cubic-overshoot saturation", testCubicOvershootSaturates},
    {"delay endpoints and maximum rate", testDelayEndpointsAndMaximumRate},
    {"settled DC gain", testSettledDcGain},
    {"atomic configuration validation", testInvalidConfigurationsAreAtomic},
    {"state reset", testResetClearsHistoryAndPhase},
    {"state through bypass", testBypassContinuesRecordingHistoryAndPhase},
    {"AudioMemory allocation fallback",
     testAllocationFailureFallsBackToDryAndPreservesState},
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
