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

void testReferenceRealFftPackingAndRoundTrip()
{
  constexpr std::size_t size = AudioEffectPitchShiftFFT::FFT_SIZE;
  arm_rfft_fast_instance_f32 instance{};
  require(arm_rfft_fast_init_f32(&instance, size) == ARM_MATH_SUCCESS,
          "reference RFFT initialization failed");

  std::vector<float> input(size, 0.0f);
  std::vector<float> spectrum(size, 0.0f);
  input[0] = 1.0f;
  arm_rfft_fast_f32(&instance, input.data(), spectrum.data(), 0);
  require(std::abs(spectrum[0] - 1.0f) < 2.0e-5f,
          "RFFT impulse DC coefficient is incorrect");
  require(std::abs(spectrum[1] - 1.0f) < 2.0e-5f,
          "RFFT impulse Nyquist coefficient is incorrect");
  for (std::size_t bin = 1; bin < size / 2; ++bin) {
    require(std::abs(spectrum[2 * bin] - 1.0f) < 2.0e-5f &&
              std::abs(spectrum[2 * bin + 1]) < 2.0e-5f,
            "RFFT impulse spectrum packing is incorrect");
  }

  std::fill(input.begin(), input.end(), -0.25f);
  arm_rfft_fast_f32(&instance, input.data(), spectrum.data(), 0);
  require(std::abs(spectrum[0] + 0.25f * size) < 2.0e-3f,
          "RFFT lost the sign of negative DC");
  require(std::abs(spectrum[1]) < 2.0e-3f,
          "RFFT negative DC leaked into Nyquist");

  for (std::size_t i = 0; i < size; ++i) {
    input[i] = (i & 1U) == 0 ? -0.25f : 0.25f;
  }
  arm_rfft_fast_f32(&instance, input.data(), spectrum.data(), 0);
  require(std::abs(spectrum[0]) < 2.0e-3f,
          "RFFT Nyquist input leaked into DC");
  require(std::abs(spectrum[1] + 0.25f * size) < 2.0e-3f,
          "RFFT lost the sign of the Nyquist coefficient");

  for (std::size_t i = 0; i < size; ++i) {
    input[i] = static_cast<float>(
      0.5 * std::cos(kTwoPi * 113.0 * static_cast<double>(i) / size) +
      0.25 * std::sin(kTwoPi * 23.0 * static_cast<double>(i) / size));
  }
  const std::vector<float> original = input;
  arm_rfft_fast_f32(&instance, input.data(), spectrum.data(), 0);
  require(std::abs(spectrum[2 * 113] - 0.25f * size) < 0.02f &&
            std::abs(spectrum[2 * 113 + 1]) < 0.02f,
          "RFFT cosine-bin packing is incorrect");
  require(std::abs(spectrum[2 * 23]) < 0.02f &&
            std::abs(spectrum[2 * 23 + 1] + 0.125f * size) < 0.02f,
          "RFFT sine-bin packing or phase sign is incorrect");

  std::vector<float> reconstructed(size, 0.0f);
  arm_rfft_fast_f32(&instance, spectrum.data(), reconstructed.data(), 1);
  double maxError = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    maxError = std::max(
      maxError,
      std::abs(static_cast<double>(reconstructed[i] - original[i])));
  }
  require(maxError < 2.0e-5,
          measured("reference RFFT round-trip error", maxError, 0.0));
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

void testResetDiscardsBufferedAudioAndPhaseHistory()
{
  constexpr std::size_t sampleCount =
    12 * AudioEffectPitchShiftFFT::FFT_SIZE;
  const Samples tone = makeTone(32.0, 6000.0, sampleCount);

  AudioStream::resetTestState();
  AudioEffectPitchShiftFFT effect;
  effect.setPitchRatio(1.25f);
  bool producedNonzero = false;
  for (std::size_t offset = 0; offset < tone.size();
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    std::copy_n(tone.begin() + static_cast<std::ptrdiff_t>(offset),
                AUDIO_BLOCK_SAMPLES, block.data);
    AudioStream::queueTestInput(&block);
    effect.update();
    producedNonzero = producedNonzero ||
      std::any_of(std::begin(block.data), std::end(block.data),
                  [](int16_t sample) { return sample != 0; });
  }
  require(producedNonzero, "reset test did not prime the effect with audio");

  effect.reset();
  for (std::size_t offset = 0; offset < sampleCount;
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    AudioStream::queueTestInput(&block);
    effect.update();
    require(std::all_of(std::begin(block.data), std::end(block.data),
                        [](int16_t sample) { return sample == 0; }),
            "reset left buffered audio or phase energy in the stream");
  }
  AudioStream::resetTestState();
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
  long double dcErrorEnergy = 0.0;
  for (std::size_t i = start; i < start + count; ++i) {
    const long double error =
      dcOutput[i] - dcInput[i - kStreamingLatency];
    dcErrorEnergy += error * error;
  }
  const double dcErrorRms =
    std::sqrt(static_cast<double>(dcErrorEnergy / count));
  require(dcErrorRms < 2.0,
          measured("negative DC reconstruction RMS error", dcErrorRms, 0.0));

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
                      double pitchTolerance,
                      double minimumRmsFraction)
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
  require(outputRms > minimumRmsFraction * kAmplitude,
          measured("shifted tone RMS", outputRms,
                   minimumRmsFraction * kAmplitude));

  for (std::size_t offset = 0; offset < kMeasureCount;
       offset += AudioEffectPitchShiftFFT::FFT_SIZE) {
    const int dominantBin = dominantIntegerBin(
      output,
      kMeasureStart + offset,
      AudioEffectPitchShiftFFT::FFT_SIZE);
    require(std::abs(static_cast<double>(dominantBin) - expectedBin) <= 1.0,
            measured("dominant integer FFT bin", dominantBin, expectedBin));
  }
}

