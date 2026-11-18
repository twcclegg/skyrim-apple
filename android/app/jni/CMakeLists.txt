cmake_minimum_required(VERSION 3.18)
project(skyrim C CXX)
set(CMAKE_CXX_STANDARD 17)
# Each recompiled file needs 1-2 GB of RAM to compile; cap parallel jobs.
set(LB_GEN_JOBS 3 CACHE STRING "parallel compiles of recompiled code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS gen=${LB_GEN_JOBS})

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
# Dependencies live outside the Android tree: tools/fetch_sdl.py and the third_party/dxvk submodule.
get_filename_component(SKYRIM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
add_subdirectory(${SKYRIM_ROOT}/third_party/SDL2 SDL2)
add_subdirectory(${SKYRIM_ROOT}/third_party/dxvk dxvk)
add_subdirectory(game)
