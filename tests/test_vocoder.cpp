#include "Vocoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
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

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::array<unsigned int, AudioEffectVocoder::NUM_BANDS + 1>
  kExpectedBandEdges = {
    2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25,
    31, 39, 49, 61, 76, 95, 118, 146, 181, 234,
  };

static_assert(AudioEffectVocoder::FFT_SIZE == 1024,
              "the vocoder tests require the production FFT size");
static_assert(AudioEffectVocoder::NUM_BINS ==
                AudioEffectVocoder::FFT_SIZE / 2 + 1,
              "a real FFT must expose DC through Nyquist");
static_assert(AudioEffectVocoder::FFT_SIZE ==
                4 * AudioEffectVocoder::HOP_SIZE,
              "the WOLA schedule requires four overlapping frames");
static_assert(AudioEffectVocoder::HOP_SIZE % AUDIO_BLOCK_SAMPLES == 0,
              "vocoder frames must be scheduled on audio-block boundaries");
static_assert((AudioEffectVocoder::OUT_FIFO_SIZE &
               (AudioEffectVocoder::OUT_FIFO_SIZE - 1U)) == 0,
              "the output FIFO wrap mask requires a power-of-two size");
static_assert(AudioEffectVocoder::OUT_FIFO_SIZE >=
                2 * AudioEffectVocoder::HOP_SIZE,
              "the output FIFO needs room for at least two hops");

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
  message << label << ": measured " << std::fixed << std::setprecision(6)
          << actual << ", expected " << expected;
  return message.str();
}

int16_t toAudioSample(double value)
{
  if (value >= 32767.0) {
    return 32767;
  }
  if (value <= -32768.0) {
    return -32768;
  }
  return static_cast<int16_t>(std::lround(value));
}

Samples makeTone(double fftBin, double amplitude, std::size_t sampleCount)
{
  Samples samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    samples[i] = toAudioSample(
      amplitude * std::sin(kTwoPi * fftBin * static_cast<double>(i) /
                           AudioEffectVocoder::FFT_SIZE));
  }
  return samples;
}

Samples makeDeterministicInput(std::size_t sampleCount)
{
  Samples samples(sampleCount);
  std::uint32_t state = 0x6D2B79F5U;
  for (int16_t &sample : samples) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    sample = static_cast<int16_t>(
      static_cast<int>(state % 16001U) - 8000);
  }
  return samples;
}

Samples negate(const Samples &input)
{
  Samples output(input.size());
  std::transform(input.begin(), input.end(), output.begin(), [](int16_t value) {
    return static_cast<int16_t>(-static_cast<int>(value));
  });
  return output;
}

Samples streamEffect(AudioEffectVocoder &effect,
                     const Samples &input,
                     bool forceAllocationFailure = false)
{
  require(input.size() % AUDIO_BLOCK_SAMPLES == 0,
          "test input must contain whole Teensy audio blocks");

  AudioStream::resetTestState();
  AudioStream::setTestAllocationFailure(forceAllocationFailure);
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

    require(AudioStream::lastTransmitted(0) == &block,
            "update() did not transmit the writable input block");
    const audio_block_t *snapshot = AudioStream::transmittedSnapshot(0);
    require(snapshot != nullptr,
            "update() did not leave an inspectable output block");
    output.insert(output.end(),
                  std::begin(snapshot->data),
                  std::end(snapshot->data));
    require(AudioStream::ownershipErrorCount() == 0,
            "update() violated AudioStream block ownership");
    require(!AudioStream::hasOutstandingBlocks(),
            "update() leaked an AudioStream block");
  }

  const std::size_t expectedBlocks = input.size() / AUDIO_BLOCK_SAMPLES;
  require(AudioStream::channelTransmissionCount(0) == expectedBlocks,
          "not every vocoder block was transmitted");
  require(AudioStream::channelTransmissionCount(1) == 0,
          "the mono vocoder unexpectedly transmitted a second channel");
  require(AudioStream::transmissionCount() == expectedBlocks,
          "the vocoder transmitted an unexpected number of blocks");
  require(AudioStream::releaseCount() == expectedBlocks,
          "the vocoder released an unexpected number of blocks");
  AudioStream::resetTestState();
  return output;
}

