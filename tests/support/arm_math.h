#ifndef TEST_SUPPORT_ARM_MATH_H_
#define TEST_SUPPORT_ARM_MATH_H_

#include <cstdint>

using float32_t = float;

enum arm_status {
  ARM_MATH_SUCCESS = 0,
  ARM_MATH_ARGUMENT_ERROR = -1,
};

struct arm_cfft_instance_f32 {
  std::uint16_t fftLen;
};

struct arm_rfft_fast_instance_f32 {
  arm_cfft_instance_f32 Sint;
  std::uint16_t fftLenRFFT;
  float32_t *pTwiddleRFFT;
};

void arm_cfft_f32(const arm_cfft_instance_f32 *instance,
                  float32_t *data,
                  std::uint8_t inverse,
                  std::uint8_t bitReverse);

arm_status arm_rfft_fast_init_f32(arm_rfft_fast_instance_f32 *instance,
                                  std::uint16_t fftLength);

void arm_rfft_fast_f32(arm_rfft_fast_instance_f32 *instance,
                       float32_t *input,
                       float32_t *output,
                       std::uint8_t inverse);

void arm_sin_cos_f32(float32_t theta,
                     float32_t *sine,
                     float32_t *cosine);

#endif
