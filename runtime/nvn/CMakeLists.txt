# Star Road Recomp NVN on Vulkan (task-12). Our own code. C++17 + one C file, clang (LLVM-MinGW), Windows x64.
# Standalone (library, unit tests, GPU tests):
#   cmake -S runtime/nvn -B A:/AI/Recomps/smrpg-work/build/nvn -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build A:/AI/Recomps/smrpg-work/build/nvn
# From the runtime: add_subdirectory(nvn), link `smrpg_nvn` and add ${SMRPG_NVN_REGISTER_SOURCE} to smrpg_boot (docs/NVN.md).
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(smrpg_nvn C CXX)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(NVN_VULKAN_SDK "A:/AI/Recomps/new-pokemon-snap/tools/vulkan_sdk" CACHE PATH "Vulkan SDK (headers, glslc, validation layer)")

if(NOT TARGET smrpg_texture)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gfx/texture ${CMAKE_BINARY_DIR}/texture)
endif()

add_library(smrpg_nvn STATIC
  nvn_vk.cpp nvn_objects.cpp nvn_formats.cpp nvn_shaders.cpp nvn_replay.cpp nvn_proc.cpp
  nvn_api_device.cpp nvn_api_mem.cpp nvn_api_cmd.cpp)
# task-23: present-time upscale pass (FSR 1) and the weak overlay hook, runtime/gfx/upscale/, compiled into this library so QueuePresent links without a cycle
include(${CMAKE_CURRENT_SOURCE_DIR}/../gfx/upscale/upscale.cmake)
target_include_directories(smrpg_nvn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${NVN_VULKAN_SDK}/Include)
target_include_directories(smrpg_nvn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(smrpg_nvn PUBLIC _WIN32_WINNT=0x0A00)
target_compile_options(smrpg_nvn PRIVATE -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(smrpg_nvn PUBLIC smrpg_texture)

# The C registration file is compiled by the runtime target (it needs rt_internal.h); expose its path.
set(SMRPG_NVN_REGISTER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/nvn_register.c CACHE INTERNAL "")

option(SMRPG_NVN_TESTS "build the NVN tests (tests/nvn)" ON)
set(NVN_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/nvn)
if(SMRPG_NVN_TESTS AND EXISTS ${NVN_TESTS_DIR}/nvn_unit.cpp)
  add_executable(nvn_unit ${NVN_TESTS_DIR}/nvn_unit.cpp)
  target_link_libraries(nvn_unit PRIVATE smrpg_nvn)
  target_compile_options(nvn_unit PRIVATE -O1 -g -Wall -Wextra -Wno-unused-parameter)
endif()
if(SMRPG_NVN_TESTS AND EXISTS ${NVN_TESTS_DIR}/nvn_gpu.cpp)
  add_executable(nvn_gpu ${NVN_TESTS_DIR}/nvn_gpu.cpp)
  target_link_libraries(nvn_gpu PRIVATE smrpg_nvn)
  target_compile_options(nvn_gpu PRIVATE -O1 -g -Wall -Wextra -Wno-unused-parameter)
endif()
if(SMRPG_NVN_TESTS AND EXISTS ${NVN_TESTS_DIR}/nvn_cache.cpp)
  add_executable(nvn_cache ${NVN_TESTS_DIR}/nvn_cache.cpp)
  target_link_libraries(nvn_cache PRIVATE smrpg_nvn)
  target_compile_options(nvn_cache PRIVATE -O1 -g -Wall -Wextra -Wno-unused-parameter)
endif()
