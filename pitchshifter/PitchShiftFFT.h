// FFT phase-vocoder pitch shifter for the Teensy Audio Library (Teensy 4.x).
//
// There is no ready-made library for this (the stock Audio library's only
// pitch shifter is the granular effect; see ../audioshield.ino). This is a
// from-scratch implementation, so expect to tune the constants below and in
// the .cpp against real hardware -- see README.md.

#ifndef pitch_shift_fft_h_
#define pitch_shift_fft_h_

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>

class AudioEffectPitchShiftFFT : public AudioStream
{
public:
  AudioEffectPitchShiftFFT();
  // >1.0 shifts up and <1.0 shifts down. Non-finite and non-positive
  // values are ignored, preserving the last valid ratio.
  void setPitchRatio(float ratio);
  virtual void update(void);

  static const int FFT_SIZE = 1024;
  static const int NUM_BINS = FFT_SIZE / 2 + 1; // bin 0 (DC) .. bin FFT_SIZE/2 (Nyquist)
  static const int HOP_SIZE = 256;              // must be a multiple of AUDIO_BLOCK_SAMPLES
  static const int OUT_FIFO_SIZE = 1024;        // power of 2, >= 2x HOP_SIZE

private:
  void processFrame();

  audio_block_t *inputQueueArray[1];

  float pitchRatio;
  float window[FFT_SIZE]; // Hann, used for both analysis and synthesis

  float inBuf[FFT_SIZE]; // sliding history of the most recent input samples
  int inBufFill;         // new input samples accumulated since the last hop

  float fftBuf[FFT_SIZE * 2]; // interleaved real/imag scratch for arm_cfft_f32

  float lastPhase[NUM_BINS];       // analysis phase from the previous hop
  float synthPhaseAccum[NUM_BINS]; // running output phase, per output bin
  float synthMag[NUM_BINS];        // scratch: magnitude assigned per output bin this hop
  float synthFreq[NUM_BINS];       // scratch: shifted true frequency (in bins) per output bin

  float outAccum[FFT_SIZE]; // overlap-add accumulator, aligned to the newest frame

  int16_t outFifo[OUT_FIFO_SIZE];
  unsigned int outFifoHead, outFifoTail, outFifoCount;
};

#endif
