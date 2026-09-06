#include "Vocoder.h"

#include <VoiceChangerAudioSample.h>
#include <arm_common_tables.h>
#include <arm_const_structs.h>

#include <math.h>
#include <string.h>

namespace {

constexpr float kTwoPi = 6.283185307179586476925286766559f;
constexpr float kCarrierAmplitude = 16384.0f;
constexpr float kMaximumBandGain = 64.0f;
constexpr uint32_t kNoiseSeed = 0x6D2B79F5U;

static_assert(AudioEffectVocoder::FFT_SIZE == 1024,
              "the fixed CMSIS RFFT initializer requires FFT_SIZE == 1024");
static_assert(AudioEffectVocoder::FFT_SIZE ==
                4 * AudioEffectVocoder::HOP_SIZE,
              "the WOLA gain assumes four overlapping Hann windows");
static_assert(AudioEffectVocoder::HOP_SIZE % AUDIO_BLOCK_SAMPLES == 0,
              "vocoder frames must align with Teensy audio blocks");
static_assert((AudioEffectVocoder::OUT_FIFO_SIZE &
               (AudioEffectVocoder::OUT_FIFO_SIZE - 1)) == 0,
              "the output FIFO size must be a power of two");
static_assert(AudioEffectVocoder::BAND_EDGE_BINS[
                AudioEffectVocoder::NUM_BANDS] <=
                AudioEffectVocoder::NUM_BINS - 1,
              "vocoder bands must end below the real-only Nyquist bin");

inline float magnitudeSquared(const float *spectrum, int bin)
{
  const float real = spectrum[2 * bin];
  const float imaginary = spectrum[2 * bin + 1];
  return real * real + imaginary * imaginary;
}

} // namespace

/**
 * Construct a silent vocoder with its voice-oriented default carrier.
 *
 * A periodic Hann window and 75% overlap provide smooth weighted overlap-add
 * reconstruction. The fixed CMSIS initializer retains only the transform
 * tables used by this 1024-point effect.
 *
 * Background: https://en.wikipedia.org/wiki/Hann_function
 */
AudioEffectVocoder::AudioEffectVocoder()
  : AudioStream(1, inputQueueArray), outputFifoHead(0), outputFifoTail(0),
    outputFifoCount(0), inputFill(0), carrierPhase(0.0f),
    noiseState(kNoiseSeed),
    carrierPhaseIncrement(DEFAULT_CARRIER_HZ / AUDIO_SAMPLE_RATE_EXACT),
    attackCoefficient(envelopeCoefficient(DEFAULT_ATTACK_MS)),
    releaseCoefficient(envelopeCoefficient(DEFAULT_RELEASE_MS)),
    noiseMix(DEFAULT_NOISE_MIX),
    gatePower(gatePowerThreshold(DEFAULT_GATE_THRESHOLD_DBFS))
{
  rfft.Sint = arm_cfft_sR_f32_len512;
  rfft.fftLenRFFT = FFT_SIZE;
  rfft.pTwiddleRFFT =
    const_cast<float32_t *>(twiddleCoef_rfft_1024);

  for (int i = 0; i < FFT_SIZE; ++i) {
    window[i] = 0.5f -
                0.5f * cosf(kTwoPi * static_cast<float>(i) /
                             static_cast<float>(FFT_SIZE));
  }
  reset();
}

float AudioEffectVocoder::envelopeCoefficient(float milliseconds)
{
  if (milliseconds == 0.0f) {
    return 0.0f;
  }
  const float samples =
    milliseconds * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f);
  return expf(-static_cast<float>(HOP_SIZE) / samples);
}

float AudioEffectVocoder::gatePowerThreshold(float decibelsFullScale)
{
  const float threshold =
    32768.0f * powf(10.0f, decibelsFullScale / 20.0f);
  const float transformScale = static_cast<float>(FFT_SIZE);

  // The periodic Hann window has mean-square gain 3/8. A real signal's
  // positive-frequency bins contain half its non-DC spectral power, so this
  // converts an RMS threshold in PCM units to the unscaled RFFT power used by
  // processFrame().
  return threshold * threshold * transformScale * transformScale *
         (3.0f / 16.0f);
}

/**
 * Validate and apply the carrier and envelope controls atomically as a set.
 *
 * Attack and release are exponential time constants evaluated once per STFT
 * hop. A zero-millisecond time selects the new level immediately. Noise mix is
 * a linear blend between the harmonic saw carrier and deterministic white
 * noise, which mainly improves consonant articulation. The dBFS gate prevents
 * a steady microphone noise floor from holding the carrier open between words.
 *
 * Background: https://en.wikipedia.org/wiki/DBFS
 */
