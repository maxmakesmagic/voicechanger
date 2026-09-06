#include "StereoChorus.h"

#include <VoiceChangerAudioSample.h>
#include <arm_math.h>
#include <math.h>

namespace {

constexpr float kHalfCycleDegrees = 180.0f;
constexpr float kFullCycleDegrees = 360.0f;

} // namespace

/**
 * Construct a silent stereo chorus with voice-oriented default settings.
 *
 * The delay storage is owned by the effect and contains no dynamic allocation.
 * The right Audio block is obtained from Teensy's normal AudioMemory pool only
 * while update() is producing a stereo pair.
 */
AudioEffectStereoChorus::AudioEffectStereoChorus()
  : AudioStream(1, inputQueueArray),
    centerDelaySamples(DEFAULT_DELAY_MS * SAMPLES_PER_MILLISECOND),
    depthSamples(DEFAULT_DEPTH_MS * SAMPLES_PER_MILLISECOND),
    phaseAdvanceDegrees(DEFAULT_RATE_HZ * DEGREES_PER_BLOCK),
    wetMix(DEFAULT_WET_MIX), lfoPhaseDegrees(0.0f)
{
  reset();
}

/**
 * Validate and apply the delay, modulation, and mix controls as one unit.
 *
 * The entire swept delay must remain inside the interpolation-safe region of
 * the circular buffer. Building the candidate values before assigning members
 * prevents a partially applied configuration if any argument is invalid. The
 * caller must pause Audio updates if changing these members during streaming.
 */
bool AudioEffectStereoChorus::configure(float delayMs,
                                        float depthMs,
                                        float rateHz,
                                        float newWetMix)
{
  if (!isfinite(delayMs) || !isfinite(depthMs) || !isfinite(rateHz) ||
      !isfinite(newWetMix) || delayMs <= 0.0f || depthMs < 0.0f ||
      rateHz < 0.0f || rateHz > MAX_RATE_HZ || newWetMix < 0.0f ||
      newWetMix > 1.0f) {
    return false;
  }

  const float newCenterDelay = delayMs * SAMPLES_PER_MILLISECOND;
  const float newDepth = depthMs * SAMPLES_PER_MILLISECOND;
  const float minimumDelay = newCenterDelay - newDepth;
  const float maximumDelay = newCenterDelay + newDepth;
  // Allow only enough tolerance for an exact sample boundary to survive the
  // public milliseconds-to-samples conversion. clampDelay() still enforces
  // the hard bounds at every read.
  if (!isfinite(newCenterDelay) || !isfinite(newDepth) ||
      !isfinite(minimumDelay) || !isfinite(maximumDelay) ||
      minimumDelay < MIN_DELAY_SAMPLES - CONFIGURATION_EPSILON_SAMPLES ||
      maximumDelay > MAX_DELAY_SAMPLES + CONFIGURATION_EPSILON_SAMPLES) {
    return false;
  }

  centerDelaySamples = newCenterDelay;
  depthSamples = newDepth;
  phaseAdvanceDegrees = rateHz * DEGREES_PER_BLOCK;
  wetMix = newWetMix;
  return true;
}

/**
 * Discard all delayed audio and restart the stereo LFO at its reference phase.
 *
 * Configuration is deliberately retained, allowing reset() to mark a clean
 * stream boundary without making the caller repeat its control settings.
 */
void AudioEffectStereoChorus::reset()
{
  delayLine.clear();
  lfoPhaseDegrees = 0.0f;
}

float AudioEffectStereoChorus::clampDelay(float delaySamples)
{
  if (delaySamples < MIN_DELAY_SAMPLES) {
    return MIN_DELAY_SAMPLES;
  }
  if (delaySamples > MAX_DELAY_SAMPLES) {
    return MAX_DELAY_SAMPLES;
  }
  return delaySamples;
}

float AudioEffectStereoChorus::nextLfoPhase() const
{
  float phase = lfoPhaseDegrees + phaseAdvanceDegrees;
  if (phase > kHalfCycleDegrees) {
    phase -= kFullCycleDegrees;
  }
  return phase;
}

/**
 * Process one mono input block into left and right chorus outputs.
 *
 * CMSIS evaluates the starting LFO phase and a one-sample rotation once per
 * block. The per-sample loop rotates that sine/cosine pair with four
 * multiplications, avoiding both per-sample trigonometry and a coarse linear
 * approximation. Opposite sine values place the wet voices half a modulation
 * cycle apart.
 *
 * Every input sample is written before its delayed positions are read. If the
 * AudioMemory pool cannot supply the second block, update() keeps the delay and
 * LFO histories moving but sends an unmodified mono block to both outputs.
 */
void AudioEffectStereoChorus::update(void)
{
  audio_block_t *leftBlock = receiveWritable(0);
  if (!leftBlock) {
    return;
  }

  const float endPhase = nextLfoPhase();
  if (wetMix == 0.0f) {
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      delayLine.push(leftBlock->data[i]);
    }
    lfoPhaseDegrees = endPhase;

    transmit(leftBlock, 0);
    transmit(leftBlock, 1);
    release(leftBlock);
    return;
  }

  audio_block_t *rightBlock = allocate();
  if (!rightBlock) {
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      delayLine.push(leftBlock->data[i]);
    }
    lfoPhaseDegrees = endPhase;

    transmit(leftBlock, 0);
    transmit(leftBlock, 1);
    release(leftBlock);
    return;
  }

  float lfoSine;
  float lfoCosine;
  float rotationSine;
  float rotationCosine;
  arm_sin_cos_f32(lfoPhaseDegrees, &lfoSine, &lfoCosine);
  arm_sin_cos_f32(
    phaseAdvanceDegrees / static_cast<float>(AUDIO_BLOCK_SAMPLES),
    &rotationSine,
    &rotationCosine);

  const float dryMix = 1.0f - wetMix;

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
    const int16_t input = leftBlock->data[i];
    delayLine.push(input);
    const float leftDelay =
      clampDelay(centerDelaySamples + depthSamples * lfoSine);
    const float rightDelay =
      clampDelay(centerDelaySamples - depthSamples * lfoSine);
    const float dry = static_cast<float>(input);
    const float left =
      dryMix * dry + wetMix * delayLine.read(leftDelay);
    const float right =
      dryMix * dry + wetMix * delayLine.read(rightDelay);
    leftBlock->data[i] = voicechanger::saturatingFloatToInt16(left);
    rightBlock->data[i] = voicechanger::saturatingFloatToInt16(right);

    const float nextSine =
      lfoSine * rotationCosine + lfoCosine * rotationSine;
    lfoCosine =
      lfoCosine * rotationCosine - lfoSine * rotationSine;
    lfoSine = nextSine;
  }

  lfoPhaseDegrees = endPhase;
  transmit(leftBlock, 0);
  transmit(rightBlock, 1);
  release(leftBlock);
  release(rightBlock);
}
