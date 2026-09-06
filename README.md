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

Once flashed, the sketch reports both whole-audio-graph CPU use and the pitch
shifter's own current and one-second peak use. `processFrame()` runs only on
alternate audio updates, so use the `Pitch CPU` peak after warm-up when checking
real-time headroom; values approaching 100% are approaching the audio-block
deadline.

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
- CMSIS-compatible RFFT packing, phase sign, and forward/inverse scaling;
- CMSIS-compatible sine/cosine approximation accuracy across the full wrapped
  phase range;
- pitch movement for bin-centered tones at ratios `0.8` and `1.25`;
- pitch accuracy for a tone that lies between FFT bins;
- explicit state reset and stable per-slice gain after an unrelated signal;
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

## [`benchmarks/`](benchmarks/)

The host benchmark streams deterministic broadband audio through the same
public `update()` interface as the tests. It is intended for repeatable A/B
comparisons of individual C++ changes; its x86 timing is not a substitute for
measuring the CMSIS-DSP build on a Teensy.

Build and run an optimized benchmark without sanitizers:

```sh
cmake -S . -B build-bench \
  -DBUILD_TESTING=OFF \
  -DVOICECHANGER_BUILD_BENCHMARKS=ON \
  -DVOICECHANGER_ENABLE_SANITIZERS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build build-bench --target pitch_shift_benchmark --parallel
./build-bench/pitch_shift_benchmark
```

The benchmark reports the median, median absolute deviation, minimum, and 95th
percentile of repeated trial-average frame times for pitch ratios `0.8`, `1.0`,
and `1.25`. Its throughput factor describes average host throughput, not Teensy
callback-deadline headroom. Keep the machine, compiler, flags, and workload
identical when comparing the same ratio across revisions. CI runs a separate
optimized benchmark, validates its output, adds its results to the workflow
summary, and retains the raw result as an artifact. The timing is deliberately
not a blocking threshold because load on a shared runner is not deterministic.

For the target measurement, flash each revision with the same board, Teensy
core, FQBN, pitch ratio, audio graph, and repeatable input. Discard the first two
seconds, capture at least 30 `Pitch CPU` peak reports, and compare their median,
95th percentile, and maximum. This measures the deadline-critical callback on
the Cortex-M7 rather than extrapolating it from the host.

When comparing with a revision that predates the per-effect CPU report, keep
the same instrumented sketch and swap only the DSP implementation under test.
Changing both would make the measurements incomparable.

## [`ci/`](ci/)

`compile-arduino-sketches.sh` discovers and compiles every top-level Arduino
sketch directory for a supplied fully qualified board name (FQBN). Each sketch
must follow Arduino's naming rule: `<folder>/<folder>.ino`. This means a newly
added effect is included in Arduino compilation testing without changing the
script or workflow.

After each build, the script uses Teensy's own size tool to check FLASH, RAM1,
and RAM2 against the exact per-build-configuration upper bounds in
`ci/arduino-size-budgets.json`. A reduction passes automatically. An increase,
or a new sketch without a budget, fails so that resource growth has to be
reviewed and recorded explicitly. The measured use and remaining budget are
also shown in the GitHub Actions job summary. Budgets use the complete FQBN, so
changing USB or optimization options also requires a deliberate new baseline.

After installing Arduino CLI and the Teensy core, run it locally with either
supported board configuration:

```sh
bash ci/compile-arduino-sketches.sh \
  teensy:avr:teensy40:usb=serial,speed=600,opt=o2std,keys=en-us

bash ci/compile-arduino-sketches.sh \
  teensy:avr:teensy41:usb=serial,speed=600,opt=o2std,keys=en-us
```
