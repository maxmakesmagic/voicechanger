cmake_minimum_required(VERSION 3.16)

project(voicechanger LANGUAGES CXX)

# VS Code's C/C++ extension can use this for the native test sources. The .ino
# files use the Teensy fallback configuration in .vscode/c_cpp_properties.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(VOICECHANGER_ENABLE_SANITIZERS
       "Build host targets with AddressSanitizer and UndefinedBehaviorSanitizer"
       OFF)

option(VOICECHANGER_BUILD_BENCHMARKS
       "Build host-side DSP microbenchmarks"
       OFF)

option(VOICECHANGER_BUILD_AUDIO_RENDERER
       "Build the host tool that renders the checked-in audio examples"
       ON)

add_library(voicechanger_common_headers INTERFACE)
target_include_directories(
  voicechanger_common_headers
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/VoiceChangerCommon/src)

function(voicechanger_configure_native_target target)
  target_include_directories(
    ${target}
    PRIVATE
      tests/support
      ${ARGN})

  target_compile_features(${target} PRIVATE cxx_std_17)
  target_link_libraries(${target} PRIVATE voicechanger_common_headers)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Werror)

    if(VOICECHANGER_ENABLE_SANITIZERS)
      target_compile_options(
        ${target}
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
      target_link_options(
        ${target}
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
    endif()
  endif()
endfunction()

if(VOICECHANGER_BUILD_AUDIO_RENDERER)
  add_executable(
    audio_example_renderer
    pitchshifter/PitchShiftFFT.cpp
    chorus/StereoChorus.cpp
    vocoder/Vocoder.cpp
    tests/support/cmsis_fft_stub.cpp
    tools/WavFile.cpp
    tools/render_audio_examples.cpp)

  voicechanger_configure_native_target(
    audio_example_renderer
    pitchshifter
    chorus
    vocoder
    tools)
endif()

if(BUILD_TESTING)
  add_executable(
    common_tests
    tests/test_common.cpp)

  voicechanger_configure_native_target(common_tests)

  add_test(NAME common_tests COMMAND common_tests)
  set_tests_properties(
    common_tests
    PROPERTIES
      TIMEOUT 60)

  add_executable(
    pitch_shift_tests
    pitchshifter/PitchShiftFFT.cpp
    tests/support/cmsis_fft_stub.cpp
    tests/test_pitch_shift.cpp)

  # The support directory supplies host implementations of the Teensy APIs.
  # Keeping PitchShiftFFT.cpp in this target means the tests exercise the same
  # implementation that is compiled into the Arduino sketch.
  voicechanger_configure_native_target(pitch_shift_tests pitchshifter)

  add_test(NAME pitch_shift_tests COMMAND pitch_shift_tests)
  set_tests_properties(
    pitch_shift_tests
    PROPERTIES
      TIMEOUT 60)

  add_executable(
    chorus_tests
    chorus/StereoChorus.cpp
    tests/support/cmsis_fft_stub.cpp
    tests/test_chorus.cpp)

  # Exercise the production chorus through the same AudioStream update method
  # used on the Teensy, including its second-block allocation path.
  voicechanger_configure_native_target(chorus_tests chorus)

  add_test(NAME chorus_tests COMMAND chorus_tests)
  set_tests_properties(
    chorus_tests
    PROPERTIES
      TIMEOUT 60)

  add_executable(
    vocoder_tests
    vocoder/Vocoder.cpp
    tests/support/cmsis_fft_stub.cpp
    tests/test_vocoder.cpp)

  # Exercise the production vocoder, including its internally generated
  # carrier and block-to-STFT buffering, through AudioStream::update().
  voicechanger_configure_native_target(vocoder_tests vocoder)

  add_test(NAME vocoder_tests COMMAND vocoder_tests)
  set_tests_properties(
    vocoder_tests
    PROPERTIES
      TIMEOUT 60)

  if(VOICECHANGER_BUILD_AUDIO_RENDERER)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
      NAME audio_examples_current
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/check-audio-examples.py
        --renderer $<TARGET_FILE:audio_example_renderer>
        --audio-dir ${CMAKE_CURRENT_SOURCE_DIR}/docs/audio)
    set_tests_properties(
      audio_examples_current
      PROPERTIES
        TIMEOUT 180)
  endif()
endif()

if(VOICECHANGER_BUILD_BENCHMARKS)
  add_executable(
    pitch_shift_benchmark
    pitchshifter/PitchShiftFFT.cpp
    tests/support/cmsis_fft_stub.cpp
    benchmarks/benchmark_pitch_shift.cpp)

  voicechanger_configure_native_target(pitch_shift_benchmark pitchshifter)
endif()
