#ifndef TEST_SUPPORT_AUDIO_STREAM_H_
#define TEST_SUPPORT_AUDIO_STREAM_H_

#include <cstddef>
#include <cstdint>

constexpr int AUDIO_BLOCK_SAMPLES = 128;

struct audio_block_t {
  int16_t data[AUDIO_BLOCK_SAMPLES]{};
};

// Minimal host double for the part of Teensy's AudioStream API used by the
// effect. A test queues one writable block, calls update(), and then reads the
// same block after transmit().
class AudioStream {
public:
  AudioStream(unsigned char, audio_block_t **) {}
  virtual ~AudioStream() = default;
  virtual void update() = 0;

  static void queueTestInput(audio_block_t *block)
  {
    queuedInput_ = block;
  }

  static void resetTestState()
  {
    queuedInput_ = nullptr;
    lastTransmitted_ = nullptr;
    transmissionCount_ = 0;
    releaseCount_ = 0;
  }

  static const audio_block_t *lastTransmitted()
  {
    return lastTransmitted_;
  }

  static std::size_t transmissionCount()
  {
    return transmissionCount_;
  }

  static std::size_t releaseCount()
  {
    return releaseCount_;
  }

protected:
  audio_block_t *receiveWritable(unsigned int)
  {
    audio_block_t *block = queuedInput_;
    queuedInput_ = nullptr;
    return block;
  }

  void transmit(audio_block_t *block, unsigned char = 0)
  {
    lastTransmitted_ = block;
    ++transmissionCount_;
  }

  void release(audio_block_t *)
  {
    ++releaseCount_;
  }

private:
  inline static audio_block_t *queuedInput_ = nullptr;
  inline static audio_block_t *lastTransmitted_ = nullptr;
  inline static std::size_t transmissionCount_ = 0;
  inline static std::size_t releaseCount_ = 0;
};

#endif