void testPitchShiftsBinCenteredTones()
{
  // A bin-centred sine has an input RMS of amplitude/sqrt(2). Leave margin
  // for phase-vocoder/window loss, but reject the multi-decibel attenuation
  // that can otherwise hide behind a mere "non-silent output" check.
  checkShiftedTone(32.0, 1.25f, 40.0, 0.05, 0.60);
  checkShiftedTone(40.0, 0.8f, 32.0, 0.05, 0.60);
}

void testPitchShiftsOffBinTone()
{
  checkShiftedTone(50.3, 1.25f, 62.875, 0.20, 0.45);
}

void testInactiveSynthesisBinsDoNotRetainUnrelatedPhase()
{
  constexpr std::size_t toneSamples =
    24 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t silenceSamples =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr double amplitude = 6000.0;

  AudioStream::resetTestState();
  AudioEffectPitchShiftFFT effect;
  const auto stream = [&effect](const Samples &input) {
    Samples output;
    output.reserve(input.size());
    for (std::size_t offset = 0; offset < input.size();
         offset += AUDIO_BLOCK_SAMPLES) {
      audio_block_t block{};
      std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                  AUDIO_BLOCK_SAMPLES, block.data);
      AudioStream::queueTestInput(&block);
      effect.update();
      output.insert(output.end(), std::begin(block.data), std::end(block.data));
    }
    return output;
  };

  effect.setPitchRatio(1.25f);
  stream(makeTone(32.0, amplitude, toneSamples));
  stream(Samples(silenceSamples, 0));
  effect.setPitchRatio(0.8f);
  const Samples downTone = makeTone(40.0, amplitude, toneSamples);
  const Samples downshifted = stream(downTone);

  constexpr std::size_t measureStart =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  constexpr std::size_t measureCount =
    8 * AudioEffectPitchShiftFFT::FFT_SIZE;
  const double outputRms = rms(downshifted, measureStart, measureCount);
  require(outputRms > 0.60 * amplitude,
          measured("downshift RMS after an unrelated prior signal",
                   outputRms, 0.60 * amplitude));
  const double detected = peakBinNear(
    downshifted, measureStart, measureCount, 32.0, 2.0, 0.025);
  require(std::abs(detected - 32.0) <= 0.05,
          measured("downshift bin after an unrelated prior signal",
                   detected, 32.0));

  const Samples freshDownshift = runEffect(downTone, 0.8f);
  for (std::size_t offset = 0; offset < measureCount;
       offset += AudioEffectPitchShiftFFT::FFT_SIZE) {
    const double transitionedSlice = rms(
      downshifted, measureStart + offset, AudioEffectPitchShiftFFT::FFT_SIZE);
    const double freshSlice = rms(
      freshDownshift, measureStart + offset, AudioEffectPitchShiftFFT::FFT_SIZE);
    require(std::abs(transitionedSlice / freshSlice - 1.0) < 0.01,
            measured("downshift slice gain after prior signal",
                     transitionedSlice / freshSlice, 1.0));
  }
  AudioStream::resetTestState();
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
    {"reference real FFT packing and round trip",
     testReferenceRealFftPackingAndRoundTrip},
    {"no input", testNoInputDoesNothing},
    {"stream reset", testResetDiscardsBufferedAudioAndPhaseHistory},
    {"silence", testSilenceIsExactlySilent},
    {"invalid pitch ratios", testInvalidRatiosRetainTheLastValidRatio},
    {"unity broadband reconstruction",
     testUnityReconstructsBroadbandInputAtDocumentedLatency},
    {"signed FFT endpoints", testUnityPreservesSignedFftEndpoints},
    {"bin-centered pitch shifts", testPitchShiftsBinCenteredTones},
    {"off-bin pitch shift", testPitchShiftsOffBinTone},
    {"inactive synthesis-bin phase reset",
     testInactiveSynthesisBinsDoNotRetainUnrelatedPhase},
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
