#include "PitchShiftFFT.h"

#include "arm_const_structs.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Samples = std::vector<int16_t>;

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kStreamingLatency =
  AudioEffectPitchShiftFFT::FFT_SIZE - AUDIO_BLOCK_SAMPLES;

static_assert((AudioEffectPitchShiftFFT::FFT_SIZE &
               (AudioEffectPitchShiftFFT::FFT_SIZE - 1)) == 0,
              "FFT size must be a power of two");
static_assert(AudioEffectPitchShiftFFT::FFT_SIZE ==
                4 * AudioEffectPitchShiftFFT::HOP_SIZE,
              "the overlap-add gain assumes four Hann windows");
static_assert(AudioEffectPitchShiftFFT::FFT_SIZE %
                AudioEffectPitchShiftFFT::HOP_SIZE == 0,
              "hop size must divide the FFT size");
static_assert(AudioEffectPitchShiftFFT::HOP_SIZE % AUDIO_BLOCK_SAMPLES == 0,
              "a frame must be scheduled on an audio block boundary");
static_assert((AudioEffectPitchShiftFFT::OUT_FIFO_SIZE &
               (AudioEffectPitchShiftFFT::OUT_FIFO_SIZE - 1)) == 0,
              "the FIFO wraparound mask requires a power-of-two size");
static_assert(AudioEffectPitchShiftFFT::OUT_FIFO_SIZE >=
                2 * AudioEffectPitchShiftFFT::HOP_SIZE,
              "the FIFO must have headroom for at least two hops");

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

Samples makeTone(double fftBin, double amplitude, std::size_t sampleCount)
{
  Samples samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const double value =
      amplitude * std::sin(kTwoPi * fftBin * static_cast<double>(i) /
                           AudioEffectPitchShiftFFT::FFT_SIZE);
    samples[i] = static_cast<int16_t>(std::lround(value));
  }
  return samples;
}

template <typename ConfigureEffect>
Samples runConfiguredEffect(const Samples &input, ConfigureEffect configureEffect)
{
  require(input.size() % AUDIO_BLOCK_SAMPLES == 0,
          "test input must contain whole Teensy audio blocks");

  AudioStream::resetTestState();
  AudioEffectPitchShiftFFT effect;
  configureEffect(effect);

  Samples output;
  output.reserve(input.size());
  for (std::size_t offset = 0; offset < input.size();
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                AUDIO_BLOCK_SAMPLES,
                block.data);
    AudioStream::queueTestInput(&block);
    effect.update();
    require(AudioStream::lastTransmitted() == &block,
            "update() did not transmit the writable input block");
    output.insert(output.end(), std::begin(block.data), std::end(block.data));
  }

  const std::size_t expectedBlocks = input.size() / AUDIO_BLOCK_SAMPLES;
  require(AudioStream::transmissionCount() == expectedBlocks,
          "not every input block was transmitted");
  require(AudioStream::releaseCount() == expectedBlocks,
          "not every input block was released");
  AudioStream::resetTestState();
  return output;
}

Samples runEffect(const Samples &input, float ratio = 1.0f)
{
  return runConfiguredEffect(input, [ratio](AudioEffectPitchShiftFFT &effect) {
    effect.setPitchRatio(ratio);
  });
}

double rms(const Samples &samples, std::size_t start, std::size_t count)
{
  require(start + count <= samples.size(), "RMS interval is out of range");
  long double energy = 0.0;
  for (std::size_t i = start; i < start + count; ++i) {
    const long double value = samples[i];
    energy += value * value;
  }
  return std::sqrt(static_cast<double>(energy / count));
}

double mean(const Samples &samples, std::size_t start, std::size_t count)
{
  require(start + count <= samples.size(), "mean interval is out of range");
  long double sum = 0.0;
  for (std::size_t i = start; i < start + count; ++i) {
    sum += samples[i];
  }
  return static_cast<double>(sum / count);
}

double magnitudeAt(const Samples &samples,
                   std::size_t start,
                   std::size_t count,
                   double fftBin)
{
  require(start + count <= samples.size(),
          "spectral measurement interval is out of range");
  const double step =
    kTwoPi * fftBin / AudioEffectPitchShiftFFT::FFT_SIZE;
  const std::complex<double> rotation(std::cos(step), -std::sin(step));
  std::complex<double> oscillator(1.0, 0.0);
  std::complex<long double> projection(0.0, 0.0);

  for (std::size_t i = 0; i < start + count; ++i) {
    if (i >= start) {
      projection += static_cast<long double>(samples[i]) *
                    std::complex<long double>(oscillator.real(),
                                              oscillator.imag());
    }
    oscillator *= rotation;
  }
  return 2.0 * static_cast<double>(std::abs(projection)) /
         static_cast<double>(count);
}

