#ifndef VOICECHANGER_TOOLS_WAV_FILE_H_
#define VOICECHANGER_TOOLS_WAV_FILE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voicechanger::wav {

// Interleaved signed 16-bit PCM samples. A frame contains one sample for each
// channel.
struct AudioData {
  std::uint32_t sampleRate = 0;
  std::uint16_t channels = 0;
  std::vector<std::int16_t> samples;

  std::size_t frameCount() const;
};

// Read a RIFF/WAVE file containing uncompressed signed 16-bit PCM. Unknown
// RIFF chunks are skipped, including their required odd-byte padding.
AudioData readPcm16(const std::string &path);

// Write a deterministic canonical RIFF/WAVE file: a 16-byte PCM fmt chunk
// followed by one data chunk, with every integer encoded little-endian.
void writePcm16(const std::string &path, const AudioData &audio);

} // namespace voicechanger::wav

#endif
