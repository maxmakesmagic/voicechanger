#include "WavFile.h"

#include "PitchShiftFFT.h"
#include "StereoChorus.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRequiredSampleRate = 44100;

static_assert(AUDIO_BLOCK_SAMPLES == 128,
              "the example renderer requires Teensy's 128-sample blocks");
static_assert(AUDIO_SAMPLE_RATE_EXACT ==
                static_cast<float>(kRequiredSampleRate),
              "the example renderer's WAV rate must match the DSP rate");
static_assert((AudioEffectPitchShiftFFT::FFT_SIZE - AUDIO_BLOCK_SAMPLES) %
                AUDIO_BLOCK_SAMPLES == 0,
              "pitch-shifter latency must contain whole audio blocks");

class UsageError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class AudioStreamStateGuard {
public:
  AudioStreamStateGuard()
  {
    AudioStream::resetTestState();
  }

  ~AudioStreamStateGuard()
  {
    AudioStream::resetTestState();
  }

  AudioStreamStateGuard(const AudioStreamStateGuard &) = delete;
  AudioStreamStateGuard &operator=(const AudioStreamStateGuard &) = delete;
};

void printUsage(const char *program)
{
  std::cerr
    << "Usage:\n"
    << "  " << program << " pitch --ratio R INPUT OUTPUT\n"
    << "  " << program
    << " chorus --delay-ms D --depth-ms D --rate-hz R --wet W INPUT OUTPUT\n";
}

float parseNumber(const std::string &text, const std::string &option)
{
  errno = 0;
  char *end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
      !std::isfinite(value)) {
    throw UsageError(option + " requires a finite number, got '" + text + "'");
  }
  return value;
}

std::size_t roundUpToBlock(std::size_t sampleCount)
{
  constexpr std::size_t blockSize = AUDIO_BLOCK_SAMPLES;
  if (sampleCount >
      std::numeric_limits<std::size_t>::max() - (blockSize - 1U)) {
    throw std::runtime_error("input contains too many samples");
  }
  return ((sampleCount + blockSize - 1U) / blockSize) * blockSize;
}

void requireMono44100(const voicechanger::wav::AudioData &input)
{
  if (input.channels != 1U) {
    throw std::runtime_error("input WAV must be mono PCM16; it has " +
                             std::to_string(input.channels) + " channels");
  }
  if (input.sampleRate != kRequiredSampleRate) {
    throw std::runtime_error("input WAV must use a 44100 Hz sample rate; it uses " +
                             std::to_string(input.sampleRate) + " Hz");
  }
}

void validatePitchOwnership(std::size_t completedBlocks)
{
  if (AudioStream::channelTransmissionCount(0) != completedBlocks ||
      AudioStream::channelTransmissionCount(1) != 0U ||
      AudioStream::transmissionCount() != completedBlocks ||
      AudioStream::releaseCount() != completedBlocks ||
      AudioStream::ownershipErrorCount() != 0U ||
      AudioStream::hasOutstandingBlocks()) {
    throw std::runtime_error(
      "pitch shifter violated AudioStream block ownership while rendering");
  }
}

voicechanger::wav::AudioData renderPitch(
  const voicechanger::wav::AudioData &input,
  float ratio)
{
  if (!(ratio > 0.0f)) {
    throw UsageError("--ratio must be greater than zero");
  }

  constexpr std::size_t latency =
    AudioEffectPitchShiftFFT::FFT_SIZE - AUDIO_BLOCK_SAMPLES;
  const std::size_t paddedInputSamples = roundUpToBlock(input.samples.size());
  if (paddedInputSamples >
      std::numeric_limits<std::size_t>::max() - latency) {
    throw std::runtime_error("input is too long to flush the pitch shifter");
  }
  const std::size_t streamedSamples = paddedInputSamples + latency;

  AudioStreamStateGuard streamState;
  AudioEffectPitchShiftFFT effect;
  effect.setPitchRatio(ratio);

  std::vector<std::int16_t> streamedOutput;
  streamedOutput.reserve(streamedSamples);
  std::size_t completedBlocks = 0;
  for (std::size_t offset = 0; offset < streamedSamples;
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    if (offset < input.samples.size()) {
      const std::size_t available = input.samples.size() - offset;
      const std::size_t copyCount =
        std::min<std::size_t>(AUDIO_BLOCK_SAMPLES, available);
      std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                  copyCount,
                  block.data);
    }

    AudioStream::queueTestInput(&block);
    effect.update();
    ++completedBlocks;
    const audio_block_t *output = AudioStream::transmittedSnapshot(0);
    if (output == nullptr) {
      throw std::runtime_error(
        "pitch shifter did not transmit an output audio block");
    }
    streamedOutput.insert(streamedOutput.end(),
                          std::begin(output->data),
                          std::end(output->data));
    validatePitchOwnership(completedBlocks);
  }

  if (latency > streamedOutput.size() ||
      input.samples.size() > streamedOutput.size() - latency) {
    throw std::runtime_error("pitch shifter did not produce enough output");
  }

  voicechanger::wav::AudioData result;
  result.sampleRate = kRequiredSampleRate;
  result.channels = 1;
  result.samples.assign(
    streamedOutput.begin() + static_cast<std::ptrdiff_t>(latency),
    streamedOutput.begin() + static_cast<std::ptrdiff_t>(
                               latency + input.samples.size()));
  return result;
}

