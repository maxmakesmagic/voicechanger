#include "PitchShiftFFT.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kPrimeBlocks = 32;
// Use a long timed region so scheduler and clock jitter remain small relative
// to the measured work.
constexpr std::size_t kDefaultBlocks = 16384;
constexpr std::size_t kDefaultRepetitions = 31;
constexpr std::size_t kDefaultWarmups = 2;
constexpr std::size_t kMaximumBlocks = 1U << 20;
constexpr std::size_t kMaximumTrials = 10000;

struct Options {
  std::size_t blocks = kDefaultBlocks;
  std::size_t repetitions = kDefaultRepetitions;
  std::size_t warmups = kDefaultWarmups;
  std::vector<float> ratios{0.8f, 1.0f, 1.25f};
};

struct Trial {
  double nanoseconds;
  std::uint64_t checksum;
};

std::size_t parseSize(const char *text,
                      const char *option,
                      std::size_t maximum,
                      bool allowZero = false)
{
  const std::string valueText = text;
  if (valueText.empty() || valueText.front() == '-') {
    throw std::invalid_argument(std::string(option) + " has an invalid value");
  }

  std::size_t parsed = 0;
  const unsigned long long value = std::stoull(valueText, &parsed);
  if (parsed != valueText.size() || (!allowZero && value == 0) ||
      value > maximum || value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string(option) + " has an invalid value");
  }
  return static_cast<std::size_t>(value);
}

float parseRatio(const char *text)
{
  const std::string valueText = text;
  std::size_t parsed = 0;
  const float ratio = std::stof(valueText, &parsed);
  if (parsed != valueText.size() || !std::isfinite(ratio) || ratio <= 0.0f) {
    throw std::invalid_argument("--ratio must be finite and positive");
  }
  return ratio;
}

Options parseOptions(int argc, char **argv)
{
  Options options;
  bool customRatios = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") {
      std::cout
        << "usage: pitch_shift_benchmark [--blocks N] [--repetitions N] "
           "[--warmups N] [--ratio R ...]\n";
      std::exit(0);
    }
    if (i + 1 >= argc) {
      throw std::invalid_argument("missing value after " + argument);
    }
    if (argument == "--blocks") {
      options.blocks = parseSize(argv[++i], "--blocks", kMaximumBlocks);
    } else if (argument == "--repetitions") {
      options.repetitions =
        parseSize(argv[++i], "--repetitions", kMaximumTrials);
    } else if (argument == "--warmups") {
      options.warmups =
        parseSize(argv[++i], "--warmups", kMaximumTrials, true);
    } else if (argument == "--ratio") {
      if (!customRatios) {
        options.ratios.clear();
        customRatios = true;
      }
      options.ratios.push_back(parseRatio(argv[++i]));
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }

  constexpr std::size_t kBlocksPerFrame =
    AudioEffectPitchShiftFFT::HOP_SIZE / AUDIO_BLOCK_SAMPLES;
  if (options.blocks % kBlocksPerFrame != 0) {
    throw std::invalid_argument(
      "--blocks must contain a whole number of processing frames");
  }
  return options;
}

std::vector<audio_block_t> makeInput(std::size_t blockCount,
                                     std::uint32_t seed)
{
  std::vector<audio_block_t> blocks(blockCount);
  std::uint32_t state = seed;
  for (audio_block_t &block : blocks) {
    for (int16_t &sample : block.data) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      sample = static_cast<int16_t>(static_cast<int>(state % 12001U) - 6000);
    }
  }
  return blocks;
}