double meanSquare(const Samples &samples,
                  std::size_t start,
                  std::size_t count)
{
  require(start + count <= samples.size(),
          "mean-square interval is out of range");
  long double energy = 0.0;
  for (std::size_t i = start; i < start + count; ++i) {
    const long double value = samples[i];
    energy += value * value;
  }
  return static_cast<double>(energy / static_cast<long double>(count));
}

double rms(const Samples &samples, std::size_t start, std::size_t count)
{
  return std::sqrt(meanSquare(samples, start, count));
}

double spectralPowerAt(const Samples &samples,
                       std::size_t start,
                       std::size_t count,
                       std::size_t frequencyBin)
{
  require(start + count <= samples.size(),
          "spectral interval is out of range");
  require(frequencyBin > 0 && frequencyBin < count / 2,
          "spectral bin must exclude DC and Nyquist");

  const double angle = -kTwoPi * static_cast<double>(frequencyBin) /
                       static_cast<double>(count);
  const std::complex<long double> rotation(std::cos(angle), std::sin(angle));
  std::complex<long double> oscillator(1.0L, 0.0L);
  std::complex<long double> projection(0.0L, 0.0L);
  for (std::size_t i = start; i < start + count; ++i) {
    projection += static_cast<long double>(samples[i]) * oscillator;
    oscillator *= rotation;
  }

  const long double scale = static_cast<long double>(count);
  return static_cast<double>(2.0L * std::norm(projection) /
                             (scale * scale));
}

double spectralPowerInBaseBinRange(const Samples &samples,
                                   std::size_t start,
                                   std::size_t count,
                                   unsigned int firstBaseBin,
                                   unsigned int lastBaseBin,
                                   unsigned int paddingBaseBins = 0)
{
  require(count % AudioEffectVocoder::FFT_SIZE == 0,
          "spectral interval must contain whole vocoder FFTs");
  require(firstBaseBin < lastBaseBin,
          "spectral range must contain at least one bin");

  const std::size_t resolution =
    count / static_cast<std::size_t>(AudioEffectVocoder::FFT_SIZE);
  const unsigned int paddedFirst =
    firstBaseBin > paddingBaseBins ? firstBaseBin - paddingBaseBins : 0U;
  const unsigned int paddedLast = lastBaseBin + paddingBaseBins;
  const std::size_t firstFineBin = std::max<std::size_t>(
    1U, static_cast<std::size_t>(paddedFirst) * resolution);
  const std::size_t lastFineBin = std::min<std::size_t>(
    count / 2U,
    static_cast<std::size_t>(paddedLast) * resolution);

  double power = 0.0;
  for (std::size_t bin = firstFineBin; bin < lastFineBin; ++bin) {
    power += spectralPowerAt(samples, start, count, bin);
  }
  return power;
}

double harmonicPower(const Samples &samples,
                     std::size_t start,
                     std::size_t count,
                     unsigned int fundamentalBaseBin,
                     unsigned int firstBaseBin,
                     unsigned int lastBaseBin,
                     unsigned int excludedFundamentalBaseBin = 0)
{
  require(count % AudioEffectVocoder::FFT_SIZE == 0,
          "harmonic interval must contain whole vocoder FFTs");
  const std::size_t resolution =
    count / static_cast<std::size_t>(AudioEffectVocoder::FFT_SIZE);
  constexpr std::size_t radius = 2;
  double power = 0.0;

  for (unsigned int bin = fundamentalBaseBin; bin < lastBaseBin;
       bin += fundamentalBaseBin) {
    if (bin < firstBaseBin ||
        (excludedFundamentalBaseBin != 0U &&
         bin % excludedFundamentalBaseBin == 0U)) {
      continue;
    }
    const std::size_t centre = static_cast<std::size_t>(bin) * resolution;
    for (std::size_t fineBin = centre - radius;
         fineBin <= centre + radius;
         ++fineBin) {
      power += spectralPowerAt(samples, start, count, fineBin);
    }
  }
  return power;
}

