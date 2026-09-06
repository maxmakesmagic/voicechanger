# Teensy voice changer

This repository contains real-time voice effects for Teensy 4.x and the Teensy
Audio Shield. Each effect lives in its own top-level Arduino sketch directory;
the supporting test, CI, and editor configuration live in the folders described
below.

## [`pitchshifter/`](pitchshifter/)

An FFT phase-vocoder pitch shifter for Teensy 4.x and the Audio Shield. The
sketch routes the microphone input through `AudioEffectPitchShiftFFT` and sends
the result to both line-output channels.

- `pitchshifter.ino` configures the Audio Shield, signal path, pitch ratio, and
  runtime CPU/audio-memory reporting.
- `PitchShiftFFT.h` and `PitchShiftFFT.cpp` implement the streaming pitch-shift
  effect.
- `PITCH_RATIO` in `pitchshifter.ino` controls the effect: values above `1.0`
  shift upward and values below `1.0` shift downward.

### Compile locally

Install Arduino CLI 1.5.1, then install the pinned Teensy core:

```sh
arduino-cli core update-index \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core install teensy:avr@1.62.0 \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
```

Compile the sketch for either supported board:

```sh
arduino-cli compile --warnings all \
  --fqbn teensy:avr:teensy40:usb=serial,speed=600,opt=o2std,keys=en-us \
  pitchshifter

arduino-cli compile --warnings all \
  --fqbn teensy:avr:teensy41:usb=serial,speed=600,opt=o2std,keys=en-us \
  pitchshifter
```

## [`tests/`](tests/)

The native test target compiles the production `PitchShiftFFT.cpp` against
small host implementations of the Teensy audio-block API and CMSIS FFT in
`tests/support/`. It then drives the effect through its public streaming
`update()` interface.

The tests currently verify:

- compile-time FFT, hop-size, audio-block, and FIFO invariants;
- silence and missing-input behavior;
- rejection of non-finite and non-positive pitch ratios without disturbing the
  last valid setting;
- broadband unity-ratio reconstruction after the expected pipeline latency,
  with better than 60 dB SNR and tightly bounded gain error;
- correct signs for the real-only DC and Nyquist FFT bins;
- pitch movement for bin-centered tones at ratios `0.8` and `1.25`;
- pitch accuracy for a tone that lies between FFT bins;
- suppression, rather than aliasing, when an upward-shifted tone exceeds
  Nyquist; and
- memory and undefined-behavior checks under AddressSanitizer and UBSan.

Run the native tests with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To match the sanitizer-enabled CI job, add
`-DVOICECHANGER_ENABLE_SANITIZERS=ON` to the configure command.

These tests currently cover only the pitch shifter. Listening quality, Audio
Shield I/O, and real-time CPU headroom still require hardware testing.

## [`ci/`](ci/)

`compile-arduino-sketches.sh` discovers and compiles every top-level Arduino
sketch directory for a supplied fully qualified board name (FQBN). Each sketch
must follow Arduino's naming rule: `<folder>/<folder>.ino`. This means a newly
added effect is included in Arduino compilation testing without changing the
script or workflow.

After installing Arduino CLI and the Teensy core, run it locally with either
supported board configuration:

```sh
bash ci/compile-arduino-sketches.sh \
  teensy:avr:teensy40:usb=serial,speed=600,opt=o2std,keys=en-us

bash ci/compile-arduino-sketches.sh \
  teensy:avr:teensy41:usb=serial,speed=600,opt=o2std,keys=en-us
```
