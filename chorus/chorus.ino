// Teensy 4.x + Audio Shield: microphone -> stereo chorus -> headphones/line out

#include <Arduino.h>
#include <Audio.h>
#include <VoiceChangerCommon.h>

#include "StereoChorus.h"

AudioInputI2S             audioInput;
AudioEffectStereoChorus   chorusEffect;
AudioOutputI2S            audioOutput;
AudioControlSGTL5000      sgtl5000_1;

AudioConnection patchCord1(audioInput, 0, chorusEffect, 0);
AudioConnection patchCord2(chorusEffect, 0, audioOutput, 0);
AudioConnection patchCord3(chorusEffect, 1, audioOutput, 1);

constexpr float CHORUS_DELAY_MS = AudioEffectStereoChorus::DEFAULT_DELAY_MS;
constexpr float CHORUS_DEPTH_MS = AudioEffectStereoChorus::DEFAULT_DEPTH_MS;
constexpr float CHORUS_RATE_HZ = AudioEffectStereoChorus::DEFAULT_RATE_HZ;
constexpr float CHORUS_WET_MIX = AudioEffectStereoChorus::DEFAULT_WET_MIX;

constexpr voicechanger::AudioShieldMicrophoneConfig MICROPHONE_CONFIG = {
  40U,
  0.5f
};

voicechanger::AudioUsageReporter usageReporter;

void setup()
{
  Serial.begin(9600);
  AudioMemory(20);

  voicechanger::configureAudioShieldMicrophone(sgtl5000_1,
                                                MICROPHONE_CONFIG);

  chorusEffect.configure(CHORUS_DELAY_MS,
                         CHORUS_DEPTH_MS,
                         CHORUS_RATE_HZ,
                         CHORUS_WET_MIX);
}

void loop()
{
  usageReporter.reportIfDue(chorusEffect, "Chorus");
}
