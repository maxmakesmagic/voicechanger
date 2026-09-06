// Teensy 4.x + Audio Shield: Mic -> FFT phase-vocoder pitch shift -> Line Out

#include <Arduino.h>
#include <Audio.h>

#include "PitchShiftFFT.h"

AudioInputI2S           audioInput;
AudioEffectPitchShiftFFT pitchShift;
AudioOutputI2S          audioOutput;
AudioControlSGTL5000    sgtl5000_1;

AudioConnection patchCord1(audioInput, 0, pitchShift, 0);
AudioConnection patchCord2(pitchShift, 0, audioOutput, 0);
AudioConnection patchCord3(pitchShift, 0, audioOutput, 1);

const float PITCH_RATIO = 1.25f; // >1.0 = shift up, <1.0 = shift down
// const float PITCH_RATIO = 0.8f;

elapsedMillis usageReportTimer;

void setup() {
  Serial.begin(9600);
  AudioMemory(20);

  sgtl5000_1.enable();
  sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
  sgtl5000_1.micGain(40);
  sgtl5000_1.volume(0.5);

  pitchShift.setPitchRatio(PITCH_RATIO);
}

void loop() {
  // Snapshot CPU/memory usage every second so serial printing cannot race the
  // audio ISR. The per-effect peak captures the expensive alternate updates
  // that call processFrame(); a single current-usage sample may miss them.
  if (usageReportTimer >= 1000) {
    usageReportTimer = 0;
    AudioNoInterrupts();
    const float pitchCpu = pitchShift.processorUsage();
    const float pitchCpuMax = pitchShift.processorUsageMax();
    const float totalCpu = AudioProcessorUsage();
    const float totalCpuMax = AudioProcessorUsageMax();
    const unsigned int memory = AudioMemoryUsage();
    const unsigned int memoryMax = AudioMemoryUsageMax();
    pitchShift.processorUsageMaxReset();
    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
    AudioInterrupts();

    Serial.print("Pitch CPU: ");
    Serial.print(pitchCpu);
    Serial.print("% (peak ");
    Serial.print(pitchCpuMax);
    Serial.print("%)  Total CPU: ");
    Serial.print(totalCpu);
    Serial.print("% (peak ");
    Serial.print(totalCpuMax);
    Serial.print("%)  Memory: ");
    Serial.print(memory);
    Serial.print(" (max ");
    Serial.print(memoryMax);
    Serial.println(")");
  }
}