void testTopologyAndDefaults()
{
  for (std::size_t i = 0; i < kExpectedBandEdges.size(); ++i) {
    require(AudioEffectVocoder::BAND_EDGE_BINS[i] == kExpectedBandEdges[i],
            "vocoder band edge does not match the tested analysis bank");
    if (i != 0) {
      require(AudioEffectVocoder::BAND_EDGE_BINS[i - 1] <
                  AudioEffectVocoder::BAND_EDGE_BINS[i],
              "vocoder band edges are not strictly increasing");
    }
  }
  require(AudioEffectVocoder::BAND_EDGE_BINS[0] > 1,
          "the first speech band must exclude DC and bin one");
  require(AudioEffectVocoder::BAND_EDGE_BINS[AudioEffectVocoder::NUM_BANDS] <
            AudioEffectVocoder::NUM_BINS - 1,
          "the speech bank must exclude Nyquist");

  require(AudioEffectVocoder::DEFAULT_CARRIER_HZ == 110.0f,
          "the default carrier pitch changed unexpectedly");
  require(AudioEffectVocoder::DEFAULT_ATTACK_MS == 8.0f,
          "the default envelope attack changed unexpectedly");
  require(AudioEffectVocoder::DEFAULT_RELEASE_MS == 80.0f,
          "the default envelope release changed unexpectedly");
  require(AudioEffectVocoder::DEFAULT_NOISE_MIX == 0.15f,
          "the default carrier noise mix changed unexpectedly");
  require(AudioEffectVocoder::MIN_GATE_DBFS == -96.0f &&
            AudioEffectVocoder::MAX_GATE_DBFS == 0.0f,
          "the documented modulator gate range changed unexpectedly");
  require(AudioEffectVocoder::DEFAULT_GATE_THRESHOLD_DBFS == -45.0f,
          "the default modulator gate threshold changed unexpectedly");
}

void testNoInputDoesNothing()
{
  AudioStream::resetTestState();
  AudioEffectVocoder effect;
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
  AudioEffectVocoder effect;
  const Samples silence(12U * AudioEffectVocoder::FFT_SIZE, 0);
  const Samples output = streamEffect(effect, silence);
  require(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 0; }),
          "the internal carrier leaked through a silent modulator");
}

void testPipelinePrimesSafelyAndProducesAudio()
{
  AudioEffectVocoder effect;
  require(effect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
          "valid zero-time envelope configuration was rejected");
  const Samples output = streamEffect(
    effect, makeTone(54.0, 6000.0, 12U * AudioEffectVocoder::FFT_SIZE));
  require(std::all_of(output.begin(),
                      output.begin() + AUDIO_BLOCK_SAMPLES,
                      [](int16_t sample) { return sample == 0; }),
          "the output FIFO was not silent before its first complete hop");
  require(std::any_of(output.begin() + AUDIO_BLOCK_SAMPLES, output.end(),
                      [](int16_t sample) { return sample != 0; }),
          "the vocoder never produced audio after priming");
}

void testConfigurationValidationIsAtomic()
{
  constexpr float carrier = 173.0f;
  constexpr float attack = 11.0f;
  constexpr float release = 97.0f;
  constexpr float noise = 0.37f;
  constexpr float gate = -72.0f;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const std::array<std::array<float, 5>, 22> invalid = {{
    {{nan, attack, release, noise, gate}},
    {{infinity, attack, release, noise, gate}},
    {{39.99f, attack, release, noise, gate}},
    {{1000.01f, attack, release, noise, gate}},
    {{carrier, nan, release, noise, gate}},
    {{carrier, infinity, release, noise, gate}},
    {{carrier, -0.01f, release, noise, gate}},
    {{carrier, 1000.01f, release, noise, gate}},
    {{carrier, attack, nan, noise, gate}},
    {{carrier, attack, infinity, noise, gate}},
    {{carrier, attack, -0.01f, noise, gate}},
    {{carrier, attack, 5000.01f, noise, gate}},
    {{carrier, attack, release, nan, gate}},
    {{carrier, attack, release, infinity, gate}},
    {{carrier, attack, release, -0.01f, gate}},
    {{carrier, attack, release, 1.01f, gate}},
    {{carrier, attack, release, noise, nan}},
    {{carrier, attack, release, noise, infinity}},
    {{carrier, attack, release, noise, -96.01f}},
    {{carrier, attack, release, noise, 0.01f}},
    {{std::numeric_limits<float>::max(), attack, release, noise, gate}},
    {{carrier, attack, std::numeric_limits<float>::max(), noise, gate}},
  }};

  AudioEffectVocoder endpoints;
  require(endpoints.configure(40.0f, 0.0f, 0.0f, 0.0f, -96.0f),
          "minimum valid vocoder controls were rejected");
  require(endpoints.configure(1000.0f, 1000.0f, 5000.0f, 1.0f, 0.0f),
          "maximum valid vocoder controls were rejected");

  AudioEffectVocoder candidate;
  AudioEffectVocoder reference;
  require(candidate.configure(carrier, attack, release, noise, gate) &&
            reference.configure(carrier, attack, release, noise, gate),
          "valid baseline vocoder controls were rejected");
  for (const auto &values : invalid) {
    require(!candidate.configure(
              values[0], values[1], values[2], values[3], values[4]),
            "an invalid vocoder configuration was accepted");
  }

  const Samples input = makeDeterministicInput(
    16U * AudioEffectVocoder::FFT_SIZE);
  const Samples candidateOutput = streamEffect(candidate, input);
  const Samples referenceOutput = streamEffect(reference, input);
  require(candidateOutput == referenceOutput,
          "a rejected configuration changed later vocoder output");
}

