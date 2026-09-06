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

if(BUILD_TESTING)
  add_executable(
    pitch_shift_tests
    pitchshifter/PitchShiftFFT.cpp
    tests/support/cmsis_fft_stub.cpp
    tests/test_pitch_shift.cpp)

  # The support directory supplies host implementations of the Teensy APIs.
  # Keeping PitchShiftFFT.cpp in this target means the tests exercise the same
  # implementation that is compiled into the Arduino sketch.
  target_include_directories(
    pitch_shift_tests
    PRIVATE
      tests/support
      pitchshifter)

  target_compile_features(pitch_shift_tests PRIVATE cxx_std_17)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      pitch_shift_tests
      PRIVATE -Wall -Wextra -Wpedantic -Werror)

    if(VOICECHANGER_ENABLE_SANITIZERS)
      target_compile_options(
        pitch_shift_tests
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
      target_link_options(
        pitch_shift_tests
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
    endif()
  endif()

  add_test(NAME pitch_shift_tests COMMAND pitch_shift_tests)
  set_tests_properties(
    pitch_shift_tests
    PROPERTIES
      TIMEOUT 60)
endif()

if(VOICECHANGER_BUILD_BENCHMARKS)
  add_executable(
    pitch_shift_benchmark
    pitchshifter/PitchShiftFFT.cpp
    tests/support/cmsis_fft_stub.cpp
    benchmarks/benchmark_pitch_shift.cpp)

  target_include_directories(
    pitch_shift_benchmark
    PRIVATE
      tests/support
      pitchshifter)

  target_compile_features(pitch_shift_benchmark PRIVATE cxx_std_17)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      pitch_shift_benchmark
      PRIVATE -Wall -Wextra -Wpedantic -Werror)

    if(VOICECHANGER_ENABLE_SANITIZERS)
      target_compile_options(
        pitch_shift_benchmark
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
      target_link_options(
        pitch_shift_benchmark
        PRIVATE
          -fno-omit-frame-pointer
          -fsanitize=address,undefined,float-cast-overflow)
    endif()
  endif()
endif()