int dominantIntegerBin(const Samples &samples,
                       std::size_t start,
                       std::size_t count)
{
  int bestBin = 1;
  double bestMagnitude = -1.0;
  for (int bin = 1; bin < AudioEffectPitchShiftFFT::NUM_BINS - 1; ++bin) {
    const double magnitude = magnitudeAt(samples, start, count, bin);
    if (magnitude > bestMagnitude) {
      bestMagnitude = magnitude;
      bestBin = bin;
    }
  }
  return bestBin;
}

double peakBinNear(const Samples &samples,
                   std::size_t start,
                   std::size_t count,
                   double center,
                   double radius,
                   double increment)
{
  double bestBin = center - radius;
  double bestMagnitude = -1.0;
  for (double bin = center - radius; bin <= center + radius;
       bin += increment) {
    const double magnitude = magnitudeAt(samples, start, count, bin);
    if (magnitude > bestMagnitude) {
      bestMagnitude = magnitude;
      bestBin = bin;
    }
  }
  return bestBin;
}

void testReferenceFftRoundTrip()
{
  std::vector<float> values(2 * AudioEffectPitchShiftFFT::FFT_SIZE);
  for (std::size_t i = 0; i < AudioEffectPitchShiftFFT::FFT_SIZE; ++i) {
    values[2 * i] = static_cast<float>(
      0.25 * std::sin(kTwoPi * 7.0 * static_cast<double>(i) /
                      AudioEffectPitchShiftFFT::FFT_SIZE) +
      0.5 * std::cos(kTwoPi * 113.0 * static_cast<double>(i) /
                     AudioEffectPitchShiftFFT::FFT_SIZE));
    values[2 * i + 1] = static_cast<float>(
      0.1 * std::sin(kTwoPi * 23.0 * static_cast<double>(i) /
                     AudioEffectPitchShiftFFT::FFT_SIZE));
  }
  const std::vector<float> original = values;

  arm_cfft_f32(&arm_cfft_sR_f32_len1024, values.data(), 0, 1);
  arm_cfft_f32(&arm_cfft_sR_f32_len1024, values.data(), 1, 1);

  double maxError = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    maxError = std::max(maxError,
                        std::abs(static_cast<double>(values[i] - original[i])));
  }
  require(maxError < 2.0e-5,
          measured("reference FFT round-trip error", maxError, 0.0));
}

void testNoInputDoesNothing()
{
  AudioStream::resetTestState();
  AudioEffectPitchShiftFFT effect;
  effect.update();
  require(AudioStream::transmissionCount() == 0,
          "update() transmitted a block when no input was available");
  require(AudioStream::releaseCount() == 0,
          "update() released a block when no input was available");
}

void testSilenceIsExactlySilent()
{
  const Samples input(8 * AudioEffectPitchShiftFFT::FFT_SIZE, 0);
  const Samples output = runEffect(input, 1.25f);
  require(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 0; }),
          "silence produced a non-zero output sample");
}

void testInvalidRatiosRetainTheLastValidRatio()
{
  constexpr std::size_t kSampleCount = 16 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureStart =
    6 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureCount =
    6 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr double kInputBin = 24.0;
  constexpr float kValidRatio = 1.25f;
  constexpr double kExpectedBin = kInputBin * kValidRatio;
  const Samples input = makeTone(kInputBin, 4000.0, kSampleCount);
  const std::vector<float> invalidRatios = {
    0.0f,
    -1.0f,
    std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN(),
  };

  for (float ratio : invalidRatios) {
    const Samples output = runConfiguredEffect(
      input, [ratio](AudioEffectPitchShiftFFT &effect) {
        effect.setPitchRatio(kValidRatio);
        effect.setPitchRatio(ratio);
      });
    const int dominantBin =
      dominantIntegerBin(output, kMeasureStart, kMeasureCount);
    require(dominantBin == static_cast<int>(kExpectedBin),
            measured("bin after invalid pitch ratio", dominantBin, kExpectedBin));
  }
}

void testUnityReconstructsBroadbandInputAtDocumentedLatency()
{
  constexpr std::size_t kSampleCount = 32 * AudioEffectPitchShiftFFT::FFT_SIZE;
  Samples input(kSampleCount);
  std::mt19937 generator(0x5EED1234u);
  std::uniform_int_distribution<int> distribution(-6000, 6000);
  for (int16_t &sample : input) {
    sample = static_cast<int16_t>(distribution(generator));
  }

  const Samples output = runEffect(input);
  const std::size_t outputStart =
    kStreamingLatency + 6 * AudioEffectPitchShiftFFT::FFT_SIZE;
  const std::size_t count = 16 * AudioEffectPitchShiftFFT::FFT_SIZE;
  long double signalEnergy = 0.0;
  long double errorEnergy = 0.0;
  long double outputEnergy = 0.0;
  for (std::size_t i = outputStart; i < outputStart + count; ++i) {
    const long double expected = input[i - kStreamingLatency];
    const long double actual = output[i];
    signalEnergy += expected * expected;
    outputEnergy += actual * actual;
    const long double error = actual - expected;
    errorEnergy += error * error;
  }

  const double snr =
    10.0 * std::log10(static_cast<double>(signalEnergy / errorEnergy));
  const double gain = std::sqrt(static_cast<double>(outputEnergy / signalEnergy));
  require(snr > 60.0, measured("unity reconstruction SNR in dB", snr, 60.0));
  require(std::abs(gain - 1.0) < 0.002,
          measured("unity reconstruction gain", gain, 1.0));
}