void testResetRestoresAllStreamingState()
{
  constexpr float carrier = 227.0f;
  constexpr float attack = 4.0f;
  constexpr float release = 123.0f;
  constexpr float noise = 0.63f;
  AudioEffectVocoder resetEffect;
  AudioEffectVocoder freshEffect;
  require(resetEffect.configure(carrier, attack, release, noise, -96.0f) &&
            freshEffect.configure(carrier, attack, release, noise, -96.0f),
          "valid reset-test controls were rejected");

  streamEffect(resetEffect,
               makeDeterministicInput(12U * AudioEffectVocoder::FFT_SIZE));
  resetEffect.reset();

  const Samples probe = makeTone(
    54.0, 5000.0, 16U * AudioEffectVocoder::FFT_SIZE);
  const Samples afterReset = streamEffect(resetEffect, probe);
  const Samples fresh = streamEffect(freshEffect, probe);
  require(afterReset == fresh,
          "reset did not restore FFT, envelope, carrier, FIFO, and PRNG state");
}

void testMonoEffectDoesNotDependOnAudioMemoryAllocation()
{
  AudioEffectVocoder normal;
  AudioEffectVocoder starved;
  require(normal.configure(191.0f, 3.0f, 70.0f, 0.45f, -96.0f) &&
            starved.configure(191.0f, 3.0f, 70.0f, 0.45f, -96.0f),
          "valid allocation-independence controls were rejected");
  const Samples input = makeDeterministicInput(
    12U * AudioEffectVocoder::FFT_SIZE);
  const Samples normalOutput = streamEffect(normal, input);
  const Samples starvedOutput = streamEffect(starved, input, true);
  require(starvedOutput == normalOutput,
          "the in-place vocoder unexpectedly depends on AudioMemory allocation");
}

void testDetectorIsPolarityInvariantAndLinear()
{
  constexpr std::size_t sampleCount =
    16U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    4U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;
  const Samples positive = makeTone(54.0, 2400.0, sampleCount);

  AudioEffectVocoder positiveEffect;
  AudioEffectVocoder negativeEffect;
  require(positiveEffect.configure(
            173.0f, 0.0f, 0.0f, 1.0f, -96.0f) &&
            negativeEffect.configure(
              173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
          "valid polarity-test controls were rejected");
  const Samples positiveOutput = streamEffect(positiveEffect, positive);
  const Samples negativeOutput = streamEffect(negativeEffect, negate(positive));
  require(positiveOutput == negativeOutput,
          "RMS envelope detection changed when input polarity was inverted");

  AudioEffectVocoder quietEffect;
  AudioEffectVocoder loudEffect;
  require(quietEffect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f) &&
            loudEffect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
          "valid linearity-test controls were rejected");
  const Samples quietOutput = streamEffect(
    quietEffect, makeTone(54.0, 1200.0, sampleCount));
  const Samples loudOutput = streamEffect(
    loudEffect, makeTone(54.0, 2400.0, sampleCount));
  const double quietRms = rms(quietOutput, measureStart, measureCount);
  const double loudRms = rms(loudOutput, measureStart, measureCount);
  require(quietRms > 100.0,
          measured("quiet vocoder RMS", quietRms, 100.0));
  const double ratio = loudRms / quietRms;
  require(ratio > 1.99 && ratio < 2.01,
          measured("two-to-one modulator amplitude transfer", ratio, 2.0));
}

