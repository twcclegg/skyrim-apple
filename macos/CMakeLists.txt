cmake_minimum_required(VERSION 3.24)
project(SkyrimJitlessMacOS LANGUAGES C CXX)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  message(FATAL_ERROR "This target requires native Apple Silicon macOS (arm64).")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Native architecture" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
get_filename_component(SKYRIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
option(LB_STRICT_ORDER "Preserve x86 guest memory ordering on ARM64" ON)
option(LB_CONST_MEM "Generated code treats the guest base pointer as a constant" ON)
set(SKYRIM_GEN_FLAGS "" CACHE STRING "Extra compiler flags for the generated code (performance experiments)")
option(LB_PINNED_REGS "Keep the guest registers in fixed host registers in lifted code (arm64; needs generated code from the same lifter)" OFF)
option(SKYRIM_BUILD_GAME "Link the game using privately generated Windows code" OFF)
option(SKYRIM_BUNDLED_SDL "Build the pinned SDL2 source instead of relying on Homebrew SDL2" ON)
option(SKYRIM_FFMPEG_AUDIO "Decode Windows xWMA source voices with private native FFmpeg libraries" ON)
set(SKYRIM_GEN_DIR "${SKYRIM_ROOT}/gen" CACHE PATH "Privately generated code directory")
set(SKYRIM_GAME_DIR "" CACHE PATH "Game folder gen/ is bound to; empty uses the one its manifest records")

if(SKYRIM_BUNDLED_SDL)
  set(SDL_SHARED ON CACHE BOOL "Build local SDL2 library" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "Do not duplicate SDL compilation" FORCE)
  set(SDL_TEST OFF CACHE BOOL "No SDL test library" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "No unrelated SDL tests" FORCE)
  add_subdirectory("${SKYRIM_ROOT}/android/app/jni/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/sdl")
else()
  find_package(SDL2 CONFIG REQUIRED)
endif()
if(NOT TARGET SDL2)
  add_library(SDL2 INTERFACE)
  target_link_libraries(SDL2 INTERFACE SDL2::SDL2)
endif()
get_target_property(_sdl_include SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
foreach(_path IN LISTS _sdl_include)
  if(IS_DIRECTORY "${_path}/../SDL2")
    target_include_directories(SDL2 INTERFACE "${_path}/..")
  endif()
endforeach()
add_subdirectory(graphics)

file(GLOB RT_C CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.c")
list(REMOVE_ITEM RT_C "${SKYRIM_ROOT}/runtime/main.c")
file(GLOB RT_CPP CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.cpp")
add_library(skyrim_runtime OBJECT ${RT_C} ${RT_CPP})
target_include_directories(skyrim_runtime PUBLIC "${SKYRIM_ROOT}/runtime")
target_compile_definitions(skyrim_runtime PUBLIC GAME_SKYRIM DXVK_NATIVE)
if(LB_PINNED_REGS)
  target_compile_definitions(skyrim_runtime PUBLIC LB_PINNED)
endif()
if(LB_STRICT_ORDER)
  target_compile_definitions(skyrim_runtime PUBLIC LB_STRICT_ORDER)
endif()
target_compile_options(skyrim_runtime PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
  $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
target_link_libraries(skyrim_runtime PUBLIC SDL2 dxvk_d3d9)
if(SKYRIM_FFMPEG_AUDIO)
  set(SKYRIM_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg-macos" CACHE PATH "Private minimal macOS FFmpeg prefix")
  foreach(_archive avcodec avutil)
    if(NOT EXISTS "${SKYRIM_FFMPEG_ROOT}/lib/lib${_archive}.a")
      message(FATAL_ERROR "Missing private ${_archive}. Run macos/audio/build_ffmpeg.py macos, or macos/bootstrap.sh.")
    endif()
  endforeach()
  add_library(skyrim_native_audio INTERFACE)
  target_include_directories(skyrim_native_audio INTERFACE "${SKYRIM_FFMPEG_ROOT}/include" "${SKYRIM_ROOT}/runtime")
  target_compile_definitions(skyrim_native_audio INTERFACE SKYRIM_FFMPEG_AUDIO=1)
  target_link_libraries(skyrim_native_audio INTERFACE "${SKYRIM_FFMPEG_ROOT}/lib/libavcodec.a"
    "${SKYRIM_FFMPEG_ROOT}/lib/libavutil.a" m)
  target_link_libraries(skyrim_runtime PUBLIC skyrim_native_audio)
  add_executable(audio_codec_probe audio/codec_probe.c "${SKYRIM_ROOT}/runtime/audio_decode.c")
  target_compile_definitions(audio_codec_probe PRIVATE AUDIO_CODEC_PROBE_MAIN)
  target_include_directories(audio_codec_probe PRIVATE audio)
  target_link_libraries(audio_codec_probe PRIVATE skyrim_native_audio)
endif()

# Compile the genuine game entry separately so the game-free test can link the
# complete runtime without running its PE loader or pretending game code exists.
add_library(skyrim_entry OBJECT "${SKYRIM_ROOT}/runtime/main.c")
target_compile_definitions(skyrim_entry PRIVATE GAME_SKYRIM main=skyrim_guest_main)
target_include_directories(skyrim_entry PRIVATE "${SKYRIM_ROOT}/runtime")
target_compile_options(skyrim_entry PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char)
target_link_libraries(skyrim_entry PRIVATE SDL2)

enable_testing()
if(TARGET audio_codec_probe)
  foreach(_sample musicmono_xwma.wav musicsurround_xwma.wav ergon.xwma)
    string(REPLACE "." "_" _test_name "${_sample}")
    add_test(NAME "audio_${_test_name}" COMMAND audio_codec_probe
      "${CMAKE_CURRENT_SOURCE_DIR}/build/audio-audit/${_sample}")
  endforeach()
endif()
add_executable(runtime_smoke tests/runtime_smoke.c tests/fixture_modules.c
  $<TARGET_OBJECTS:skyrim_entry>)
target_link_libraries(runtime_smoke PRIVATE skyrim_runtime)
add_test(NAME runtime_smoke COMMAND runtime_smoke)
set_tests_properties(runtime_smoke PROPERTIES TIMEOUT 30)
# Desktop keyboard/mouse/controller input and guest thread stacks through the
# real shims (synthetic devices; the test carries its own fixture functions).
add_executable(desktop_platform tests/desktop_platform.c $<TARGET_OBJECTS:skyrim_entry>)
target_link_libraries(desktop_platform PRIVATE skyrim_runtime)
add_test(NAME desktop_platform COMMAND desktop_platform)
set_tests_properties(desktop_platform PROPERTIES TIMEOUT 30
  ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_VIDEODRIVER=dummy")
# XAudio2 output-device recovery after a process suspension or a real stall.
add_executable(audio_device_recovery tests/audio_device_recovery.c tests/fixture_modules.c
  $<TARGET_OBJECTS:skyrim_entry>)
target_link_libraries(audio_device_recovery PRIVATE skyrim_runtime)
add_test(NAME audio_device_recovery COMMAND audio_device_recovery)
set_tests_properties(audio_device_recovery PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_AUDIODRIVER=dummy")
if(SKYRIM_FFMPEG_AUDIO)
  add_executable(audio_voice_probe audio/voice_probe.c tests/fixture_modules.c
    $<TARGET_OBJECTS:skyrim_entry>)
  target_link_libraries(audio_voice_probe PRIVATE skyrim_runtime)
  add_test(NAME audio_voice_queue COMMAND audio_voice_probe
    "${CMAKE_CURRENT_SOURCE_DIR}/build/audio-audit/musicmono_xwma.wav")
  set_tests_properties(audio_voice_queue PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_AUDIODRIVER=dummy")
endif()
if(TARGET graphics_smoke)
  add_test(NAME graphics_smoke COMMAND graphics_smoke)
  set_tests_properties(graphics_smoke PROPERTIES TIMEOUT 60)
endif()
if(TARGET depth_compare_smoke)
  add_test(NAME depth_compare_smoke COMMAND depth_compare_smoke)
  set_tests_properties(depth_compare_smoke PROPERTIES TIMEOUT 60)
endif()
if(TARGET base_vertex_fold_smoke)
  add_test(NAME base_vertex_fold_smoke COMMAND base_vertex_fold_smoke --compare)
  set_tests_properties(base_vertex_fold_smoke PROPERTIES TIMEOUT 120)
endif()

set(SKYRIM_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH
  "Isolated Python with capstone and pefile")
if(EXISTS "${SKYRIM_PYTHON}")
  add_test(NAME input_validation COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_inputs.py")
  add_test(NAME dependency_paths COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dependencies.py")
  add_test(NAME runtime_config COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_config.py")
  add_test(NAME build_binding COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_receipt.py")
  add_test(NAME private_staging COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_staging.py")
  add_test(NAME steam_depot_view COMMAND "${SKYRIM_PYTHON}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_merge_depots.py")
  add_test(NAME aot_instructions COMMAND "${SKYRIM_PYTHON}"
    "${SKYRIM_ROOT}/tests/aot/run.py" --output-dir "${CMAKE_CURRENT_BINARY_DIR}/aot-fixtures")
  add_test(NAME aot_instructions_strict COMMAND "${SKYRIM_PYTHON}"
    "${SKYRIM_ROOT}/tests/aot/run.py" --strict-order --output-dir "${CMAKE_CURRENT_BINARY_DIR}/aot-fixtures-strict")
  add_test(NAME aot_discovery_strict COMMAND "${SKYRIM_PYTHON}"
    "${SKYRIM_ROOT}/tests/aot/run.py" --discover --strict-order --output-dir "${CMAKE_CURRENT_BINARY_DIR}/aot-discovery-strict")
  add_test(NAME darwin_runtime COMMAND "${SKYRIM_PYTHON}"
    "${SKYRIM_ROOT}/tests/macos_runtime/run.py")
  set_tests_properties(aot_instructions aot_instructions_strict aot_discovery_strict darwin_runtime PROPERTIES TIMEOUT 120)
  add_test(NAME jitless_boundary COMMAND "${SKYRIM_PYTHON}"
    "${SKYRIM_ROOT}/tests/jitless/run.py" --build-mapping-probe
    --runtime-bin "$<TARGET_FILE:runtime_smoke>"
    --aot-receipt "${CMAKE_CURRENT_BINARY_DIR}/aot-fixtures-strict/receipt.json"
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/jitless-proof")
  set_tests_properties(jitless_boundary PROPERTIES DEPENDS "aot_instructions_strict;runtime_smoke" TIMEOUT 60)
  set_tests_properties(aot_instructions aot_instructions_strict aot_discovery_strict darwin_runtime jitless_boundary
    PROPERTIES RUN_SERIAL TRUE)
endif()

if(SKYRIM_BUILD_GAME)
  if(NOT EXISTS "${SKYRIM_PYTHON}")
    message(FATAL_ERROR "Run macos/bootstrap.sh to create the private Python environment.")
  endif()
  set(_game_dir_args)
  if(SKYRIM_GAME_DIR)
    set(_game_dir_args --game-dir "${SKYRIM_GAME_DIR}")
  endif()
  execute_process(COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/inputs.py"
    verify-generated "${SKYRIM_GEN_DIR}" ${_game_dir_args} RESULT_VARIABLE _input_status OUTPUT_QUIET)
  if(NOT _input_status EQUAL 0)
    message(FATAL_ERROR "Game code is absent or unverified. Run macos/prepare_game.py with your matching game files.")
  endif()
  file(GLOB GEN_C CONFIGURE_DEPENDS "${SKYRIM_GEN_DIR}/*.c")
  add_library(skyrim_generated OBJECT ${GEN_C})
  target_include_directories(skyrim_generated PRIVATE "${SKYRIM_ROOT}/runtime" "${SKYRIM_GEN_DIR}")
  target_compile_definitions(skyrim_generated PRIVATE GAME_SKYRIM)
  if(LB_STRICT_ORDER)
    target_compile_definitions(skyrim_generated PRIVATE LB_STRICT_ORDER)
  endif()
  target_compile_options(skyrim_generated PRIVATE -O2 -g0 -fno-strict-aliasing -fwrapv -fsigned-char)
  if(LB_CONST_MEM)
    target_compile_definitions(skyrim_generated PRIVATE LB_CONST_MEM)
  endif()
  if(LB_PINNED_REGS)
    # x21-x28 hold the guest registers (runtime/lift_prelude.h); x19/x20 stay with the compiler.
    target_compile_definitions(skyrim_generated PRIVATE LB_PINNED)
    target_compile_options(skyrim_generated PRIVATE -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24
      -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28)
    # Three LLVM passes that move or speculate instructions miscompile writes to the reserved registers
    # (found by whole-build bisection, docs/perf/LOG.md); the build crashes at boot unless all are off.
    target_compile_options(skyrim_generated PRIVATE "SHELL:-mllvm -disable-postra-machine-licm"
      "SHELL:-mllvm -disable-early-ifcvt" "SHELL:-mllvm -aarch64-enable-ccmp=false")
  endif()
  separate_arguments(_gen_flags NATIVE_COMMAND "${SKYRIM_GEN_FLAGS}")
  target_compile_options(skyrim_generated PRIVATE ${_gen_flags})
  set_property(GLOBAL PROPERTY JOB_POOLS generated_compile=2)
  set_property(TARGET skyrim_generated PROPERTY JOB_POOL_COMPILE generated_compile)
  add_custom_target(verify_game_inputs
    COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/build_receipt.py" begin
      --gen-dir "${SKYRIM_GEN_DIR}" --binary "${CMAKE_CURRENT_BINARY_DIR}/skyrim_macos"
      --build-dir "${CMAKE_CURRENT_BINARY_DIR}" ${_game_dir_args}
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/skyrim_macos.build-start.json" VERBATIM)
  add_dependencies(skyrim_generated verify_game_inputs)
  add_dependencies(skyrim_runtime verify_game_inputs)
  add_executable(skyrim_macos "${SKYRIM_ROOT}/runtime/main.c"
    $<TARGET_OBJECTS:skyrim_generated>)
  target_link_libraries(skyrim_macos PRIVATE skyrim_runtime)
  add_dependencies(skyrim_macos verify_game_inputs)
  set_property(TARGET skyrim_macos APPEND PROPERTY LINK_DEPENDS
    "${SKYRIM_GEN_DIR}/input-manifest.json"
    "${CMAKE_CURRENT_BINARY_DIR}/skyrim_macos.build-start.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/build_receipt.py" "${CMAKE_CURRENT_SOURCE_DIR}/inputs.py")
  add_custom_command(TARGET skyrim_macos POST_BUILD
    COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/build_receipt.py" seal
      --gen-dir "${SKYRIM_GEN_DIR}" --binary "$<TARGET_FILE:skyrim_macos>"
      --build-dir "${CMAKE_CURRENT_BINARY_DIR}" ${_game_dir_args}
      --dependency "dxvk=$<TARGET_FILE:dxvk_d3d9>"
      --dependency "sdl2=$<TARGET_FILE:SDL2::SDL2>"
      --dependency "moltenvk=${SKYRIM_MOLTENVK_LIBRARY}" VERBATIM)
else()
  message(STATUS "Game executable disabled: this build validates runtime and graphics without game files.")
endif()
