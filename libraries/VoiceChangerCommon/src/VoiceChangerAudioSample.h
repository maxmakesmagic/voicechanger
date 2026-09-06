#ifndef VOICE_CHANGER_AUDIO_SAMPLE_H_
#define VOICE_CHANGER_AUDIO_SAMPLE_H_

#include <stdint.h>

namespace voicechanger {

/**
 * Convert a floating-point DSP sample to Teensy's signed 16-bit audio format.
 *
 * Values outside the representable range are saturated rather than allowed to
 * wrap. Conversion inside the range truncates toward zero, matching a normal
 * C++ floating-point-to-integer conversion. NaN maps to silence; positive and
 * negative infinity saturate to the corresponding endpoint.
 */
constexpr int16_t saturatingFloatToInt16(float sample)
{
  // This combined upper-bound check keeps the ordinary finite path to the same
  // two comparisons as a basic clamp. NaN fails the ordered comparison and is
  // distinguished only on that uncommon branch.
  if (!(sample < 32767.0f)) {
    return sample == sample ? INT16_MAX : 0;
  }
  if (sample <= -32768.0f) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(sample);
}

} // namespace voicechanger

#endif
