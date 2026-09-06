#ifndef TEST_SUPPORT_AUDIO_STREAM_H_
#define TEST_SUPPORT_AUDIO_STREAM_H_

#include <cstddef>
#include <cstdint>

constexpr int AUDIO_BLOCK_SAMPLES = 128;
constexpr float AUDIO_SAMPLE_RATE_EXACT = 44100.0f;

struct audio_block_t {
  int16_t data[AUDIO_BLOCK_SAMPLES]{};
};

// Minimal host double for the part of Teensy's AudioStream API used by the
// effects. Tests can queue one writable block, inspect each output channel,
// and force AudioMemory allocation failure.
class AudioStream {
public:
  static constexpr std::size_t TEST_OUTPUT_CHANNELS = 4;

  AudioStream(unsigned char, audio_block_t **) {}
  virtual ~AudioStream() = default;
  virtual void update() = 0;

  static void queueTestInput(audio_block_t *block)
  {
    if (queuedInput_ != nullptr || inputOutstanding_) {
      ++ownershipErrorCount_;
    }
    queuedInput_ = block;
  }

  static void resetTestState()
  {
    queuedInput_ = nullptr;
    for (std::size_t channel = 0; channel < TEST_OUTPUT_CHANNELS; ++channel) {
      lastTransmitted_[channel] = nullptr;
      channelTransmissionCount_[channel] = 0;
      transmittedSnapshot_[channel] = audio_block_t{};
    }
    transmissionCount_ = 0;
    releaseCount_ = 0;
    ownershipErrorCount_ = 0;
    allocationFailure_ = false;
    allocated_ = false;
    inputOutstanding_ = false;
    receivedInput_ = nullptr;
    allocatedBlock_ = audio_block_t{};
  }

  static const audio_block_t *lastTransmitted(std::size_t channel = 0)
  {
    return channel < TEST_OUTPUT_CHANNELS ? lastTransmitted_[channel]
                                         : nullptr;
  }

  static std::size_t channelTransmissionCount(std::size_t channel)
  {
    return channel < TEST_OUTPUT_CHANNELS
             ? channelTransmissionCount_[channel]
             : 0;
  }

  static const audio_block_t *transmittedSnapshot(std::size_t channel)
  {
    return channel < TEST_OUTPUT_CHANNELS &&
             channelTransmissionCount_[channel] != 0
             ? &transmittedSnapshot_[channel]
             : nullptr;
  }

  static std::size_t transmissionCount()
  {
    return transmissionCount_;
  }

  static std::size_t releaseCount()
  {
    return releaseCount_;
  }

  static std::size_t ownershipErrorCount()
  {
    return ownershipErrorCount_;
  }

  static bool hasOutstandingBlocks()
  {
    return queuedInput_ != nullptr || inputOutstanding_ || allocated_;
  }

  static void setTestAllocationFailure(bool fail)
  {
    allocationFailure_ = fail;
  }

protected:
  audio_block_t *receiveWritable(unsigned int)
  {
    audio_block_t *block = queuedInput_;
    queuedInput_ = nullptr;
    if (block != nullptr) {
      if (inputOutstanding_) {
        ++ownershipErrorCount_;
      }
      receivedInput_ = block;
      inputOutstanding_ = true;
    }
    return block;
  }

  static audio_block_t *allocate()
  {
    if (allocationFailure_ || allocated_) {
      if (allocated_ && !allocationFailure_) {
        ++ownershipErrorCount_;
      }
      return nullptr;
    }
    allocated_ = true;
    for (int16_t &sample : allocatedBlock_.data) {
      sample = static_cast<int16_t>(0x5A5A);
    }
    return &allocatedBlock_;
  }

  void transmit(audio_block_t *block, unsigned char channel = 0)
  {
    const bool ownsInput = block == receivedInput_ && inputOutstanding_;
    const bool ownsAllocation = block == &allocatedBlock_ && allocated_;
    if (block == nullptr || (!ownsInput && !ownsAllocation)) {
      ++ownershipErrorCount_;
    }
    if (channel < TEST_OUTPUT_CHANNELS) {
      lastTransmitted_[channel] = block;
      ++channelTransmissionCount_[channel];
      if (block != nullptr) {
        transmittedSnapshot_[channel] = *block;
      }
    }
    ++transmissionCount_;
  }

  static void release(audio_block_t *block)
  {
    if (block == receivedInput_ && inputOutstanding_) {
      inputOutstanding_ = false;
    } else if (block == &allocatedBlock_ && allocated_) {
      allocated_ = false;
    } else {
      ++ownershipErrorCount_;
    }
    ++releaseCount_;
  }

private:
  inline static audio_block_t *queuedInput_ = nullptr;
  inline static audio_block_t *lastTransmitted_[TEST_OUTPUT_CHANNELS]{};
  inline static std::size_t
    channelTransmissionCount_[TEST_OUTPUT_CHANNELS]{};
  inline static audio_block_t transmittedSnapshot_[TEST_OUTPUT_CHANNELS]{};
  inline static std::size_t transmissionCount_ = 0;
  inline static std::size_t releaseCount_ = 0;
  inline static std::size_t ownershipErrorCount_ = 0;
  inline static audio_block_t allocatedBlock_{};
  inline static audio_block_t *receivedInput_ = nullptr;
  inline static bool allocationFailure_ = false;
  inline static bool allocated_ = false;
  inline static bool inputOutstanding_ = false;
};

#endif
