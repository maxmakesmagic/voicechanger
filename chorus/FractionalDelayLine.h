#ifndef voicechanger_fractional_delay_line_h_
#define voicechanger_fractional_delay_line_h_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace voicechanger {

/**
 * Fixed-size circular delay with third-order Lagrange fractional reads.
 *
 * push() stores the current sample before read() is called, so a delay of zero
 * returns that current sample, a delay of one returns the previous sample, and
 * a delay of 1.5 interpolates between the samples one and two positions back.
 * Four neighbouring samples form a cubic polynomial, reducing the high-
 * frequency loss caused by linear interpolation. Keeping this primitive
 * independent of AudioStream makes it reusable by chorus, flanging, vibrato,
 * and doubling effects.
 *
 * Background:
 *   https://en.wikipedia.org/wiki/Circular_buffer
 *   https://en.wikipedia.org/wiki/Lagrange_polynomial
 */
template <size_t SampleCount>
class FractionalDelayLine
{
  static_assert(SampleCount >= 4, "a delay line needs at least four samples");
  static_assert((SampleCount & (SampleCount - 1)) == 0,
                "delay line size must be a power of two");

public:
  FractionalDelayLine()
  {
    clear();
  }

  void clear()
  {
    memset(samples, 0, sizeof(samples));
    nextWriteIndex = 0;
  }

  void push(int16_t sample)
  {
    samples[nextWriteIndex] = sample;
    nextWriteIndex = (nextWriteIndex + 1U) & kIndexMask;
  }

  // delaySamples must be finite and in [1, SampleCount - 3].
  float read(float delaySamples) const
  {
    const size_t wholeSamples = static_cast<size_t>(delaySamples);
    const float fraction = delaySamples - static_cast<float>(wholeSamples);
    const size_t intervalStartIndex =
      (nextWriteIndex - 1U - wholeSamples) & kIndexMask;
    const size_t precedingIndex = (intervalStartIndex + 1U) & kIndexMask;
    const size_t intervalEndIndex = (intervalStartIndex - 1U) & kIndexMask;
    const size_t followingIndex = (intervalEndIndex - 1U) & kIndexMask;

    const float preceding = static_cast<float>(samples[precedingIndex]);
    const float intervalStart =
      static_cast<float>(samples[intervalStartIndex]);
    const float intervalEnd = static_cast<float>(samples[intervalEndIndex]);
    const float following = static_cast<float>(samples[followingIndex]);

    // Evaluate the polynomial through positions -1, 0, 1, and 2 at the
    // fractional position in [0, 1). At integral delays this returns the
    // interval-start sample exactly.
    const float coefficientPreceding =
      -fraction * (fraction - 1.0f) * (fraction - 2.0f) / 6.0f;
    const float coefficientStart =
      (fraction + 1.0f) * (fraction - 1.0f) *
      (fraction - 2.0f) / 2.0f;
    const float coefficientEnd =
      -(fraction + 1.0f) * fraction * (fraction - 2.0f) / 2.0f;
    const float coefficientFollowing =
      (fraction + 1.0f) * fraction * (fraction - 1.0f) / 6.0f;

    return coefficientPreceding * preceding +
           coefficientStart * intervalStart +
           coefficientEnd * intervalEnd +
           coefficientFollowing * following;
  }

  static constexpr size_t capacity()
  {
    return SampleCount;
  }

private:
  static constexpr size_t kIndexMask = SampleCount - 1U;

  int16_t samples[SampleCount];
  size_t nextWriteIndex;
};

} // namespace voicechanger

#endif
