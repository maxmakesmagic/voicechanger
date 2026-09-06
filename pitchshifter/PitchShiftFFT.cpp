#include "PitchShiftFFT.h"
#include <arm_const_structs.h>
#include <string.h>
#include <math.h>

namespace {

// Keep phase arithmetic in single precision on Cortex-M7. Arduino's TWO_PI is
// a double constant, and division by it otherwise promotes the hot loops to
// much slower double-precision work.
constexpr float kTwoPi = 6.283185307179586476925286766559f;
constexpr float kInverseTwoPi = 1.0f / kTwoPi;

inline void sinCos(float angle, float &sine, float &cosine)
{
#if defined(__GNUC__) && !defined(__clang__)
  __builtin_sincosf(angle, &sine, &cosine);
#else
  sine = sinf(angle);
  cosine = cosf(angle);
#endif
}

} // namespace

/**
 * Construct a silent pitch shifter at unity pitch and precompute its window.
 *
 * Input history, phase state, overlap-add storage, and the output FIFO start at
 * zero. The periodic Hann window tapers each FFT frame so adjacent, overlapping
 * frames can be recombined without discontinuities. The same window is used
 * for analysis and synthesis, so processFrame() compensates for their product.
 *
 * Background: https://en.wikipedia.org/wiki/Hann_function
 */
AudioEffectPitchShiftFFT::AudioEffectPitchShiftFFT()
  : AudioStream(1, inputQueueArray), pitchRatio(1.0f), inBufFill(0),
    outFifoHead(0), outFifoTail(0), outFifoCount(0)
{
  for (int i = 0; i < FFT_SIZE; i++) {
    window[i] = 0.5f - 0.5f * cosf(kTwoPi * (float)i / (float)FFT_SIZE); // periodic Hann
    inBuf[i] = 0.0f;
    outAccum[i] = 0.0f;
  }
  for (int i = 0; i < NUM_BINS; i++) {
    lastPhase[i] = 0.0f;
    synthPhaseAccum[i] = 0.0f;
    synthMag[i] = 0.0f;
    synthFreq[i] = 0.0f;
  }
}

/**
 * Set the output-to-input pitch ratio used by subsequent FFT frames.
 *
 * Values above 1 shift upward and values between 0 and 1 shift downward.
 * Zero, negative, NaN, and infinite values have no useful pitch meaning, so
 * they are ignored and the previous valid ratio remains active.
 */
void AudioEffectPitchShiftFFT::setPitchRatio(float ratio)
{
  if (isfinite(ratio) && ratio > 0.0f) {
    pitchRatio = ratio;
  }
}

/**
 * Consume and replace one Teensy Audio block.
 *
 * Each call appends AUDIO_BLOCK_SAMPLES input samples to the sliding analysis
 * window. Every HOP_SIZE new samples, processFrame() creates one hop of output.
 * A circular FIFO adapts those HOP_SIZE chunks back to Teensy's block size;
 * until the FFT pipeline is primed, the missing output is returned as silence.
 * The consumed block is then transmitted and released. The resulting
 * input-to-output latency is (FFT_SIZE - AUDIO_BLOCK_SAMPLES) samples.
 *
 * Background: https://en.wikipedia.org/wiki/Circular_buffer
 */
void AudioEffectPitchShiftFFT::update(void)
{
  audio_block_t *block = receiveWritable(0);
  if (!block) return;

  // Slide the analysis history forward by one audio block.
  memmove(inBuf, inBuf + AUDIO_BLOCK_SAMPLES, (FFT_SIZE - AUDIO_BLOCK_SAMPLES) * sizeof(float));
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    inBuf[FFT_SIZE - AUDIO_BLOCK_SAMPLES + i] = (float)block->data[i];
  }
  inBufFill += AUDIO_BLOCK_SAMPLES;

  if (inBufFill >= HOP_SIZE) {
    inBufFill -= HOP_SIZE;
    processFrame();
  }

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    if (outFifoCount > 0) {
      block->data[i] = outFifo[outFifoTail];
      outFifoTail = (outFifoTail + 1) & (OUT_FIFO_SIZE - 1);
      outFifoCount--;
    } else {
      block->data[i] = 0; // still priming the pipeline at startup
    }
  }

  transmit(block);
  release(block);
}

