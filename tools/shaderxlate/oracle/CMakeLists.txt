cmake_minimum_required(VERSION 3.20)
project(vkdiff CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Pass -DVULKAN_INCLUDE=<dir containing vulkan/vulkan.h>. The loader is loaded
# at run time (vulkan-1.dll), so no import library is needed.
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan headers directory")
add_executable(vkdiff vkdiff.cpp)
target_include_directories(vkdiff PRIVATE ${VULKAN_INCLUDE})
target_compile_options(vkdiff PRIVATE -O2 -Wall -Wno-unused-but-set-global)
target_link_options(vkdiff PRIVATE -static)