void testEveryAnalysisBandCanDriveTheCarrier()
{
  constexpr std::size_t sampleCount =
    8U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    2U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;
  double minimumRms = std::numeric_limits<double>::infinity();
  double maximumRms = 0.0;

  for (std::size_t band = 0; band < AudioEffectVocoder::NUM_BANDS; ++band) {
    const unsigned int first = kExpectedBandEdges[band];
    const unsigned int last = kExpectedBandEdges[band + 1];
    const double centre =
      0.5 * static_cast<double>(first + last - 1U);
    AudioEffectVocoder effect;
    require(effect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
            "valid band-coverage controls were rejected");
    const Samples output = streamEffect(
      effect, makeTone(centre, 6000.0, sampleCount));
    const double outputRms = rms(output, measureStart, measureCount);
    minimumRms = std::min(minimumRms, outputRms);
    maximumRms = std::max(maximumRms, outputRms);
    require(outputRms > 1000.0,
            measured("output RMS for analysis band " + std::to_string(band),
                     outputRms, 1000.0));
  }
  require(minimumRms > 0.65 * maximumRms,
          measured("minimum/maximum analysis-band RMS",
                   minimumRms / maximumRms, 0.65));
}

void testSpectralEnvelopeRoutesToTheSelectedBand()
{
  constexpr std::array<std::size_t, 3> selectedBands = {7, 13, 18};
  constexpr std::size_t sampleCount =
    16U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    8U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;

  for (const std::size_t band : selectedBands) {
    const unsigned int first = kExpectedBandEdges[band];
    const unsigned int last = kExpectedBandEdges[band + 1];
    const double centre =
      0.5 * static_cast<double>(first + last - 1U);
    AudioEffectVocoder effect;
    require(effect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
            "valid spectral-routing controls were rejected");
    const Samples output = streamEffect(
      effect, makeTone(centre, 6000.0, sampleCount));

    const double totalPower = meanSquare(output, measureStart, measureCount);
    require(totalPower > 100.0,
            measured("material spectral-routing power", totalPower, 100.0));
    const double selectedPower = spectralPowerInBaseBinRange(
      output, measureStart, measureCount, first, last, 2);
    const double fraction = selectedPower / totalPower;
    require(fraction > 0.97,
            measured("power retained near selected band " +
                       std::to_string(band),
                     fraction, 0.97));
  }
}

void testDcNyquistAndOutOfRangeToneAreRejected()
{
  constexpr std::size_t sampleCount =
    16U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    4U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;
  std::array<Samples, 3> excluded = {
    Samples(sampleCount, 6000),
    Samples(sampleCount),
    makeTone(300.0, 6000.0, sampleCount),
  };
  for (std::size_t i = 0; i < sampleCount; ++i) {
    excluded[1][i] = (i & 1U) == 0U ? 6000 : -6000;
  }

  for (std::size_t signal = 0; signal < excluded.size(); ++signal) {
    AudioEffectVocoder effect;
    require(effect.configure(173.0f, 0.0f, 0.0f, 1.0f, -96.0f),
            "valid rejection-test controls were rejected");
    const Samples output = streamEffect(effect, excluded[signal]);
    const double residual = rms(output, measureStart, measureCount);
    require(residual < 0.25,
            measured("excluded-spectrum residual " +
                       std::to_string(signal),
                     residual, 0.0));
  }
}

