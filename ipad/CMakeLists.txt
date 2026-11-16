cmake_minimum_required(VERSION 3.24)
project(SkyrimIPadNative LANGUAGES C CXX OBJC OBJCXX)
# The native Skyrim engine for the physical iPad, linked into the launcher app (apple/).
# Same pinned sources as the Mac and iPad Simulator builds; device (iphoneos arm64) slices only.
# ipad/visionos/toolchain.cmake builds the same engine for Apple Vision Pro (xros arm64), and
# ipad/visionos/toolchain-simulator.cmake for the visionOS Simulator (xrsimulator arm64).
string(TOLOWER "${CMAKE_OSX_SYSROOT}" _sdk)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND _sdk MATCHES "iphoneos")
  set(SKYRIM_DEVICE_SDK iphoneos)
  set(SKYRIM_SDL_PLATFORM ios)
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS" AND _sdk MATCHES "xrsimulator")
  set(SKYRIM_DEVICE_SDK xrsimulator)  # pre-headset checks of the window and surface path only
  set(SKYRIM_SDL_PLATFORM visionos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS" AND _sdk MATCHES "xros")
  set(SKYRIM_DEVICE_SDK xros)
  set(SKYRIM_SDL_PLATFORM visionos)
else()
  message(FATAL_ERROR "Use ipad/toolchain.cmake (iphoneos), ipad/visionos/toolchain.cmake (xros) or ipad/visionos/toolchain-simulator.cmake (xrsimulator).")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(SKYRIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SKYRIM_PYTHON "" CACHE FILEPATH "Python 3.10+; ipad/build.py passes its own interpreter")
set(SKYRIM_GEN_DIR "" CACHE PATH "Verified private generated Windows source (read only)")
set(SKYRIM_GAME_DIR "" CACHE PATH "Game folder gen/ is bound to; empty uses the one its manifest records")
if(SKYRIM_DEVICE_SDK STREQUAL "iphoneos")
  set(_moltenvk_dir MoltenVK-ios-v1.4.2)
else()
  set(_moltenvk_dir MoltenVK-${SKYRIM_DEVICE_SDK}-v1.4.2)
endif()
set(SKYRIM_MOLTENVK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/${_moltenvk_dir}" CACHE PATH "MoltenVK v1.4.2 device slice")
set(SKYRIM_MOLTENVK_LIBRARY "${SKYRIM_MOLTENVK_ROOT}/lib/libMoltenVK.a")
set(SKYRIM_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg-${SKYRIM_DEVICE_SDK}" CACHE PATH "FFmpeg 7.1.2 WMA decoders, device SDK")
if(NOT SKYRIM_PYTHON)
  message(FATAL_ERROR "Configure through ipad/build.py engine, which sets SKYRIM_PYTHON and SKYRIM_GEN_DIR.")
endif()
foreach(_required "${SKYRIM_MOLTENVK_LIBRARY}" "${SKYRIM_FFMPEG_ROOT}/lib/libavcodec.a"
                  "${SKYRIM_FFMPEG_ROOT}/lib/libavutil.a" "${SKYRIM_GEN_DIR}/input-manifest.json")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "Missing ${_required}; configure through ipad/build.py engine (see ipad/build.py --help).")
  endif()
endforeach()

# Pinned SDL with the UIKit scene overlay, as a static library: SDL attaches its window to the
# launcher's scene. HIDAPI stays off (its Bluetooth path needs a usage string; DualSense comes
# through GameController.framework) and so does OpenGL ES (the game renders via Vulkan/Metal).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
execute_process(COMMAND "${SKYRIM_PYTHON}" "${SKYRIM_ROOT}/simulator/prepare_sdl.py"
  --platform ${SKYRIM_SDL_PLATFORM} "${CMAKE_CURRENT_BINARY_DIR}/sdl2-source" RESULT_VARIABLE _sdl_scene_status)
if(NOT _sdl_scene_status EQUAL 0)
  message(FATAL_ERROR "Unable to prepare the native UIKit SDL scene adaptation.")
endif()
add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/sdl2-source" sdl2)
target_compile_definitions(SDL2-static PRIVATE SKYRIM_UIKIT_SCENE_LIFECYCLE)
add_library(SDL2 ALIAS SDL2-static)

add_library(Vulkan::Vulkan STATIC IMPORTED GLOBAL)
set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION "${SKYRIM_MOLTENVK_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${SKYRIM_MOLTENVK_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "-framework Metal;-framework QuartzCore;-framework Foundation;-framework IOSurface;-framework CoreGraphics;-framework UIKit")
set(DXVK_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${SKYRIM_ROOT}/android/app/jni/dxvk" dxvk)

file(GLOB RT_C CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.c")
list(REMOVE_ITEM RT_C "${SKYRIM_ROOT}/runtime/main.c")
file(GLOB RT_CPP CONFIGURE_DEPENDS "${SKYRIM_ROOT}/runtime/*.cpp")
add_library(skyrim_ipad_runtime OBJECT ${RT_C} ${RT_CPP})
target_include_directories(skyrim_ipad_runtime PUBLIC "${SKYRIM_ROOT}/runtime" "${SKYRIM_FFMPEG_ROOT}/include")
target_compile_definitions(skyrim_ipad_runtime PUBLIC GAME_SKYRIM DXVK_NATIVE DXVK_WSI_SDL2 LB_STRICT_ORDER
  SKYRIM_FFMPEG_AUDIO=1)
target_compile_options(skyrim_ipad_runtime PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
  $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
# Guest-code host threads get the same 8 MiB stacks as the Mac and Simulator builds
# (iOS secondary pthreads otherwise default to 512 KiB).
target_compile_definitions(skyrim_ipad_runtime PRIVATE GUEST_HOST_STACK=8388608)
target_link_libraries(skyrim_ipad_runtime PUBLIC SDL2 dxvk_d3d9)

add_library(skyrim_ipad_entry OBJECT "${SKYRIM_ROOT}/runtime/main.c")
target_compile_definitions(skyrim_ipad_entry PRIVATE GAME_SKYRIM main=skyrim_guest_main)
target_include_directories(skyrim_ipad_entry PRIVATE "${SKYRIM_ROOT}/runtime")
target_link_libraries(skyrim_ipad_entry PRIVATE SDL2)

set(_game_dir_args)
if(SKYRIM_GAME_DIR)
  set(_game_dir_args --game-dir "${SKYRIM_GAME_DIR}")
endif()
execute_process(COMMAND "${SKYRIM_PYTHON}" "${SKYRIM_ROOT}/macos/inputs.py" verify-generated "${SKYRIM_GEN_DIR}"
  ${_game_dir_args} RESULT_VARIABLE _game_inputs_status OUTPUT_QUIET)
if(NOT _game_inputs_status EQUAL 0)
  message(FATAL_ERROR "The iPad game build requires the verified original Windows Skyrim gen/.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SKYRIM_GEN_DIR}/input-manifest.json")
execute_process(COMMAND "${SKYRIM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_identity.py"
  "${SKYRIM_GEN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated/skyrim_native_identity.h" ${_game_dir_args}
  --sdk ${SKYRIM_DEVICE_SDK}
  RESULT_VARIABLE _identity_status)
if(NOT _identity_status EQUAL 0)
  message(FATAL_ERROR "Unable to write the native runtime identity.")
endif()
add_library(skyrim_ipad_bridge OBJECT native_bridge.mm)
if(SKYRIM_SDL_PLATFORM STREQUAL "visionos")
  # The immersive compositor's frame handoff needs DXVK's tap header and MoltenVK's Vulkan headers.
  target_sources(skyrim_ipad_bridge PRIVATE immersive_bridge.mm)
  target_include_directories(skyrim_ipad_bridge PRIVATE "${SKYRIM_ROOT}/android/app/jni/dxvk/include/native"
    "${SKYRIM_MOLTENVK_ROOT}/include")
endif()
target_include_directories(skyrim_ipad_bridge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(skyrim_ipad_bridge PRIVATE -fobjc-arc)
target_link_libraries(skyrim_ipad_bridge PRIVATE SDL2)

file(GLOB GAME_C CONFIGURE_DEPENDS "${SKYRIM_GEN_DIR}/*.c")
add_library(skyrim_ipad_generated OBJECT ${GAME_C})
target_include_directories(skyrim_ipad_generated PRIVATE "${SKYRIM_ROOT}/runtime" "${SKYRIM_GEN_DIR}")
target_compile_definitions(skyrim_ipad_generated PRIVATE GAME_SKYRIM LB_STRICT_ORDER)
# The guest base pointer never changes after startup; telling the generated code so removes its reloads
# (docs/perf/LOG.md). -DLB_CONST_MEM=OFF builds the old way for an A/B.
option(LB_CONST_MEM "Generated code treats the guest base pointer as a constant" ON)
if(LB_CONST_MEM)
  target_compile_definitions(skyrim_ipad_generated PRIVATE LB_CONST_MEM)
endif()
target_compile_options(skyrim_ipad_generated PRIVATE -O2 -g0 -fno-strict-aliasing -fwrapv -fsigned-char)
# Guest registers in x21-x28 (runtime/lift_prelude.h), as in macos/CMakeLists.txt: the generated code
# reserves the registers and has the three LLVM passes that move writes to reserved registers turned
# off; the runtime only needs to know the mode is on, for its entry trampoline.
option(LB_PINNED_REGS "Keep the guest registers in fixed host registers in lifted code" OFF)
if(LB_PINNED_REGS)
  target_compile_definitions(skyrim_ipad_runtime PUBLIC LB_PINNED)
  target_compile_definitions(skyrim_ipad_generated PRIVATE LB_PINNED)
  target_compile_options(skyrim_ipad_generated PRIVATE -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24
    -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28
    "SHELL:-mllvm -disable-postra-machine-licm" "SHELL:-mllvm -disable-early-ifcvt"
    "SHELL:-mllvm -aarch64-enable-ccmp=false")
endif()
set_property(GLOBAL PROPERTY JOB_POOLS generated_compile=2)
set_property(TARGET skyrim_ipad_generated PROPERTY JOB_POOL_COMPILE generated_compile)

# The engine archive: generated game code, runtime, entry and bridge. ipad/build.py merges it
# with the SDL2, DXVK, MoltenVK and FFmpeg archives into one library for the launcher.
add_library(SkyrimNativeEngine STATIC $<TARGET_OBJECTS:skyrim_ipad_runtime> $<TARGET_OBJECTS:skyrim_ipad_entry>
  $<TARGET_OBJECTS:skyrim_ipad_bridge> $<TARGET_OBJECTS:skyrim_ipad_generated>)
add_dependencies(SkyrimNativeEngine SDL2-static dxvk_d3d9)