/**
 * Analyze, pitch-remap, and resynthesize one overlapping FFT frame.
 *
 * This is a phase-vocoder pipeline:
 *   1. Apply a Hann window and transform the newest time-domain frame.
 *   2. Use inter-frame phase change to estimate each bin's true frequency.
 *   3. Move its magnitude and frequency to a bin scaled by pitchRatio.
 *   4. Accumulate synthesis phase and rebuild a real-signal spectrum.
 *   5. Inverse-transform, window again, and overlap-add one output hop.
 *
 * Background:
 *   https://en.wikipedia.org/wiki/Phase_vocoder
 *   https://en.wikipedia.org/wiki/Short-time_Fourier_transform
 */
void AudioEffectPitchShiftFFT::processFrame()
{
  // Use one coherent ratio for the whole frame and keep it in a local so calls
  // to the math library cannot force repeated member reloads.
  const float ratio = pitchRatio;

  // --- Analysis: one windowed short-time Fourier transform (STFT) frame ---
  for (int i = 0; i < FFT_SIZE; i++) {
    fftBuf[2 * i]     = inBuf[i] * window[i];
    fftBuf[2 * i + 1] = 0.0f;
  }
  // The FFT is the efficient algorithm used here to compute the frame's DFT.
  // https://en.wikipedia.org/wiki/Fast_Fourier_transform
  arm_cfft_f32(&arm_cfft_sR_f32_len1024, fftBuf, 0, 1);

  // A sinusoid centered on bin k advances by k * 2*pi*H/N radians between
  // frames, where H is HOP_SIZE and N is FFT_SIZE.
  const float binFreqStep =
    kTwoPi * (float)HOP_SIZE / (float)FFT_SIZE;
  const float inverseBinFreqStep = 1.0f / binFreqStep;

  for (int i = 0; i < NUM_BINS; i++) {
    synthMag[i] = 0.0f;
    synthFreq[i] = 0.0f;
  }

  for (int k = 0; k < NUM_BINS; k++) {
    float re = fftBuf[2 * k];
    float im = fftBuf[2 * k + 1];
    float mag = sqrtf(re * re + im * im);
    float phase = atan2f(im, re);

    // DC and Nyquist are purely real for a real input signal. Keep their sign
    // in the coefficient because synthesis deliberately fixes their phase at
    // zero. Treating them as unsigned magnitudes would turn negative DC (and
    // one polarity of Nyquist) positive, even when pitchRatio is exactly 1.
    if (k == 0 || k == NUM_BINS - 1) {
      mag = re;
    }

    // Subtract the phase advance expected at the bin center and reduce the
    // residual to its principal value in [-pi, pi], resolving its 2*pi
    // ambiguity. Expressing that correction in fractional FFT bins gives the
    // phase-vocoder estimate of instantaneous frequency.
    // https://en.wikipedia.org/wiki/Instantaneous_phase_and_frequency
    float trueBin = (float)k;
    if (k > 0 && k < NUM_BINS - 1) {
      float deltaPhase = phase - lastPhase[k] - (float)k * binFreqStep;
      deltaPhase -=
        kTwoPi * roundf(deltaPhase * kInverseTwoPi); // wrap to [-pi, pi]
      trueBin = (float)k + deltaPhase * inverseBinFreqStep;
    }
    lastPhase[k] = phase;

    // Accumulate this bin's magnitude at round(k * ratio), and scale its
    // fractional frequency by the same ratio. Bins shifted above Nyquist are
    // discarded. If source bins collide, their magnitudes add and the final
    // contributor supplies the output bin's frequency estimate.
    const float shiftedBin = (float)k * ratio;
    // Check the floating-point value before converting it. This also safely
    // discards bins for extremely large (but otherwise valid) ratios.
    if (shiftedBin >= 0.0f && shiftedBin < (float)NUM_BINS - 0.5f) {
      int newBin = (int)(shiftedBin + 0.5f);
      synthMag[newBin] += mag;
      synthFreq[newBin] = trueBin * ratio;
    }
  }

  // --- Synthesis: rebuild the spectrum with coherent inter-frame phase ---
  for (int k = 0; k < NUM_BINS; k++) {
    // DC and Nyquist must be real, so they need neither phase accumulation nor
    // trigonometry.
    if (k == 0 || k == NUM_BINS - 1) {
      fftBuf[2 * k] = synthMag[k];
      fftBuf[2 * k + 1] = 0.0f;
      continue;
    }

    // Advance by the shifted instantaneous frequency. Wrapping is inaudible
    // because sin/cos are 2*pi-periodic, and prevents loss of float precision
    // during long-running use.
    synthPhaseAccum[k] += synthFreq[k] * binFreqStep;
    synthPhaseAccum[k] -=
      kTwoPi * roundf(synthPhaseAccum[k] * kInverseTwoPi);

    const int mirror = FFT_SIZE - k;
    // Pitch remapping leaves some output bins empty. Preserve their phase
    // state above, but avoid trigonometry when multiplying by zero would
    // produce an empty coefficient anyway.
    if (synthMag[k] == 0.0f) {
      fftBuf[2 * k] = 0.0f;
      fftBuf[2 * k + 1] = 0.0f;
      fftBuf[2 * mirror] = 0.0f;
      fftBuf[2 * mirror + 1] = 0.0f;
      continue;
    }

    float sine;
    float cosine;
    sinCos(synthPhaseAccum[k], sine, cosine);
    float re = synthMag[k] * cosine;
    float im = synthMag[k] * sine;
    fftBuf[2 * k] = re;
    fftBuf[2 * k + 1] = im;
    // A real time-domain signal has conjugate-symmetric positive and negative
    // frequency bins: X[N-k] = conjugate(X[k]).
    // https://en.wikipedia.org/wiki/Discrete_Fourier_transform#DFT_of_real_and_purely_imaginary_signals
    fftBuf[2 * mirror] = re;
    fftBuf[2 * mirror + 1] = -im;
  }

  arm_cfft_f32(&arm_cfft_sR_f32_len1024, fftBuf, 1, 1); // includes the 1/FFT_SIZE scaling

  // Weighted overlap-add (WOLA): because both analysis and synthesis multiply
  // by Hann, four overlapping squared periodic Hann windows sum to 1.5. The
  // reciprocal restores unity gain when pitchRatio == 1.
  // https://en.wikipedia.org/wiki/Short-time_Fourier_transform#Inverse_STFT
  const float OUTPUT_GAIN = 1.0f / 1.5f;

  for (int i = 0; i < FFT_SIZE; i++) {
    outAccum[i] += fftBuf[2 * i] * window[i] * OUTPUT_GAIN;
  }

  // No future frame can overlap the oldest HOP_SIZE samples, so saturate them
  // to the int16 audio range and hand them to the output FIFO.
  // https://en.wikipedia.org/wiki/Saturation_arithmetic
  for (int i = 0; i < HOP_SIZE; i++) {
    float sample = outAccum[i];
    if (sample > 32767.0f) sample = 32767.0f;
    else if (sample < -32768.0f) sample = -32768.0f;
    if (outFifoCount < OUT_FIFO_SIZE) {
      outFifo[outFifoHead] = (int16_t)sample;
      outFifoHead = (outFifoHead + 1) & (OUT_FIFO_SIZE - 1);
      outFifoCount++;
    }
  }

  // Slide the accumulator down by one hop, making room for the next frame.
  memmove(outAccum, outAccum + HOP_SIZE, (FFT_SIZE - HOP_SIZE) * sizeof(float));
  for (int i = FFT_SIZE - HOP_SIZE; i < FFT_SIZE; i++) {
    outAccum[i] = 0.0f;
  }
}
