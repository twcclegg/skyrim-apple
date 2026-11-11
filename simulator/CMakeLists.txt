cmake_minimum_required(VERSION 3.24)
project(SkyrimSimulatorGroundwork LANGUAGES C CXX OBJC OBJCXX)
string(TOLOWER "${CMAKE_OSX_SYSROOT}" _simulator_sdk)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT _simulator_sdk MATCHES "iphonesimulator")
  message(FATAL_ERROR "Use simulator/toolchain.cmake; a real iOS Simulator SDK slice is required.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SKYRIM_SIM_BUILD_GAME "Build actual Skyrim from verified private Windows-generated code" OFF)
get_filename_component(SKYRIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SKYRIM_PYTHON "${SKYRIM_ROOT}/macos/.venv/bin/python" CACHE FILEPATH "Python with lifter dependencies")
set(SKYRIM_MOLTENVK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/MoltenVK-source/Package/Latest/MoltenVK" CACHE PATH "Source-built simulator MoltenVK package")
set(SKYRIM_MOLTENVK_LIBRARY "${SKYRIM_MOLTENVK_ROOT}/static/MoltenVK.xcframework/ios-arm64-simulator/libMoltenVK.a" CACHE FILEPATH "Genuine arm64 iOS Simulator MoltenVK")
if(NOT EXISTS "${SKYRIM_MOLTENVK_LIBRARY}")
  message(FATAL_ERROR "Build the pinned Simulator MoltenVK slice with simulator/bootstrap.sh first.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/fixture/fixture_addresses.h")
  message(FATAL_ERROR "Run simulator/generate_fixture.py --output ${CMAKE_CURRENT_BINARY_DIR}/fixture first.")
endif()

# Bundled, pinned SDL source avoids linking a macOS or device binary into a simulator app.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
execute_process(COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sdl.py"
  "${CMAKE_CURRENT_BINARY_DIR}/sdl2-source" RESULT_VARIABLE _sdl_scene_status)
if(NOT _sdl_scene_status EQUAL 0)
  message(FATAL_ERROR "Unable to prepare the native UIKit SDL scene adaptation.")
endif()
add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/sdl2-source" sdl2)
target_compile_definitions(SDL2 PRIVATE SKYRIM_UIKIT_SCENE_LIFECYCLE)
set_target_properties(SDL2 PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
add_library(Vulkan::Vulkan STATIC IMPORTED GLOBAL)
set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION "${SKYRIM_MOLTENVK_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${SKYRIM_MOLTENVK_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "-framework Metal;-framework QuartzCore;-framework Foundation;-framework IOSurface;-framework CoreGraphics;-framework UIKit")
set(DXVK_VULKAN_INCLUDE_DIR "${SKYRIM_MOLTENVK_ROOT}/include")
add_subdirectory("${SKYRIM_ROOT}/third_party/dxvk" dxvk)
set_target_properties(dxvk_d3d9 PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)

file(GLOB RT_C CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.c")
list(REMOVE_ITEM RT_C "${SKYRIM_ROOT}/runtime/main.c")
file(GLOB RT_CPP CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.cpp")
add_library(skyrim_sim_runtime OBJECT ${RT_C} ${RT_CPP})
target_include_directories(skyrim_sim_runtime PUBLIC "${SKYRIM_ROOT}/runtime")
target_compile_definitions(skyrim_sim_runtime PUBLIC GAME_SKYRIM DXVK_NATIVE DXVK_WSI_SDL2 LB_STRICT_ORDER)
target_compile_options(skyrim_sim_runtime PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
  $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
# Guest-code host threads (CreateThread, timer callbacks, the XAudio2 engine)
# get the same 8 MiB stacks as the Mac build. runtime/ defines GUEST_HOST_STACK
# only for TARGET_OS_OSX; iOS secondary pthreads otherwise default to 512 KiB.
set(SKYRIM_SIM_GUEST_STACK_BYTES 8388608 CACHE STRING "Host stack bytes for guest-code threads (page multiple)")
target_compile_definitions(skyrim_sim_runtime PRIVATE GUEST_HOST_STACK=${SKYRIM_SIM_GUEST_STACK_BYTES})
target_link_libraries(skyrim_sim_runtime PUBLIC SDL2 dxvk_d3d9)
set(SKYRIM_FFMPEG_ROOT "${SKYRIM_ROOT}/macos/deps/ffmpeg-simulator" CACHE PATH "Private arm64 iOS Simulator FFmpeg libraries")
set(_audio_default OFF)
if(EXISTS "${SKYRIM_FFMPEG_ROOT}/lib/libavcodec.a" AND EXISTS "${SKYRIM_FFMPEG_ROOT}/lib/libavutil.a")
  set(_audio_default ON)
endif()
option(SKYRIM_FFMPEG_AUDIO "Use the native software xWMA decoder" ${_audio_default})
if(SKYRIM_FFMPEG_AUDIO)
  foreach(_library avcodec avutil)
    if(NOT EXISTS "${SKYRIM_FFMPEG_ROOT}/lib/lib${_library}.a")
      message(FATAL_ERROR "Native Simulator audio requested but private lib${_library}.a is missing")
    endif()
  endforeach()
  target_compile_definitions(skyrim_sim_runtime PUBLIC SKYRIM_FFMPEG_AUDIO=1)
  target_include_directories(skyrim_sim_runtime PUBLIC "${SKYRIM_FFMPEG_ROOT}/include")
  target_link_libraries(skyrim_sim_runtime PUBLIC "${SKYRIM_FFMPEG_ROOT}/lib/libavcodec.a" "${SKYRIM_FFMPEG_ROOT}/lib/libavutil.a" m)
endif()
add_library(skyrim_sim_entry OBJECT "${SKYRIM_ROOT}/runtime/main.c")
target_compile_definitions(skyrim_sim_entry PRIVATE GAME_SKYRIM main=skyrim_guest_main)
target_include_directories(skyrim_sim_entry PRIVATE "${SKYRIM_ROOT}/runtime")
target_link_libraries(skyrim_sim_entry PRIVATE SDL2)

file(GLOB FIXTURE_C CONFIGURE_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/fixture/generated/*.c")
add_library(simulator_fixture OBJECT ${FIXTURE_C})
target_include_directories(simulator_fixture PRIVATE "${SKYRIM_ROOT}/runtime" "${CMAKE_CURRENT_BINARY_DIR}/fixture/generated")
target_compile_definitions(simulator_fixture PRIVATE GAME_SKYRIM LB_STRICT_ORDER)
target_compile_options(simulator_fixture PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char)

add_executable(SkyrimRuntimeProbe MACOSX_BUNDLE app.cpp cpu_probe.c ui.mm
  "${CMAKE_CURRENT_BINARY_DIR}/fixture/fixture_modules.c"
  $<TARGET_OBJECTS:skyrim_sim_entry> $<TARGET_OBJECTS:simulator_fixture>)
target_include_directories(SkyrimRuntimeProbe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fixture")
if(SKYRIM_FFMPEG_AUDIO)
  target_sources(SkyrimRuntimeProbe PRIVATE "${SKYRIM_ROOT}/macos/audio/codec_probe.c")
  target_include_directories(SkyrimRuntimeProbe PRIVATE "${SKYRIM_ROOT}/macos/audio")
  foreach(_sample musicmono_xwma.wav musicsurround_xwma.wav)
    set(_sample_path "${SKYRIM_ROOT}/macos/build/audio-audit/${_sample}")
    if(NOT EXISTS "${_sample_path}")
      message(FATAL_ERROR "Native codec diagnostic requires the private public-sample corpus: ${_sample}")
    endif()
    set_source_files_properties("${_sample_path}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/AudioFixtures)
    target_sources(SkyrimRuntimeProbe PRIVATE "${_sample_path}")
  endforeach()
endif()
target_compile_definitions(SkyrimRuntimeProbe PRIVATE DXVK_WSI_SDL2)
target_link_libraries(SkyrimRuntimeProbe PRIVATE skyrim_sim_runtime SDL2main "-framework UIKit" "-framework Foundation" objc)
set_target_properties(SkyrimRuntimeProbe PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.zschwendi.skyrim.jitless.probe"
  BUILD_RPATH "@executable_path/Frameworks")
add_custom_command(TARGET SkyrimRuntimeProbe POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>/Frameworks"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL2>" "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>/Frameworks/$<TARGET_FILE_NAME:SDL2>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:dxvk_d3d9>" "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>/Frameworks/$<TARGET_FILE_NAME:dxvk_d3d9>"
  COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>/Frameworks/$<TARGET_FILE_NAME:SDL2>"
  COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>/Frameworks/$<TARGET_FILE_NAME:dxvk_d3d9>"
  COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimRuntimeProbe>"
  VERBATIM)

# Compile the future native app frontend even before game inputs arrive. It is
# not linked into the diagnostic and cannot masquerade as a game-bearing app.
add_library(simulator_game_frontend OBJECT game_app.cpp game_inputs.mm)
target_link_libraries(simulator_game_frontend PRIVATE SDL2 "-framework Foundation")
if(SKYRIM_SIM_BUILD_GAME)
  set(SKYRIM_GEN_DIR "${SKYRIM_ROOT}/gen" CACHE PATH "Verified private generated Windows source")
  execute_process(COMMAND "${SKYRIM_PYTHON}" "${SKYRIM_ROOT}/macos/inputs.py"
    verify-generated "${SKYRIM_GEN_DIR}" RESULT_VARIABLE _game_inputs_status)
  if(NOT _game_inputs_status EQUAL 0)
    message(FATAL_ERROR "Actual Simulator game build requires verified original Windows Skyrim gen/ first.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SKYRIM_GEN_DIR}/input-manifest.json")
  file(SHA256 "${SKYRIM_GEN_DIR}/input-manifest.json" _game_manifest_sha)
  target_compile_definitions(simulator_game_frontend PRIVATE SKYRIM_SIM_MANIFEST_SHA256="${_game_manifest_sha}")
  set(_game_resource "${CMAKE_CURRENT_BINARY_DIR}/game-input-manifest.json")
  execute_process(COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_bundle_inputs.py"
    "${SKYRIM_GEN_DIR}" "${_game_resource}" RESULT_VARIABLE _game_resource_status)
  if(NOT _game_resource_status EQUAL 0)
    message(FATAL_ERROR "Unable to prepare the private app input identity.")
  endif()
  file(GLOB GAME_C CONFIGURE_DEPENDS "${SKYRIM_GEN_DIR}/*.c")
  add_library(simulator_game_generated OBJECT ${GAME_C})
  target_include_directories(simulator_game_generated PRIVATE "${SKYRIM_ROOT}/runtime" "${SKYRIM_GEN_DIR}")
  target_compile_definitions(simulator_game_generated PRIVATE GAME_SKYRIM LB_STRICT_ORDER)
  target_compile_options(simulator_game_generated PRIVATE -O2 -g0 -fno-strict-aliasing -fwrapv -fsigned-char)
  set_property(GLOBAL PROPERTY JOB_POOLS generated_compile=2)
  set_property(TARGET simulator_game_generated PROPERTY JOB_POOL_COMPILE generated_compile)
  add_custom_target(verify_simulator_game_inputs
    COMMAND "${SKYRIM_PYTHON}" "${SKYRIM_ROOT}/macos/inputs.py" verify-generated "${SKYRIM_GEN_DIR}" VERBATIM)
  add_dependencies(simulator_game_generated verify_simulator_game_inputs)
  add_executable(SkyrimJitless MACOSX_BUNDLE $<TARGET_OBJECTS:simulator_game_frontend>
    $<TARGET_OBJECTS:skyrim_sim_entry> $<TARGET_OBJECTS:simulator_game_generated>)
  target_link_libraries(SkyrimJitless PRIVATE skyrim_sim_runtime SDL2main "-framework UIKit" "-framework Foundation" objc)
  set_target_properties(SkyrimJitless PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/GameInfo.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.zschwendi.skyrim.jitless" BUILD_RPATH "@executable_path/Frameworks")
  set_property(TARGET SkyrimJitless APPEND PROPERTY LINK_DEPENDS "${SKYRIM_GEN_DIR}/input-manifest.json" "${_game_resource}")
  # iOS bundles are flat: the identity resource sits at the bundle root, where
  # NSBundle pathForResource, run_game.py and build.py read it. Copied before
  # signing so the bundle signature covers it.
  add_custom_command(TARGET SkyrimJitless POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_game_resource}" "$<TARGET_BUNDLE_DIR:SkyrimJitless>/game-input-manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:SkyrimJitless>/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL2>" "$<TARGET_BUNDLE_DIR:SkyrimJitless>/Frameworks/$<TARGET_FILE_NAME:SDL2>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:dxvk_d3d9>" "$<TARGET_BUNDLE_DIR:SkyrimJitless>/Frameworks/$<TARGET_FILE_NAME:dxvk_d3d9>"
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimJitless>/Frameworks/$<TARGET_FILE_NAME:SDL2>"
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimJitless>/Frameworks/$<TARGET_FILE_NAME:dxvk_d3d9>"
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SkyrimJitless>" VERBATIM)
else()
  target_compile_definitions(simulator_game_frontend PRIVATE SKYRIM_SIM_MANIFEST_SHA256="unconfigured-game-free-build")
  message(STATUS "Actual Simulator game app disabled: awaiting verified private Windows game inputs.")
endif()
