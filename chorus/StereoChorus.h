#ifndef stereo_chorus_h_
#define stereo_chorus_h_

#include <Arduino.h>
#include <AudioStream.h>

#include "FractionalDelayLine.h"

/**
 * Mono-input, stereo-output chorus for the Teensy Audio Library.
 *
 * Each output mixes the dry input with a fractionally delayed copy. A slow
 * sinusoidal LFO moves the two delay taps in opposite directions, creating
 * pitch variation and stereo width without feedback. The dry/wet gains sum to
 * one; the final conversion also saturates any cubic-interpolation overshoot.
 *
 * Background: https://en.wikipedia.org/wiki/Chorus_effect
 */
class AudioEffectStereoChorus : public AudioStream
{
public:
  static constexpr size_t DELAY_LINE_SAMPLES = 2048;
  static constexpr float MIN_DELAY_SAMPLES = 1.0f;
  static constexpr float MAX_DELAY_SAMPLES =
    static_cast<float>(DELAY_LINE_SAMPLES - 3U);
  static constexpr float MAX_RATE_HZ = 5.0f;
  static constexpr float MAX_DEPTH_SAMPLES =
    (MAX_DELAY_SAMPLES - MIN_DELAY_SAMPLES) * 0.5f;

  // Even the deepest valid sweep must keep moving forward through input time.
  static_assert(6.28318530718f * MAX_RATE_HZ * MAX_DEPTH_SAMPLES /
                  AUDIO_SAMPLE_RATE_EXACT < 1.0f,
                "maximum modulation must not reverse the delay read head");

  static constexpr float DEFAULT_DELAY_MS = 18.0f;
  static constexpr float DEFAULT_DEPTH_MS = 8.0f;
  static constexpr float DEFAULT_RATE_HZ = 0.8f;
  static constexpr float DEFAULT_WET_MIX = 0.6f;

  AudioEffectStereoChorus();

  // Validate every value before changing the configuration. Invalid values
  // leave the last valid configuration untouched. The member writes are not
  // interrupt-atomic: pause Audio updates before calling this on a live stream.
  // New settings take effect immediately and large runtime jumps may click.
  bool configure(float delayMs, float depthMs, float rateHz, float wetMix);

  // Clear delay and LFO history while preserving the current configuration.
  // Pause Audio updates before calling this on target hardware.
  void reset();

  void update(void) override;

private:
  static constexpr float SAMPLES_PER_MILLISECOND =
    AUDIO_SAMPLE_RATE_EXACT / 1000.0f;
  static constexpr float DEGREES_PER_BLOCK =
    360.0f * static_cast<float>(AUDIO_BLOCK_SAMPLES) /
    AUDIO_SAMPLE_RATE_EXACT;
  static constexpr float CONFIGURATION_EPSILON_SAMPLES = 0.001f;

  static float clampDelay(float delaySamples);
  float nextLfoPhase() const;

  audio_block_t *inputQueueArray[1];
  voicechanger::FractionalDelayLine<DELAY_LINE_SAMPLES> delayLine;

  float centerDelaySamples;
  float depthSamples;
  float phaseAdvanceDegrees;
  float wetMix;
  float lfoPhaseDegrees;
};

#endif
