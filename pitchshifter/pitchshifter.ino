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
  // Print CPU/memory usage every second so you can confirm this fits in
  // real time on your hardware (watch for CPU usage climbing toward 100%).
  if (usageReportTimer >= 1000) {
    usageReportTimer = 0;
    Serial.print("CPU: ");
    Serial.print(AudioProcessorUsage());
    Serial.print("% (max ");
    Serial.print(AudioProcessorUsageMax());
    Serial.print("%)  Memory: ");
    Serial.print(AudioMemoryUsage());
    Serial.print(" (max ");
    Serial.print(AudioMemoryUsageMax());
    Serial.println(")");
  }
}
