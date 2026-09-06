#include "arm_const_structs.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>

const arm_cfft_instance_f32 arm_cfft_sR_f32_len1024 = {1024};

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kMaximumFftSize = 1024;

bool isPowerOfTwo(std::size_t value)
{
  return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

void arm_cfft_f32(const arm_cfft_instance_f32 *instance,
                  float32_t *data,
                  std::uint8_t inverse,
                  std::uint8_t bitReverse)
{
  if (instance == nullptr || data == nullptr || !isPowerOfTwo(instance->fftLen)) {
    throw std::invalid_argument("invalid CFFT input");
  }
  if (instance->fftLen > kMaximumFftSize) {
    throw std::invalid_argument("test CFFT exceeds its fixed scratch buffer");
  }
  if (bitReverse == 0) {
    throw std::invalid_argument("the test CFFT only supports natural-order output");
  }

  const std::size_t size = instance->fftLen;
  // CMSIS uses caller-owned storage and does not allocate for every transform.
  // Fixed scratch keeps host benchmarks from measuring allocator overhead.
  std::array<std::complex<float>, kMaximumFftSize> values;
  for (std::size_t i = 0; i < size; ++i) {
    values[i] = {data[2 * i], data[2 * i + 1]};
  }

  // Iterative radix-2 Cooley-Tukey FFT. This is intentionally independent of
  // CMSIS so native tests can exercise the production DSP code on any runner.
  for (std::size_t i = 1, reversed = 0; i < size; ++i) {
    std::size_t bit = size >> 1;
    for (; (reversed & bit) != 0; bit >>= 1) {
      reversed ^= bit;
    }
    reversed ^= bit;
    if (i < reversed) {
      std::swap(values[i], values[reversed]);
    }
  }

  const double direction = inverse != 0 ? 1.0 : -1.0;
  for (std::size_t length = 2; length <= size; length <<= 1) {
    const float angle = static_cast<float>(direction * kTwoPi /
                                           static_cast<double>(length));
    const std::complex<float> step(std::cos(angle), std::sin(angle));
    for (std::size_t start = 0; start < size; start += length) {
      std::complex<float> twiddle(1.0f, 0.0f);
      for (std::size_t offset = 0; offset < length / 2; ++offset) {
        const std::complex<float> even = values[start + offset];
        const std::complex<float> odd =
          values[start + offset + length / 2] * twiddle;
        values[start + offset] = even + odd;
        values[start + offset + length / 2] = even - odd;
        twiddle *= step;
      }
    }
  }

  const float scale = inverse != 0 ? 1.0f / static_cast<float>(size) : 1.0f;
  for (std::size_t i = 0; i < size; ++i) {
    data[2 * i] = values[i].real() * scale;
    data[2 * i + 1] = values[i].imag() * scale;
  }
}
