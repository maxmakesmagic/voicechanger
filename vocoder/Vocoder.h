#ifndef voicechanger_vocoder_h_
#define voicechanger_vocoder_h_

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>

#include <stdint.h>

/**
 * Mono spectral vocoder with a self-contained harmonic/noise carrier.
 *
 * The microphone spectrum is divided into perceptually spaced bands. Each
 * band's smoothed amplitude controls the matching band of a continuously
 * generated carrier, transferring the rhythm and resonances of speech onto a
 * synthetic sound. A small deterministic noise component gives unvoiced
 * consonants useful carrier energy without requiring a second audio input.
 *
 * Background:
 *   https://en.wikipedia.org/wiki/Vocoder
 *   https://en.wikipedia.org/wiki/Short-time_Fourier_transform
 */
class AudioEffectVocoder : public AudioStream
{
public:
  static const int FFT_SIZE = 1024;
  static const int NUM_BINS = FFT_SIZE / 2 + 1;
  static const int HOP_SIZE = 256;
  static const int OUT_FIFO_SIZE = 512;
  static const int NUM_BANDS = 20;

  // Half-open FFT-bin ranges used by the spectral envelope follower. Narrower
  // low bands preserve individual carrier harmonics while progressively wider
  // high bands follow the ear's coarser frequency resolution. Keeping the bank
  // here prevents the implementation, documentation, and tests from drifting.
  inline static constexpr uint16_t BAND_EDGE_BINS[NUM_BANDS + 1] = {
    2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25,
    31, 39, 49, 61, 76, 95, 118, 146, 181, 234
  };

  static constexpr float MIN_CARRIER_HZ = 40.0f;
  static constexpr float MAX_CARRIER_HZ = 1000.0f;
  static constexpr float MAX_ATTACK_MS = 1000.0f;
  static constexpr float MAX_RELEASE_MS = 5000.0f;
  static constexpr float MIN_GATE_DBFS = -96.0f;
  static constexpr float MAX_GATE_DBFS = 0.0f;

  static constexpr float DEFAULT_CARRIER_HZ = 110.0f;
  static constexpr float DEFAULT_ATTACK_MS = 8.0f;
  static constexpr float DEFAULT_RELEASE_MS = 80.0f;
  static constexpr float DEFAULT_NOISE_MIX = 0.15f;
  static constexpr float DEFAULT_GATE_THRESHOLD_DBFS = -45.0f;

  AudioEffectVocoder();

  // Apply all controls together. Invalid or non-finite values leave the
  // previous configuration unchanged. Pause Audio updates before changing
  // these values on a live stream.
  bool configure(float carrierHz,
                 float attackMs,
                 float releaseMs,
                 float noiseMix,
                 float gateThresholdDbfs);

  // Clear streaming, envelope, oscillator, and noise history while retaining
  // the current configuration. Pause Audio updates before calling on target.
  void reset();

  void update(void) override;

private:
  static float envelopeCoefficient(float milliseconds);
  static float gatePowerThreshold(float decibelsFullScale);
  static float polyBlep(float phase, float phaseIncrement);
  float generateCarrierSample(float phaseIncrement, float carrierNoiseMix);
  void processFrame();

  audio_block_t *inputQueueArray[1];

  arm_rfft_fast_instance_f32 rfft;
  float window[FFT_SIZE];
  float modulatorHistory[FFT_SIZE];
  float carrierHistory[FFT_SIZE];
  float fftTimeBuffer[FFT_SIZE];
  float modulatorSpectrum[FFT_SIZE];
  float carrierSpectrum[FFT_SIZE];
  float bandEnvelope[NUM_BANDS];
  float outputAccumulator[FFT_SIZE];

  int16_t outputFifo[OUT_FIFO_SIZE];
  unsigned int outputFifoHead;
  unsigned int outputFifoTail;
  unsigned int outputFifoCount;
  int inputFill;

  float carrierPhase;
  uint32_t noiseState;
  float carrierPhaseIncrement;
  float attackCoefficient;
  float releaseCoefficient;
  float noiseMix;
  float gatePower;
};

#endif
