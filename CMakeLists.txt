# aloopvst — the aloop looper, packaged as a cross-platform VST3/AU/CLAP/Standalone
# plugin. The DSP (dsp/*.dsp, effects/home/faust/*.dsp) is vendored byte-identical
# from ../aloop (see docs/CLONE-PARITY-REFERENCE.md) — this project ports the
# native shell (audio_thread.cpp's per-block orchestration, apc_grid.cpp's control
# surface) onto JUCE's AudioProcessor/processBlock model instead of aloop's
# ALSA + SCHED_FIFO thread + Ableton Link + hardware APC Key25 MIDI stack. See
# docs/ARCHITECTURE.md for the full mapping of what carries over vs what a DAW
# host already provides.
# LANGUAGES CXX C (not CXX alone): JUCE's own internal juceaide code-gen tool
# is built via a nested CMake subbuild that requires the C compiler to be
# enabled on the OUTER project too -- CI witnessed this exact failure on both
# macOS and Linux ("CMake Error: Error required internal CMake variable not
# set... Missing variable is: CMAKE_C_COMPILE_OBJECT") when only CXX was
# declared here, even though this project's own sources are all C++.
cmake_minimum_required(VERSION 3.22)
project(aloopvst VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.4
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# The Faust home stack (loop engine + effects, dsp/aloop.dsp = loop.dsp :
# effects_runtime.dsp) compiled to C++ — the SAME faust invocation aloop's own
# src/CMakeLists.txt uses, so the generated code (and therefore the DSP
# behavior) is identical. If faust isn't installed, fall back to a
# pre-generated loop.cpp checked into dsp/generated/ (regenerate it on any
# .dsp change on a machine that has faust: see dsp/generated/README.md).
set(GENERATED_LOOP_CPP "${CMAKE_BINARY_DIR}/loop.cpp")
find_program(FAUST_BIN faust)
if(FAUST_BIN)
  # WORKING_DIRECTORY is REQUIRED here, set to the repo root: dsp/
  # effects_runtime.dsp's component("effects/home/faust/filters.dsp") calls
  # use a path string that is only valid relative to the repo root (the same
  # literal string aloop's own dsp/effects_runtime.dsp uses, byte-identical --
  # see docs/CLONE-PARITY-REFERENCE.md). aloop's own CI (build-binary.yml)
  # invokes faust directly from a shell step whose cwd IS the repo root, so
  # this never surfaced there. CMake's add_custom_command defaults to
  # CMAKE_CURRENT_BINARY_DIR (the build/ dir) as its working directory --
  # WITNESSED locally: faust failed there with "unable to open file
  # effects/home/faust/filters.dsp" (component()'s path resolution combines
  # each -I dir with the LITERAL path string rather than stripping the
  # matching prefix, so -I .../effects/home/faust only ever finds
  # .../effects/home/faust/effects/home/faust/filters.dsp -- the -I flags
  # here are structurally redundant with a correct cwd, not a substitute for
  # one). Fixed by pinning WORKING_DIRECTORY explicitly instead of relying on
  # CMake's default.
  add_custom_command(
    OUTPUT  ${GENERATED_LOOP_CPP}
    COMMAND ${FAUST_BIN} -lang cpp -cn AloopLoopDsp
            -I ${CMAKE_SOURCE_DIR}/dsp -I ${CMAKE_SOURCE_DIR}/effects/home/faust
            ${CMAKE_SOURCE_DIR}/dsp/aloop.dsp -o ${GENERATED_LOOP_CPP}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/dsp/aloop.dsp ${CMAKE_SOURCE_DIR}/dsp/loop.dsp ${CMAKE_SOURCE_DIR}/dsp/effects_runtime.dsp
    COMMENT "Compiling the Faust home stack (dsp/aloop.dsp = loop + runtime effects) to C++")
  add_custom_target(faust_loop DEPENDS ${GENERATED_LOOP_CPP})
else()
  set(FALLBACK_LOOP_CPP "${CMAKE_SOURCE_DIR}/dsp/generated/loop.cpp")
  if(NOT EXISTS ${FALLBACK_LOOP_CPP})
    message(FATAL_ERROR "faust not found and no pre-generated dsp/generated/loop.cpp checked in — install faust or provide the fallback")
  endif()
  configure_file(${FALLBACK_LOOP_CPP} ${GENERATED_LOOP_CPP} COPYONLY)
  add_custom_target(faust_loop DEPENDS ${GENERATED_LOOP_CPP})
endif()

juce_add_plugin(aloopvst
  COMPANY_NAME "aloop"
  PLUGIN_MANUFACTURER_CODE Alop
  PLUGIN_CODE Alvs
  FORMATS VST3 Standalone
  PRODUCT_NAME "aloop"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT TRUE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE)

# NOTE: GENERATED_LOOP_CPP is deliberately NOT listed here. It is pulled in
# via `#include "loop.cpp"` inside Source/FaustShim.h (itself included by
# LooperEngine.h), where the dsp/Meta/UI macros FaustShim.h defines just
# above the #include are in scope. Also listing it as a target_sources entry
# would additionally compile it as its OWN separate translation unit, where
# none of those macros are defined -- this is exactly what broke CI (MSVC
# C2504 "'dsp': base class undefined", C2061 on Meta/UI): loop.cpp was being
# compiled twice, once correctly (via the include) and once without the
# macros it structurally depends on.
target_sources(aloopvst PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/LooperEngine.cpp
  Source/ApcControlSurface.cpp)

target_include_directories(aloopvst PRIVATE
  ${CMAKE_SOURCE_DIR}/Source
  ${CMAKE_SOURCE_DIR}/Source/sampler
  ${CMAKE_SOURCE_DIR}/effects/home/faust
  ${CMAKE_BINARY_DIR})

target_compile_definitions(aloopvst PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(aloopvst PRIVATE
  juce::juce_audio_utils
  juce::juce_dsp)

add_dependencies(aloopvst faust_loop)
