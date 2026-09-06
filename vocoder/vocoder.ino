// Teensy 4.x + Audio Shield: microphone -> spectral vocoder -> headphones

#include <Arduino.h>
#include <Audio.h>
#include <VoiceChangerCommon.h>

#include "Vocoder.h"

AudioInputI2S         audioInput;
AudioEffectVocoder    vocoderEffect;
AudioOutputI2S        audioOutput;
AudioControlSGTL5000  sgtl5000_1;

AudioConnection patchCord1(audioInput, 0, vocoderEffect, 0);
AudioConnection patchCord2(vocoderEffect, 0, audioOutput, 0);
AudioConnection patchCord3(vocoderEffect, 0, audioOutput, 1);

constexpr float VOCODER_CARRIER_HZ = AudioEffectVocoder::DEFAULT_CARRIER_HZ;
constexpr float VOCODER_ATTACK_MS = AudioEffectVocoder::DEFAULT_ATTACK_MS;
constexpr float VOCODER_RELEASE_MS = AudioEffectVocoder::DEFAULT_RELEASE_MS;
constexpr float VOCODER_NOISE_MIX = AudioEffectVocoder::DEFAULT_NOISE_MIX;
constexpr float VOCODER_GATE_DBFS =
  AudioEffectVocoder::DEFAULT_GATE_THRESHOLD_DBFS;

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
  vocoderEffect.configure(VOCODER_CARRIER_HZ,
                          VOCODER_ATTACK_MS,
                          VOCODER_RELEASE_MS,
                          VOCODER_NOISE_MIX,
                          VOCODER_GATE_DBFS);
}

void loop()
{
  usageReporter.reportIfDue(vocoderEffect, "Vocoder");
}