void validateChorusOwnership(std::size_t completedBlocks, bool usesWetBlock)
{
  const std::size_t expectedReleases =
    completedBlocks * (usesWetBlock ? 2U : 1U);
  if (AudioStream::channelTransmissionCount(0) != completedBlocks ||
      AudioStream::channelTransmissionCount(1) != completedBlocks ||
      AudioStream::transmissionCount() != 2U * completedBlocks ||
      AudioStream::releaseCount() != expectedReleases ||
      AudioStream::ownershipErrorCount() != 0U ||
      AudioStream::hasOutstandingBlocks()) {
    throw std::runtime_error(
      "chorus violated AudioStream block ownership while rendering");
  }
}

voicechanger::wav::AudioData renderChorus(
  const voicechanger::wav::AudioData &input,
  float delayMs,
  float depthMs,
  float rateHz,
  float wetMix)
{
  AudioStreamStateGuard streamState;
  AudioEffectStereoChorus effect;
  if (!effect.configure(delayMs, depthMs, rateHz, wetMix)) {
    throw UsageError(
      "invalid chorus settings: delay must be positive, depth non-negative, "
      "rate between 0 and 5 Hz, wet between 0 and 1, and the complete delay "
      "sweep must fit the effect's delay line");
  }

  const std::size_t streamedSamples = roundUpToBlock(input.samples.size());
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  left.reserve(streamedSamples);
  right.reserve(streamedSamples);
  std::size_t completedBlocks = 0;
  for (std::size_t offset = 0; offset < streamedSamples;
       offset += AUDIO_BLOCK_SAMPLES) {
    audio_block_t block{};
    if (offset < input.samples.size()) {
      const std::size_t available = input.samples.size() - offset;
      const std::size_t copyCount =
        std::min<std::size_t>(AUDIO_BLOCK_SAMPLES, available);
      std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                  copyCount,
                  block.data);
    }

    AudioStream::queueTestInput(&block);
    effect.update();
    ++completedBlocks;
    const audio_block_t *leftBlock = AudioStream::transmittedSnapshot(0);
    const audio_block_t *rightBlock = AudioStream::transmittedSnapshot(1);
    if (leftBlock == nullptr || rightBlock == nullptr) {
      throw std::runtime_error("chorus did not transmit both output channels");
    }
    left.insert(left.end(),
                std::begin(leftBlock->data),
                std::end(leftBlock->data));
    right.insert(right.end(),
                 std::begin(rightBlock->data),
                 std::end(rightBlock->data));
    validateChorusOwnership(completedBlocks, wetMix != 0.0f);
  }

  voicechanger::wav::AudioData result;
  result.sampleRate = kRequiredSampleRate;
  result.channels = 2;
  if (input.samples.size() >
      std::numeric_limits<std::size_t>::max() / result.channels) {
    throw std::runtime_error("chorus output contains too many samples");
  }
  result.samples.reserve(input.samples.size() * 2U);
  for (std::size_t frame = 0; frame < input.samples.size(); ++frame) {
    result.samples.push_back(left[frame]);
    result.samples.push_back(right[frame]);
  }
  return result;
}

void renderPitchCommand(int argc, char **argv)
{
  if (argc != 6 || std::string(argv[2]) != "--ratio") {
    throw UsageError("pitch expects --ratio R followed by INPUT and OUTPUT");
  }
  const float ratio = parseNumber(argv[3], "--ratio");
  const voicechanger::wav::AudioData input =
    voicechanger::wav::readPcm16(argv[4]);
  requireMono44100(input);
  const voicechanger::wav::AudioData output = renderPitch(input, ratio);
  voicechanger::wav::writePcm16(argv[5], output);
  std::cout << "Rendered " << output.frameCount()
            << " mono pitch-shifted frames to " << argv[5] << '\n';
}

void renderChorusCommand(int argc, char **argv)
{
  if (argc != 12 || std::string(argv[2]) != "--delay-ms" ||
      std::string(argv[4]) != "--depth-ms" ||
      std::string(argv[6]) != "--rate-hz" ||
      std::string(argv[8]) != "--wet") {
    throw UsageError(
      "chorus expects --delay-ms D --depth-ms D --rate-hz R --wet W "
      "followed by INPUT and OUTPUT");
  }

  const float delayMs = parseNumber(argv[3], "--delay-ms");
  const float depthMs = parseNumber(argv[5], "--depth-ms");
  const float rateHz = parseNumber(argv[7], "--rate-hz");
  const float wetMix = parseNumber(argv[9], "--wet");
  const voicechanger::wav::AudioData input =
    voicechanger::wav::readPcm16(argv[10]);
  requireMono44100(input);
  const voicechanger::wav::AudioData output =
    renderChorus(input, delayMs, depthMs, rateHz, wetMix);
  voicechanger::wav::writePcm16(argv[11], output);
  std::cout << "Rendered " << output.frameCount()
            << " stereo chorus frames to " << argv[11] << '\n';
}

} // namespace

int main(int argc, char **argv)
{
  try {
    if (argc < 2) {
      throw UsageError("missing effect name");
    }

    const std::string effect = argv[1];
    if (effect == "pitch") {
      renderPitchCommand(argc, argv);
    } else if (effect == "chorus") {
      renderChorusCommand(argc, argv);
    } else {
      throw UsageError("unknown effect '" + effect + "'");
    }
    return 0;
  } catch (const UsageError &error) {
    std::cerr << "error: " << error.what() << '\n';
    printUsage(argv[0]);
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
  }
  return 1;
}