bool AudioEffectVocoder::configure(float carrierHz,
                                   float attackMs,
                                   float releaseMs,
                                   float newNoiseMix,
                                   float gateThresholdDbfs)
{
  if (!isfinite(carrierHz) || !isfinite(attackMs) ||
      !isfinite(releaseMs) || !isfinite(newNoiseMix) ||
      !isfinite(gateThresholdDbfs) ||
      carrierHz < MIN_CARRIER_HZ || carrierHz > MAX_CARRIER_HZ ||
      attackMs < 0.0f || attackMs > MAX_ATTACK_MS ||
      releaseMs < 0.0f || releaseMs > MAX_RELEASE_MS ||
      newNoiseMix < 0.0f || newNoiseMix > 1.0f ||
      gateThresholdDbfs < MIN_GATE_DBFS ||
      gateThresholdDbfs > MAX_GATE_DBFS) {
    return false;
  }

  const float newPhaseIncrement = carrierHz / AUDIO_SAMPLE_RATE_EXACT;
  const float newAttackCoefficient = envelopeCoefficient(attackMs);
  const float newReleaseCoefficient = envelopeCoefficient(releaseMs);
  const float newGatePower = gatePowerThreshold(gateThresholdDbfs);
  if (!isfinite(newPhaseIncrement) || !isfinite(newAttackCoefficient) ||
      !isfinite(newReleaseCoefficient) || !isfinite(newGatePower)) {
    return false;
  }

  carrierPhaseIncrement = newPhaseIncrement;
  attackCoefficient = newAttackCoefficient;
  releaseCoefficient = newReleaseCoefficient;
  noiseMix = newNoiseMix;
  gatePower = newGatePower;
  return true;
}

/** Reset every time-varying part of the vocoder to a deterministic state. */
void AudioEffectVocoder::reset()
{
  outputFifoHead = 0;
  outputFifoTail = 0;
  outputFifoCount = 0;
  inputFill = 0;
  carrierPhase = 0.0f;
  noiseState = kNoiseSeed;

  memset(modulatorHistory, 0, sizeof(modulatorHistory));
  memset(carrierHistory, 0, sizeof(carrierHistory));
  memset(fftTimeBuffer, 0, sizeof(fftTimeBuffer));
  memset(modulatorSpectrum, 0, sizeof(modulatorSpectrum));
  memset(carrierSpectrum, 0, sizeof(carrierSpectrum));
  memset(bandEnvelope, 0, sizeof(bandEnvelope));
  memset(outputAccumulator, 0, sizeof(outputAccumulator));
  memset(outputFifo, 0, sizeof(outputFifo));
}

float AudioEffectVocoder::polyBlep(float phase, float phaseIncrement)
{
  if (phase < phaseIncrement) {
    const float position = phase / phaseIncrement;
    return position + position - position * position - 1.0f;
  }
  if (phase > 1.0f - phaseIncrement) {
    const float position = (phase - 1.0f) / phaseIncrement;
    return position * position + position + position + 1.0f;
  }
  return 0.0f;
}

/** Generate one continuous, antialiased saw/noise carrier sample. */
float AudioEffectVocoder::generateCarrierSample(float phaseIncrement,
                                                 float carrierNoiseMix)
{
  float saw = 2.0f * carrierPhase - 1.0f;
  saw -= polyBlep(carrierPhase, phaseIncrement);
  carrierPhase += phaseIncrement;
  if (carrierPhase >= 1.0f) {
    carrierPhase -= 1.0f;
  }

  uint32_t state = noiseState;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  noiseState = state;
  const float whiteNoise =
    static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f;

  return kCarrierAmplitude *
         (saw + carrierNoiseMix * (whiteNoise - saw));
}

/**
 * Consume one microphone block and replace it with vocoded output.
 *
 * Input blocks accumulate until one HOP_SIZE analysis hop is ready. The FIFO
 * bridges those synthesis hops back to AudioStream's block size; initial
 * missing samples are silent while the windowed pipeline primes. Carrier state
 * advances even when the modulator is silent, but its energy cannot leak
 * without a band envelope.
 */
void AudioEffectVocoder::update(void)
{
  audio_block_t *block = receiveWritable(0);
  if (!block) {
    return;
  }

  memmove(modulatorHistory,
          modulatorHistory + AUDIO_BLOCK_SAMPLES,
          (FFT_SIZE - AUDIO_BLOCK_SAMPLES) * sizeof(float));
  memmove(carrierHistory,
          carrierHistory + AUDIO_BLOCK_SAMPLES,
          (FFT_SIZE - AUDIO_BLOCK_SAMPLES) * sizeof(float));

  const float phaseIncrement = carrierPhaseIncrement;
  const float carrierNoiseMix = noiseMix;
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
    const int historyIndex = FFT_SIZE - AUDIO_BLOCK_SAMPLES + i;
    modulatorHistory[historyIndex] = static_cast<float>(block->data[i]);
    carrierHistory[historyIndex] =
      generateCarrierSample(phaseIncrement, carrierNoiseMix);
  }

  inputFill += AUDIO_BLOCK_SAMPLES;
  if (inputFill >= HOP_SIZE) {
    inputFill -= HOP_SIZE;
    processFrame();
  }

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
    if (outputFifoCount != 0U) {
      block->data[i] = outputFifo[outputFifoTail];
      outputFifoTail = (outputFifoTail + 1U) & (OUT_FIFO_SIZE - 1U);
      --outputFifoCount;
    } else {
      block->data[i] = 0;
    }
  }

  transmit(block);
  release(block);
}