std::uint64_t checksum(const std::vector<audio_block_t> &blocks)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (const audio_block_t &block : blocks) {
    for (const int16_t sample : block.data) {
      const std::uint16_t bits = static_cast<std::uint16_t>(sample);
      hash ^= bits & 0xFFU;
      hash *= 1099511628211ULL;
      hash ^= bits >> 8;
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

Trial runTrial(float ratio,
               const std::vector<audio_block_t> &primeInput,
               const std::vector<audio_block_t> &measuredInput)
{
  AudioStream::resetTestState();
  AudioEffectPitchShiftFFT effect;
  effect.setPitchRatio(ratio);

  std::vector<audio_block_t> primeBlocks = primeInput;
  for (audio_block_t &block : primeBlocks) {
    AudioStream::queueTestInput(&block);
    effect.update();
  }

  std::vector<audio_block_t> measuredBlocks = measuredInput;
  const Clock::time_point start = Clock::now();
  std::atomic_signal_fence(std::memory_order_seq_cst);
  for (audio_block_t &block : measuredBlocks) {
    AudioStream::queueTestInput(&block);
    effect.update();
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const Clock::time_point finish = Clock::now();

  const std::size_t expectedUpdates = primeBlocks.size() + measuredBlocks.size();
  if (AudioStream::transmissionCount() != expectedUpdates ||
      AudioStream::releaseCount() != expectedUpdates) {
    throw std::runtime_error("the effect did not process every benchmark block");
  }

  const double elapsed =
    std::chrono::duration<double, std::nano>(finish - start).count();
  if (!std::isfinite(elapsed) || elapsed <= 0.0) {
    throw std::runtime_error("the benchmark clock returned an invalid duration");
  }
  const std::uint64_t outputChecksum = checksum(measuredBlocks);
  AudioStream::resetTestState();
  return {elapsed, outputChecksum};
}

double median(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if ((values.size() & 1U) != 0) {
    return values[middle];
  }
  return 0.5 * (values[middle - 1] + values[middle]);
}

void reportRatio(float ratio,
                 const Options &options,
                 const std::vector<audio_block_t> &primeInput,
                 const std::vector<audio_block_t> &measuredInput)
{
  std::uint64_t expectedChecksum = 0;
  bool checksumSet = false;
  const auto validateChecksum = [&](std::uint64_t actual) {
    if (!checksumSet) {
      expectedChecksum = actual;
      checksumSet = true;
    } else if (actual != expectedChecksum) {
      throw std::runtime_error("non-deterministic benchmark output");
    }
  };

  for (std::size_t i = 0; i < options.warmups; ++i) {
    validateChecksum(runTrial(ratio, primeInput, measuredInput).checksum);
  }

  std::vector<double> perFrameMicroseconds;
  perFrameMicroseconds.reserve(options.repetitions);
  const double frameCount =
    static_cast<double>(options.blocks * AUDIO_BLOCK_SAMPLES) /
    AudioEffectPitchShiftFFT::HOP_SIZE;

  for (std::size_t i = 0; i < options.repetitions; ++i) {
    const Trial trial = runTrial(ratio, primeInput, measuredInput);
    validateChecksum(trial.checksum);
    perFrameMicroseconds.push_back(trial.nanoseconds / (1000.0 * frameCount));
  }

  std::vector<double> sorted = perFrameMicroseconds;
  std::sort(sorted.begin(), sorted.end());
  const double middle = median(sorted);
  std::vector<double> deviations;
  deviations.reserve(sorted.size());
  for (const double value : sorted) {
    deviations.push_back(std::abs(value - middle));
  }
  const double mad = median(deviations);
  const std::size_t p95Index = static_cast<std::size_t>(
    std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1;
  const double nanosecondsPerSample =
    middle * 1000.0 / AudioEffectPitchShiftFFT::HOP_SIZE;
  const double averageThroughputFactor =
    1.0e9 / (nanosecondsPerSample * AUDIO_SAMPLE_RATE_EXACT);

  std::cout << std::defaultfloat
            << std::setprecision(std::numeric_limits<float>::max_digits10)
            << ratio << ',' << std::fixed << std::setprecision(3)
            << middle << ',' << mad << ',' << sorted.front()
            << ',' << sorted[p95Index] << ',' << nanosecondsPerSample << ','
            << averageThroughputFactor << ",0x" << std::hex
            << expectedChecksum << std::dec << '\n';
}

} // namespace

int main(int argc, char **argv)
{
  try {
    const Options options = parseOptions(argc, argv);
    const std::vector<audio_block_t> primeInput =
      makeInput(kPrimeBlocks, 0x13579BDFU);
    const std::vector<audio_block_t> measuredInput =
      makeInput(options.blocks, 0x2468ACE1U);

#if defined(__VERSION__)
    std::cout << "compiler=" << __VERSION__ << '\n';
#endif
    std::cout << "blocks=" << options.blocks
              << " repetitions=" << options.repetitions
              << " warmups=" << options.warmups << '\n';
    std::cout
      << "ratio,median_trial_mean_us_per_frame,"
         "mad_trial_mean_us_per_frame,min_trial_mean_us_per_frame,"
         "p95_trial_mean_us_per_frame,ns_per_sample,"
         "average_throughput_factor,output_checksum\n";
    for (const float ratio : options.ratios) {
      reportRatio(ratio, options, primeInput, measuredInput);
    }
  } catch (const std::exception &error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
