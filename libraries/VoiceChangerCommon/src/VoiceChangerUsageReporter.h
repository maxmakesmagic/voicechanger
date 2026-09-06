#ifndef VOICE_CHANGER_USAGE_REPORTER_H_
#define VOICE_CHANGER_USAGE_REPORTER_H_

#include <Arduino.h>
#include <Audio.h>

namespace voicechanger {

/**
 * Periodically report one effect's usage alongside the complete audio graph.
 *
 * The usage snapshot and peak-counter resets happen together while Audio
 * Library updates are paused. Serial output happens afterwards so it cannot
 * lengthen the audio interrupt's disabled period.
 */
class AudioUsageReporter {
public:
  AudioUsageReporter() = default;

  template <typename Effect>
  bool reportIfDue(Effect &effect,
                   const char *effectLabel,
                   unsigned long intervalMilliseconds = 1000UL,
                   Print &output = Serial)
  {
    if (timer_ < intervalMilliseconds) {
      return false;
    }
    timer_ = 0;

    AudioNoInterrupts();
    const UsageSnapshot snapshot = {
      effect.processorUsage(),
      effect.processorUsageMax(),
      AudioProcessorUsage(),
      AudioProcessorUsageMax(),
      AudioMemoryUsage(),
      AudioMemoryUsageMax()
    };
    effect.processorUsageMaxReset();
    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
    AudioInterrupts();

    print(snapshot, effectLabel, output);
    return true;
  }

private:
  struct UsageSnapshot {
    float effectCpu;
    float effectCpuMax;
    float totalCpu;
    float totalCpuMax;
    unsigned int memory;
    unsigned int memoryMax;
  };

  static void print(const UsageSnapshot &snapshot,
                    const char *effectLabel,
                    Print &output)
  {
    output.print(effectLabel);
    output.print(" CPU: ");
    output.print(snapshot.effectCpu);
    output.print("% (peak ");
    output.print(snapshot.effectCpuMax);
    output.print("%)  Total CPU: ");
    output.print(snapshot.totalCpu);
    output.print("% (peak ");
    output.print(snapshot.totalCpuMax);
    output.print("%)  Memory: ");
    output.print(snapshot.memory);
    output.print(" (max ");
    output.print(snapshot.memoryMax);
    output.println(")");
  }

  elapsedMillis timer_;
};

static_assert(sizeof(AudioUsageReporter) == sizeof(elapsedMillis),
              "usage reporter must store only its timer");

} // namespace voicechanger

#endif