/**
 * Transfer one frame of microphone band envelopes to the carrier spectrum.
 *
 * Separate real FFTs analyze the microphone and carrier. Within each band the
 * carrier is normalized to the microphone's smoothed spectral energy while
 * retaining its own harmonic/noise phase. The inverse FFT is Hann-windowed a
 * second time and combined by weighted overlap-add (WOLA).
 *
 * Envelope changes affect a complete FFT_SIZE-sample frame, so sharp
 * transients can smear across the analysis window. This is a normal STFT
 * trade-off, not a fixed sample-for-sample identity delay.
 */
void AudioEffectVocoder::processFrame()
{
  for (int i = 0; i < FFT_SIZE; ++i) {
    fftTimeBuffer[i] = modulatorHistory[i] * window[i];
  }
  arm_rfft_fast_f32(&rfft, fftTimeBuffer, modulatorSpectrum, 0);

  for (int i = 0; i < FFT_SIZE; ++i) {
    fftTimeBuffer[i] = carrierHistory[i] * window[i];
  }
  arm_rfft_fast_f32(&rfft, fftTimeBuffer, carrierSpectrum, 0);

  const float attack = attackCoefficient;
  const float release = releaseCoefficient;
  float modulatorPowers[NUM_BANDS];
  float carrierPowers[NUM_BANDS];
  float totalModulatorPower = 0.0f;
  for (int band = 0; band < NUM_BANDS; ++band) {
    float modulatorPower = 0.0f;
    float carrierPower = 0.0f;
    for (int bin = BAND_EDGE_BINS[band]; bin < BAND_EDGE_BINS[band + 1];
         ++bin) {
      modulatorPower += magnitudeSquared(modulatorSpectrum, bin);
      carrierPower += magnitudeSquared(carrierSpectrum, bin);
    }

    modulatorPowers[band] = modulatorPower;
    carrierPowers[band] = carrierPower;
    totalModulatorPower += modulatorPower;
  }

  const bool gateIsOpen = totalModulatorPower >= gatePower;
  for (int band = 0; band < NUM_BANDS; ++band) {
    const float modulatorPower = modulatorPowers[band];
    const float carrierPower = carrierPowers[band];

    const float targetEnvelope = gateIsOpen ? sqrtf(modulatorPower) : 0.0f;
    const float coefficient =
      targetEnvelope > bandEnvelope[band] ? attack : release;
    bandEnvelope[band] = coefficient * bandEnvelope[band] +
                         (1.0f - coefficient) * targetEnvelope;

    float gain = 0.0f;
    if (carrierPower > 0.0f) {
      gain = bandEnvelope[band] / sqrtf(carrierPower);
      if (gain > kMaximumBandGain) {
        gain = kMaximumBandGain;
      }
    }

    for (int bin = BAND_EDGE_BINS[band]; bin < BAND_EDGE_BINS[band + 1];
         ++bin) {
      carrierSpectrum[2 * bin] *= gain;
      carrierSpectrum[2 * bin + 1] *= gain;
    }
  }

  // The packed real FFT stores DC and Nyquist at indices 0 and 1. Bin 1 is
  // below the first vocoder band; bins above the last edge only add hiss.
  carrierSpectrum[0] = 0.0f;
  carrierSpectrum[1] = 0.0f;
  carrierSpectrum[2] = 0.0f;
  carrierSpectrum[3] = 0.0f;
  for (int bin = BAND_EDGE_BINS[NUM_BANDS]; bin < NUM_BINS - 1; ++bin) {
    carrierSpectrum[2 * bin] = 0.0f;
    carrierSpectrum[2 * bin + 1] = 0.0f;
  }

  arm_rfft_fast_f32(&rfft, carrierSpectrum, fftTimeBuffer, 1);

  constexpr float kWolaGain = 1.0f / 1.5f;
  for (int i = 0; i < FFT_SIZE; ++i) {
    outputAccumulator[i] +=
      fftTimeBuffer[i] * window[i] * kWolaGain;
  }

  for (int i = 0; i < HOP_SIZE; ++i) {
    if (outputFifoCount < OUT_FIFO_SIZE) {
      outputFifo[outputFifoHead] =
        voicechanger::saturatingFloatToInt16(outputAccumulator[i]);
      outputFifoHead = (outputFifoHead + 1U) & (OUT_FIFO_SIZE - 1U);
      ++outputFifoCount;
    }
  }

  memmove(outputAccumulator,
          outputAccumulator + HOP_SIZE,
          (FFT_SIZE - HOP_SIZE) * sizeof(float));
  memset(outputAccumulator + FFT_SIZE - HOP_SIZE,
         0,
         HOP_SIZE * sizeof(float));
}
