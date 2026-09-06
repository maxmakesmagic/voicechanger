#ifndef VOICE_CHANGER_AUDIO_SHIELD_H_
#define VOICE_CHANGER_AUDIO_SHIELD_H_

#include <Audio.h>

namespace voicechanger {

/** Settings shared by sketches that use an electret microphone input. */
struct AudioShieldMicrophoneConfig {
  unsigned int gainDb;
  float outputVolume;
};

/**
 * Enable a Teensy Audio Shield and select its microphone input.
 *
 * Every codec operation is attempted even if an earlier one fails. The return
 * value is true only when all four operations report success.
 */
inline bool configureAudioShieldMicrophone(
  AudioControlSGTL5000 &shield,
  const AudioShieldMicrophoneConfig &config)
{
  const bool enabled = shield.enable();
  const bool microphoneSelected = shield.inputSelect(AUDIO_INPUT_MIC);
  const bool gainSet = shield.micGain(config.gainDb);
  const bool volumeSet = shield.volume(config.outputVolume);
  return enabled && microphoneSelected && gainSet && volumeSet;
}

} // namespace voicechanger

#endif
