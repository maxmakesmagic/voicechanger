#ifndef TEST_SUPPORT_ARM_MATH_H_
#define TEST_SUPPORT_ARM_MATH_H_

#include <cstdint>

using float32_t = float;

struct arm_cfft_instance_f32 {
  std::uint16_t fftLen;
};

void arm_cfft_f32(const arm_cfft_instance_f32 *instance,
                  float32_t *data,
                  std::uint8_t inverse,
                  std::uint8_t bitReverse);

#endif
