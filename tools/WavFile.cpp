#include "WavFile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voicechanger::wav {
namespace {

constexpr std::size_t kRiffHeaderBytes = 12;
constexpr std::size_t kPcmFormatBytes = 16;
constexpr std::uint16_t kPcmFormatTag = 1;
constexpr std::uint16_t kBitsPerSample = 16;

[[noreturn]] void fail(const std::string &path, const std::string &message)
{
  throw std::runtime_error(path + ": " + message);
}

bool hasFourCc(const std::vector<std::uint8_t> &bytes,
               std::size_t offset,
               const char (&expected)[5])
{
  return offset <= bytes.size() && bytes.size() - offset >= 4U &&
         std::equal(expected, expected + 4, bytes.begin() +
                                             static_cast<std::ptrdiff_t>(offset));
}

std::uint16_t readU16(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset)
{
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(
           static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t readU32(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset)
{
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void writeBytes(std::ofstream &output,
                const char *bytes,
                std::size_t count,
                const std::string &path)
{
  output.write(bytes, static_cast<std::streamsize>(count));
  if (!output) {
    fail(path, "could not write WAV data");
  }
}

void writeFourCc(std::ofstream &output,
                 const char (&value)[5],
                 const std::string &path)
{
  writeBytes(output, value, 4U, path);
}

void writeU16(std::ofstream &output,
              std::uint16_t value,
              const std::string &path)
{
  const std::array<char, 2> bytes = {
    static_cast<char>(value & 0xFFU),
    static_cast<char>((value >> 8U) & 0xFFU),
  };
  writeBytes(output, bytes.data(), bytes.size(), path);
}

void writeU32(std::ofstream &output,
              std::uint32_t value,
              const std::string &path)
{
  const std::array<char, 4> bytes = {
    static_cast<char>(value & 0xFFU),
    static_cast<char>((value >> 8U) & 0xFFU),
    static_cast<char>((value >> 16U) & 0xFFU),
    static_cast<char>((value >> 24U) & 0xFFU),
  };
  writeBytes(output, bytes.data(), bytes.size(), path);
}

} // namespace

std::size_t AudioData::frameCount() const
{
  return channels == 0U ? 0U : samples.size() / channels;
}

AudioData readPcm16(const std::string &path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    fail(path, "could not open input WAV file");
  }

  const std::streampos endPosition = input.tellg();
  if (endPosition < 0) {
    fail(path, "could not determine input WAV size");
  }
  const auto fileSize = static_cast<std::uint64_t>(endPosition);
  if (fileSize > static_cast<std::uint64_t>(
                   std::numeric_limits<std::uint32_t>::max()) + 8U) {
    fail(path, "file is too large for the RIFF/WAVE format");
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
  input.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  if (!input && !bytes.empty()) {
    fail(path, "could not read the complete input WAV file");
  }

  if (bytes.size() < kRiffHeaderBytes || !hasFourCc(bytes, 0, "RIFF") ||
      !hasFourCc(bytes, 8, "WAVE")) {
    fail(path, "not a RIFF/WAVE file");
  }

  const std::uint64_t riffEnd64 = 8U + static_cast<std::uint64_t>(readU32(bytes, 4));
  if (riffEnd64 < kRiffHeaderBytes || riffEnd64 > bytes.size()) {
    fail(path, "RIFF size extends beyond the input file");
  }
  const std::size_t riffEnd = static_cast<std::size_t>(riffEnd64);

  bool foundFormat = false;
  std::uint16_t formatTag = 0;
  std::uint16_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t byteRate = 0;
  std::uint16_t blockAlign = 0;
  std::uint16_t bitsPerSample = 0;
  std::vector<std::pair<std::size_t, std::size_t>> dataChunks;

  std::size_t offset = kRiffHeaderBytes;
  while (offset < riffEnd) {
    if (riffEnd - offset < 8U) {
      fail(path, "truncated RIFF chunk header");
    }
    const std::uint32_t chunkSize32 = readU32(bytes, offset + 4U);
    const std::size_t chunkSize = static_cast<std::size_t>(chunkSize32);
    const std::size_t chunkData = offset + 8U;
    if (chunkSize > riffEnd - chunkData) {
      fail(path, "RIFF chunk extends beyond the declared RIFF size");
    }
    const std::size_t chunkEnd = chunkData + chunkSize;

    if (hasFourCc(bytes, offset, "fmt ")) {
      if (foundFormat) {
        fail(path, "contains more than one fmt chunk");
      }
      if (chunkSize < kPcmFormatBytes) {
        fail(path, "fmt chunk is shorter than the PCM header");
      }
      foundFormat = true;
      formatTag = readU16(bytes, chunkData);
      channels = readU16(bytes, chunkData + 2U);
      sampleRate = readU32(bytes, chunkData + 4U);
      byteRate = readU32(bytes, chunkData + 8U);
      blockAlign = readU16(bytes, chunkData + 12U);
      bitsPerSample = readU16(bytes, chunkData + 14U);
    } else if (hasFourCc(bytes, offset, "data")) {
      dataChunks.emplace_back(chunkData, chunkSize);
    }

    const std::size_t padding = chunkSize & 1U;
    if (padding > riffEnd - chunkEnd) {
      fail(path, "odd-sized RIFF chunk is missing its padding byte");
    }
    offset = chunkEnd + padding;
  }

  if (!foundFormat) {
    fail(path, "does not contain a fmt chunk");
  }
  if (dataChunks.empty()) {
    fail(path, "does not contain a data chunk");
  }
  if (formatTag != kPcmFormatTag) {
    fail(path, "audio format is not uncompressed integer PCM");
  }
  if (channels == 0U) {
    fail(path, "PCM channel count is zero");
  }
  if (sampleRate == 0U) {
    fail(path, "PCM sample rate is zero");
  }
  if (bitsPerSample != kBitsPerSample) {
    fail(path, "PCM sample width is not 16 bits");
  }

  const std::uint32_t expectedBlockAlign =
    static_cast<std::uint32_t>(channels) * (kBitsPerSample / 8U);
  const std::uint64_t expectedByteRate =
    static_cast<std::uint64_t>(sampleRate) * expectedBlockAlign;
  if (blockAlign != expectedBlockAlign) {
    fail(path, "PCM block alignment does not match its channel count");
  }
  if (expectedByteRate > std::numeric_limits<std::uint32_t>::max() ||
      byteRate != expectedByteRate) {
    fail(path, "PCM byte rate does not match its format");
  }

  std::size_t sampleCount = 0;
  for (const auto &chunk : dataChunks) {
    if (chunk.second % blockAlign != 0U) {
      fail(path, "data chunk does not contain a whole number of PCM frames");
    }
    const std::size_t chunkSamples = chunk.second / sizeof(std::int16_t);
    if (chunkSamples > std::numeric_limits<std::size_t>::max() - sampleCount) {
      fail(path, "PCM sample count is too large");
    }
    sampleCount += chunkSamples;
  }

  AudioData result;
  result.sampleRate = sampleRate;
  result.channels = channels;
  result.samples.reserve(sampleCount);
  for (const auto &chunk : dataChunks) {
    const std::size_t chunkEnd = chunk.first + chunk.second;
    for (std::size_t position = chunk.first; position < chunkEnd;
         position += 2U) {
      const std::uint16_t encoded = readU16(bytes, position);
      const std::int32_t decoded = encoded < 0x8000U
        ? static_cast<std::int32_t>(encoded)
        : static_cast<std::int32_t>(encoded) - 0x10000;
      result.samples.push_back(static_cast<std::int16_t>(decoded));
    }
  }
  return result;
}

void writePcm16(const std::string &path, const AudioData &audio)
{
  if (audio.channels == 0U) {
    fail(path, "cannot write WAV data with zero channels");
  }
  if (audio.sampleRate == 0U) {
    fail(path, "cannot write WAV data with a zero sample rate");
  }
  if (audio.samples.size() % audio.channels != 0U) {
    fail(path, "sample data does not contain whole audio frames");
  }

  const std::uint32_t blockAlign =
    static_cast<std::uint32_t>(audio.channels) * (kBitsPerSample / 8U);
  const std::uint64_t byteRate64 =
    static_cast<std::uint64_t>(audio.sampleRate) * blockAlign;
  const std::uint64_t dataBytes64 =
    static_cast<std::uint64_t>(audio.samples.size()) * sizeof(std::int16_t);
  const std::uint64_t riffSize64 = 36U + dataBytes64;
  if (blockAlign > std::numeric_limits<std::uint16_t>::max() ||
      byteRate64 > std::numeric_limits<std::uint32_t>::max()) {
    fail(path, "PCM format values exceed the RIFF/WAVE limits");
  }
  if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() ||
      riffSize64 > std::numeric_limits<std::uint32_t>::max()) {
    fail(path, "PCM data is too large for a canonical RIFF/WAVE file");
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    fail(path, "could not open output WAV file");
  }

  const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
  writeFourCc(output, "RIFF", path);
  writeU32(output, static_cast<std::uint32_t>(riffSize64), path);
  writeFourCc(output, "WAVE", path);
  writeFourCc(output, "fmt ", path);
  writeU32(output, static_cast<std::uint32_t>(kPcmFormatBytes), path);
  writeU16(output, kPcmFormatTag, path);
  writeU16(output, audio.channels, path);
  writeU32(output, audio.sampleRate, path);
  writeU32(output, static_cast<std::uint32_t>(byteRate64), path);
  writeU16(output, static_cast<std::uint16_t>(blockAlign), path);
  writeU16(output, kBitsPerSample, path);
  writeFourCc(output, "data", path);
  writeU32(output, dataBytes, path);
  for (const std::int16_t sample : audio.samples) {
    writeU16(output, static_cast<std::uint16_t>(sample), path);
  }

  output.close();
  if (!output) {
    fail(path, "could not finish writing output WAV file");
  }
}

} // namespace voicechanger::wav