void testUnityPreservesSignedFftEndpoints()
{
  constexpr std::size_t kSampleCount = 16 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr int16_t kDcLevel = -5000;
  const Samples dcInput(kSampleCount, kDcLevel);
  const Samples dcOutput = runEffect(dcInput);
  const std::size_t start = 6 * AudioEffectPitchShiftFFT::FFT_SIZE;
  const std::size_t count = 4 * AudioEffectPitchShiftFFT::FFT_SIZE;
  const double dcMean = mean(dcOutput, start, count);
  require(std::abs(dcMean - kDcLevel) < 2.0,
          measured("negative DC level", dcMean, kDcLevel));
  require(rms(dcOutput, start, count) > 4998.0,
          "negative DC was attenuated unexpectedly");

  Samples nyquistInput(kSampleCount);
  for (std::size_t i = 0; i < nyquistInput.size(); ++i) {
    nyquistInput[i] = (i & 1U) == 0 ? -4000 : 4000;
  }
  const Samples nyquistOutput = runEffect(nyquistInput);
  long double errorEnergy = 0.0;
  for (std::size_t i = start; i < start + count; ++i) {
    const long double error =
      nyquistOutput[i] - nyquistInput[i - kStreamingLatency];
    errorEnergy += error * error;
  }
  const double errorRms = std::sqrt(static_cast<double>(errorEnergy / count));
  require(errorRms < 2.0,
          measured("Nyquist reconstruction RMS error", errorRms, 0.0));
}

void checkShiftedTone(double inputBin,
                      float ratio,
                      double expectedBin,
                      double pitchTolerance)
{
  constexpr std::size_t kSampleCount = 24 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureStart =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureCount =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr double kAmplitude = 6000.0;

  const Samples input = makeTone(inputBin, kAmplitude, kSampleCount);
  const Samples output = runEffect(input, ratio);
  const double detected = peakBinNear(output,
                                      kMeasureStart,
                                      kMeasureCount,
                                      expectedBin,
                                      2.0,
                                      0.025);
  require(std::abs(detected - expectedBin) <= pitchTolerance,
          measured("shifted tone FFT bin", detected, expectedBin));

  const double outputRms = rms(output, kMeasureStart, kMeasureCount);
  require(outputRms > 0.20 * kAmplitude,
          measured("shifted tone RMS", outputRms, 0.20 * kAmplitude));

  const int dominantBin =
    dominantIntegerBin(output, kMeasureStart, kMeasureCount);
  require(std::abs(static_cast<double>(dominantBin) - expectedBin) <= 1.0,
          measured("dominant integer FFT bin", dominantBin, expectedBin));
}

void testPitchShiftsBinCenteredTones()
{
  checkShiftedTone(32.0, 1.25f, 40.0, 0.05);
  checkShiftedTone(40.0, 0.8f, 32.0, 0.05);
}

void testPitchShiftsOffBinTone()
{
  checkShiftedTone(50.3, 1.25f, 62.875, 0.20);
}

void testBinsShiftedPastNyquistAreDiscarded()
{
  constexpr std::size_t kSampleCount = 20 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureStart =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t kMeasureCount =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr double kAmplitude = 6000.0;
  const Samples input = makeTone(400.0, kAmplitude, kSampleCount);
  const Samples output = runEffect(input, 1.5f);
  const double outputRms = rms(output, kMeasureStart, kMeasureCount);
  require(outputRms < 2.0,
          measured("out-of-band shifted tone RMS", outputRms, 0.0));
}

} // namespace

int main()
{
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"reference FFT round trip", testReferenceFftRoundTrip},
    {"no input", testNoInputDoesNothing},
    {"silence", testSilenceIsExactlySilent},
    {"invalid pitch ratios", testInvalidRatiosRetainTheLastValidRatio},
    {"unity broadband reconstruction",
     testUnityReconstructsBroadbandInputAtDocumentedLatency},
    {"signed FFT endpoints", testUnityPreservesSignedFftEndpoints},
    {"bin-centered pitch shifts", testPitchShiftsBinCenteredTones},
    {"off-bin pitch shift", testPitchShiftsOffBinTone},
    {"above-Nyquist suppression", testBinsShiftedPastNyquistAreDiscarded},
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
