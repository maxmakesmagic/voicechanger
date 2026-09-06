// Teensy 4.x + Audio Shield: Mic -> FFT phase-vocoder pitch shift -> Line Out

#include <Arduino.h>
#include <Audio.h>
#include <VoiceChangerCommon.h>

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

constexpr voicechanger::AudioShieldMicrophoneConfig MICROPHONE_CONFIG = {
  40U,
  0.5f
};

voicechanger::AudioUsageReporter usageReporter;

void setup() {
  Serial.begin(9600);
  AudioMemory(20);

  voicechanger::configureAudioShieldMicrophone(sgtl5000_1,
                                                MICROPHONE_CONFIG);

  pitchShift.setPitchRatio(PITCH_RATIO);
}

void loop() {
  // The per-effect peak captures the expensive alternate updates that call
  // processFrame(); a single current-usage sample may miss them.
  usageReporter.reportIfDue(pitchShift, "Pitch");
}