void testModulatorNoiseGateSuppressesOnlySubthresholdInput()
{
  constexpr std::size_t sampleCount =
    16U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    4U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;
  // A 100-count peak sine is approximately -53 dBFS RMS: comfortably below
  // the default -45 dBFS gate, but comfortably above a -70 dBFS threshold.
  const Samples lowTone = makeTone(54.0, 100.0, sampleCount);
  const Samples speechLevelTone = makeTone(54.0, 6000.0, sampleCount);

  AudioEffectVocoder closedGate;
  AudioEffectVocoder openGate;
  AudioEffectVocoder speechGate;
  require(closedGate.configure(173.0f, 0.0f, 0.0f, 1.0f, -45.0f) &&
            openGate.configure(173.0f, 0.0f, 0.0f, 1.0f, -70.0f) &&
            speechGate.configure(173.0f, 0.0f, 0.0f, 1.0f, -45.0f),
          "valid gate-test controls were rejected");

  const Samples gatedOutput = streamEffect(closedGate, lowTone);
  require(std::all_of(gatedOutput.begin(), gatedOutput.end(),
                      [](int16_t sample) { return sample == 0; }),
          "a subthreshold valid-band tone leaked through the noise gate");

  const Samples lowThresholdOutput = streamEffect(openGate, lowTone);
  const double lowThresholdRms =
    rms(lowThresholdOutput, measureStart, measureCount);
  require(lowThresholdRms > 10.0,
          measured("low tone through lower gate threshold",
                   lowThresholdRms, 10.0));

  const Samples speechOutput = streamEffect(speechGate, speechLevelTone);
  const double speechRms = rms(speechOutput, measureStart, measureCount);
  require(speechRms > 1000.0,
          measured("speech-level tone through default gate",
                   speechRms, 1000.0));
}

void testCarrierPitchAndNoiseMixShapeTheSpectrum()
{
  constexpr std::size_t band = 18;
  constexpr unsigned int first = kExpectedBandEdges[band];
  constexpr unsigned int last = kExpectedBandEdges[band + 1];
  constexpr std::size_t sampleCount =
    20U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureCount =
    8U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t measureStart = sampleCount - measureCount;
  const double centre = 0.5 * static_cast<double>(first + last - 1U);
  const Samples input = makeTone(centre, 6000.0, sampleCount);
  const float binHz = AUDIO_SAMPLE_RATE_EXACT /
                      static_cast<float>(AudioEffectVocoder::FFT_SIZE);

  const auto render = [&input](float frequency, float noiseMix) {
    AudioEffectVocoder effect;
    require(effect.configure(frequency, 0.0f, 0.0f, noiseMix, -96.0f),
            "valid carrier-spectrum controls were rejected");
    return streamEffect(effect, input);
  };

  const Samples fifthBinCarrier = render(5.0f * binHz, 0.0f);
  const Samples seventhBinCarrier = render(7.0f * binHz, 0.0f);
  const Samples noiseCarrier = render(5.0f * binHz, 1.0f);

  const double fifthCorrect = harmonicPower(
    fifthBinCarrier, measureStart, measureCount, 5, first, last, 7);
  const double fifthWrong = harmonicPower(
    fifthBinCarrier, measureStart, measureCount, 7, first, last, 5);
  const double seventhCorrect = harmonicPower(
    seventhBinCarrier, measureStart, measureCount, 7, first, last, 5);
  const double seventhWrong = harmonicPower(
    seventhBinCarrier, measureStart, measureCount, 5, first, last, 7);
  require(fifthCorrect > 100.0 * fifthWrong,
          measured("bin-five carrier harmonic discrimination",
                   fifthCorrect / std::max(fifthWrong, 1.0e-12), 100.0));
  require(seventhCorrect > 100.0 * seventhWrong,
          measured("bin-seven carrier harmonic discrimination",
                   seventhCorrect / std::max(seventhWrong, 1.0e-12), 100.0));

  const double sawHarmonics = harmonicPower(
    fifthBinCarrier, measureStart, measureCount, 5, first, last);
  const double noiseAtSawHarmonics = harmonicPower(
    noiseCarrier, measureStart, measureCount, 5, first, last);
  const double sawPower = meanSquare(
    fifthBinCarrier, measureStart, measureCount);
  const double noisePower = meanSquare(noiseCarrier, measureStart, measureCount);
  require(sawPower > 0.0 && noisePower > 0.0,
          "a carrier endpoint produced no output power");
  const double sawConcentration = sawHarmonics / sawPower;
  const double noiseConcentration = noiseAtSawHarmonics / noisePower;
  require(sawConcentration > 5.0 * noiseConcentration,
          measured("saw/noise harmonic-concentration ratio",
                   sawConcentration /
                     std::max(noiseConcentration, 1.0e-12),
                   5.0));
}

