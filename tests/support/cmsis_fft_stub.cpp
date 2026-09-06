#include "arm_const_structs.h"
#include "arm_common_tables.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>

const arm_cfft_instance_f32 arm_cfft_sR_f32_len1024 = {1024};
const arm_cfft_instance_f32 arm_cfft_sR_f32_len512 = {512};
const float32_t twiddleCoef_rfft_1024[1024] = {};

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kMaximumFftSize = 1024;

bool isPowerOfTwo(std::size_t value)
{
  return value != 0 && (value & (value - 1)) == 0;
}

void transform(std::array<std::complex<float>, kMaximumFftSize> &values,
               std::size_t size,
               bool inverse)
{
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

  const double direction = inverse ? 1.0 : -1.0;
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

  if (inverse) {
    const float scale = 1.0f / static_cast<float>(size);
    for (std::size_t i = 0; i < size; ++i) {
      values[i] *= scale;
    }
  }
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
  transform(values, size, inverse != 0);
  for (std::size_t i = 0; i < size; ++i) {
    data[2 * i] = values[i].real();
    data[2 * i + 1] = values[i].imag();
  }
}

arm_status arm_rfft_fast_init_f32(arm_rfft_fast_instance_f32 *instance,
                                  std::uint16_t fftLength)
{
  if (instance == nullptr || !isPowerOfTwo(fftLength) ||
      fftLength < 32 || fftLength > kMaximumFftSize) {
    return ARM_MATH_ARGUMENT_ERROR;
  }
  instance->Sint.fftLen = fftLength / 2;
  instance->fftLenRFFT = fftLength;
  instance->pTwiddleRFFT = nullptr;
  return ARM_MATH_SUCCESS;
}

void arm_rfft_fast_f32(arm_rfft_fast_instance_f32 *instance,
                       float32_t *input,
                       float32_t *output,
                       std::uint8_t inverse)
{
  if (instance == nullptr || input == nullptr || output == nullptr ||
      input == output || !isPowerOfTwo(instance->fftLenRFFT) ||
      instance->fftLenRFFT > kMaximumFftSize) {
    throw std::invalid_argument("invalid fast RFFT input");
  }

  const std::size_t size = instance->fftLenRFFT;
  const std::size_t complexSize = size / 2;
  std::array<std::complex<float>, kMaximumFftSize> values;
  if (inverse == 0) {
    // Match the fast-RFFT factorization used by CMSIS: pair the even and odd
    // real samples into one half-length complex sequence, then split its CFFT
    // into the unique half of the real spectrum. This is mathematically
    // equivalent to an N-point complex FFT, but also exposes the production
    // code to the different floating-point rounding of the CMSIS algorithm.
    for (std::size_t i = 0; i < complexSize; ++i) {
      values[i] = {input[2 * i], input[2 * i + 1]};
    }
    transform(values, complexSize, false);

    output[0] = values[0].real() + values[0].imag();
    output[1] = values[0].real() - values[0].imag();
    for (std::size_t bin = 1; bin < complexSize; ++bin) {
      const std::complex<float> a = values[bin];
      const std::complex<float> b = std::conj(values[complexSize - bin]);
      const float angle = static_cast<float>(
        -kTwoPi * static_cast<double>(bin) / static_cast<double>(size));
      const std::complex<float> twiddle(std::cos(angle), std::sin(angle));
      const std::complex<float> coefficient =
        0.5f * (a + b - std::complex<float>(0.0f, 1.0f) *
                           twiddle * (a - b));
      output[2 * bin] = coefficient.real();
      output[2 * bin + 1] = coefficient.imag();
    }
    return;
  }

  // Undo the split above to recover the half-length CFFT of the paired
  // even/odd samples, inverse-transform it, then unpack its real and imaginary
  // components into consecutive real samples.
  for (std::size_t bin = 0; bin < complexSize; ++bin) {
    const std::complex<float> x = bin == 0
      ? std::complex<float>(input[0], 0.0f)
      : std::complex<float>(input[2 * bin], input[2 * bin + 1]);
    const std::complex<float> xOpposite = bin == 0
      ? std::complex<float>(input[1], 0.0f)
      : std::conj(std::complex<float>(
          input[2 * (complexSize - bin)],
          input[2 * (complexSize - bin) + 1]));
    const float angle = static_cast<float>(
      -kTwoPi * static_cast<double>(bin) / static_cast<double>(size));
    const std::complex<float> inverseTwiddle(
      std::cos(angle), -std::sin(angle));
    const std::complex<float> even = 0.5f * (x + xOpposite);
    const std::complex<float> odd =
      0.5f * (x - xOpposite) * inverseTwiddle;
    values[bin] = even + std::complex<float>(0.0f, 1.0f) * odd;
  }
  transform(values, complexSize, true);
  for (std::size_t i = 0; i < complexSize; ++i) {
    output[2 * i] = values[i].real();
    output[2 * i + 1] = values[i].imag();
  }
}