void testAttackAndReleaseControlsChangeEnvelopeTiming()
{
  constexpr std::size_t toneSamples =
    48U * AudioEffectVocoder::FFT_SIZE;
  constexpr std::size_t tailSamples =
    80U * AudioEffectVocoder::FFT_SIZE;
  Samples input = makeTone(54.0, 6000.0, toneSamples);
  input.resize(toneSamples + tailSamples, 0);

  AudioEffectVocoder instantEffect;
  AudioEffectVocoder slowEffect;
  require(instantEffect.configure(
            173.0f, 0.0f, 0.0f, 1.0f, -96.0f) &&
            slowEffect.configure(
              173.0f, 150.0f, 300.0f, 1.0f, -96.0f),
          "valid envelope-timing controls were rejected");
  const Samples instant = streamEffect(instantEffect, input);
  const Samples slow = streamEffect(slowEffect, input);

  constexpr std::size_t earlyStart = 4U * AudioEffectVocoder::HOP_SIZE;
  constexpr std::size_t earlyCount = 8U * AudioEffectVocoder::HOP_SIZE;
  constexpr std::size_t steadyCount = 8U * AudioEffectVocoder::HOP_SIZE;
  constexpr std::size_t steadyStart = toneSamples -
                                      12U * AudioEffectVocoder::HOP_SIZE;
  const double instantEarly = rms(instant, earlyStart, earlyCount);
  const double slowEarly = rms(slow, earlyStart, earlyCount);
  const double instantSteady = rms(instant, steadyStart, steadyCount);
  const double slowSteady = rms(slow, steadyStart, steadyCount);
  require(instantSteady > 10.0 && slowSteady > 10.0,
          "envelope-timing test did not reach material steady output");
  const double instantAttackFraction = instantEarly / instantSteady;
  const double slowAttackFraction = slowEarly / slowSteady;
  require(instantAttackFraction > 3.0 * slowAttackFraction,
          measured("instant/slow attack contrast",
                   instantAttackFraction /
                     std::max(slowAttackFraction, 1.0e-12),
                   3.0));

  const std::size_t earlyTailStart =
    toneSamples + static_cast<std::size_t>(0.15f * AUDIO_SAMPLE_RATE_EXACT);
  const std::size_t lateTailStart =
    toneSamples + static_cast<std::size_t>(0.80f * AUDIO_SAMPLE_RATE_EXACT);
  const std::size_t tailWindow =
    static_cast<std::size_t>(0.10f * AUDIO_SAMPLE_RATE_EXACT);
  const double instantTail = rms(instant, earlyTailStart, tailWindow);
  const double slowEarlyTail = rms(slow, earlyTailStart, tailWindow);
  const double slowLateTail = rms(slow, lateTailStart, tailWindow);
  require(slowEarlyTail > 0.2 * slowSteady,
          measured("slow release tail/steady RMS",
                   slowEarlyTail / slowSteady, 0.2));
  require(slowEarlyTail > 100.0 * std::max(instantTail, 1.0),
          measured("slow/instant release-tail RMS",
                   slowEarlyTail / std::max(instantTail, 1.0), 100.0));
  require(slowEarlyTail > 5.0 * slowLateTail,
          measured("release decay across the tail",
                   slowEarlyTail / std::max(slowLateTail, 1.0e-12), 5.0));
}

} // namespace

int main()
{
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"topology and defaults", testTopologyAndDefaults},
    {"no input", testNoInputDoesNothing},
    {"silence gating", testSilenceIsExactlySilent},
    {"safe pipeline priming", testPipelinePrimesSafelyAndProducesAudio},
    {"atomic configuration validation", testConfigurationValidationIsAtomic},
    {"complete state reset", testResetRestoresAllStreamingState},
    {"no AudioMemory dependency",
     testMonoEffectDoesNotDependOnAudioMemoryAllocation},
    {"RMS detector polarity and scale",
     testDetectorIsPolarityInvariantAndLinear},
    {"all analysis bands", testEveryAnalysisBandCanDriveTheCarrier},
    {"spectral-envelope routing", testSpectralEnvelopeRoutesToTheSelectedBand},
    {"out-of-band rejection", testDcNyquistAndOutOfRangeToneAreRejected},
    {"modulator noise gate",
     testModulatorNoiseGateSuppressesOnlySubthresholdInput},
    {"carrier pitch and noise spectrum",
     testCarrierPitchAndNoiseMixShapeTheSpectrum},
    {"attack and release timing",
     testAttackAndReleaseControlsChangeEnvelopeTiming},
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
