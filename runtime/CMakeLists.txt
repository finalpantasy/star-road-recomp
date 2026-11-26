# Star Road Recomp runtime (task-5, task-13). Our own code. C11 + C++17 (NVN), clang (LLVM-MinGW), Windows x64.
#
# Game build (lifted objects from tools/lifter/build.py compile, link order from its link.rsp):
#   python -I tools/rtprep/build_game.py            # configure + build + copy to A:\AI\Recomps\smrpg-work\build\bin\smrpg.exe
# by hand:
#   cmake -S runtime -B A:/AI/Recomps/smrpg-work/build/rt-game -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DSMRPG_GAME_OBJECTS=A:/AI/Recomps/smrpg-work/build/bin/link.rsp
# Without SMRPG_GAME_OBJECTS the runtime links alone (no lifted code; every guest address misses) as smrpg_boot.
#
# Pieces:   runtime/*.c   core, loader, kernel svc, nn HLE table, interpreter
#           runtime/hle/  Horizon services (HIPC/CMIF server, sm, fs, applet, hid, audio, time ...)
#           runtime/nvn/  NVN on Vulkan (docs/NVN.md "Registering with the runtime": the three lines are marked below)
cmake_minimum_required(VERSION 3.20)
project(smrpg_runtime C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SMRPG_WORK "A:/AI/Recomps/smrpg-work" CACHE PATH "work data root (extraction, build, saves)")
set(SMRPG_GEN_DIR "${SMRPG_WORK}/build/gen_game" CACHE PATH "generated headers (ops_gen.h, interp_gen.inc, prelude.h)")
set(SMRPG_GAME_OBJECTS "" CACHE FILEPATH "response file listing the lifted objects (build.py link.rsp); empty = runtime only")
set(SMRPG_EXTRA_SOURCES "" CACHE STRING "extra sources linked into the executable")
set(SMRPG_EXTRA_LIBS "" CACHE STRING "extra libraries or objects")

# NVN line 1 of 3: the NVN library (defines smrpg_nvn and SMRPG_NVN_REGISTER_SOURCE)
set(SMRPG_NVN_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(nvn)

file(GLOB SMRPG_HLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hle/*.c)
set(SMRPG_RT_SOURCES rt_core.c loader.c svc.c hle_nn.c main.c rt_weak.c gfxcap.c nvn_host.cpp)
# the interpreter needs the generated op tables; with no game objects there is no strong interp_run, so rt_weak.c covers it
if(SMRPG_GAME_OBJECTS)
  list(APPEND SMRPG_RT_SOURCES interp.c crypto_a64.c eh/eh.c gc/gc.c)   # eh/ and gc/ are task-14's (exceptions, collector safepoints); interp.c calls both
endif()

list(APPEND SMRPG_RT_SOURCES mods/postgame.c mods/datatbl_overlay.c ../tools/datatbl/nrbf.c platform/overlay/options.c)
# task-24: save view in hle_fs.c; task-25: monster table view in hle_fs.c and its NRBF codec. The plain-C options store is shared by the
# optional ImGui menu and the filesystem HLE, so it stays available in no-overlay builds without linking any SDL or ImGui code.
# NVN line 2 of 3: ${SMRPG_NVN_REGISTER_SOURCE} joins the runtime sources
add_executable(smrpg_boot ${SMRPG_RT_SOURCES} ${SMRPG_HLE_SOURCES} ${SMRPG_NVN_REGISTER_SOURCE} ${SMRPG_EXTRA_SOURCES})
target_include_directories(smrpg_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SMRPG_GEN_DIR})
target_compile_definitions(smrpg_boot PRIVATE _WIN32_WINNT=0x0A00)
# task-21: the movie HLE (hle/hle_movie.c) compiles against the headers of the LGPL FFmpeg that tools/ffmpeg/build.py installs and loads its
# DLLs at run time (LoadLibrary); without the headers it builds a stub that only reports that movies cannot decode.
set(SMRPG_FFMPEG_DIR "A:/AI/Recomps/ffmpeg/install" CACHE PATH "FFmpeg install (tools/ffmpeg/build.py): include/ for the build, bin/ for the run")
if(EXISTS "${SMRPG_FFMPEG_DIR}/include/libavcodec/avcodec.h")
  target_include_directories(smrpg_boot PRIVATE "${SMRPG_FFMPEG_DIR}/include")
endif()
# high-entropy ASLR can place the main thread stack inside the fixed guest ranges (task-14, exit 10)
target_link_options(smrpg_boot PRIVATE -Wl,--disable-high-entropy-va)
# same code generation settings the lifter uses for the game objects (build.py CFLAGS); -w there, warnings kept here for our own files
target_compile_options(smrpg_boot PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -march=x86-64-v3 -ffp-contract=off -fno-strict-aliasing -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -Wno-comment -Wno-unused-function>)
# NVN line 3 of 3: link the library (the LinkAnchor functions keep its objects in the static archive)
target_link_libraries(smrpg_boot PRIVATE smrpg_nvn ${SMRPG_EXTRA_LIBS})

# task-22: the platform layer (runtime/platform/, SDL3: window, Vulkan surface, input, audio device). hidshm.c has no SDL and always builds
# (hle_hid.c uses it); the rest builds when the SDL3 headers are found, and defines SMRPG_PLATFORM. SDL3.dll (and libiconv-2.dll) are copied
# beside the exe by tools/rtprep/build_game.py. Nothing from SDL is committed.
set(SMRPG_SDL3_DIR "A:/msys64/ucrt64" CACHE PATH "SDL3 install (MSYS2 ucrt64 mingw-w64-ucrt-x86_64-SDL3): include/SDL3, lib/libSDL3.dll.a, bin/SDL3.dll")
if(EXISTS "${SMRPG_SDL3_DIR}/include/SDL3/SDL.h" AND EXISTS "${SMRPG_SDL3_DIR}/lib/libSDL3.dll.a")
  target_sources(smrpg_boot PRIVATE platform/plat_core.c platform/plat_audio.c platform/pad.c platform/plat_nvn.cpp)
  target_compile_definitions(smrpg_boot PRIVATE SMRPG_PLATFORM=1)
  target_compile_options(smrpg_boot PRIVATE -idirafter "${SMRPG_SDL3_DIR}/include")      # after the others: the MSYS2 include dir holds unrelated headers
  target_link_libraries(smrpg_boot PRIVATE "${SMRPG_SDL3_DIR}/lib/libSDL3.dll.a" winmm)
  message(STATUS "platform layer: SDL3 from ${SMRPG_SDL3_DIR}")
else()
  message(STATUS "platform layer: SDL3 not found under ${SMRPG_SDL3_DIR}; building without a window (set SMRPG_SDL3_DIR)")
endif()

# task-16: the options overlay (runtime/platform/overlay/): Dear ImGui (MIT) drawn over the present through PlatOverlayDraw, options in options.ini. The ImGui
# source is fetched by `python -I tools/thirdparty/fetch_imgui.py` into A: (never committed); without it the runtime builds with no overlay (SMRPG_OVERLAY undefined,
# overlay.h is all no-ops) and F1 does nothing. ImGui's Vulkan backend loads its functions through vkGetInstanceProcAddr (the runtime links no Vulkan import library).
set(SMRPG_IMGUI_DIR "A:/AI/Recomps/thirdparty/imgui/src/imgui-1.93.0" CACHE PATH "Dear ImGui release unpacked by tools/thirdparty/fetch_imgui.py")
if(EXISTS "${SMRPG_SDL3_DIR}/include/SDL3/SDL.h" AND EXISTS "${SMRPG_IMGUI_DIR}/imgui.h" AND NVN_VULKAN_SDK)
  set(_ovl ${CMAKE_CURRENT_SOURCE_DIR}/platform/overlay)
  set(_imgui_srcs ${SMRPG_IMGUI_DIR}/imgui.cpp ${SMRPG_IMGUI_DIR}/imgui_draw.cpp ${SMRPG_IMGUI_DIR}/imgui_tables.cpp ${SMRPG_IMGUI_DIR}/imgui_widgets.cpp
      ${SMRPG_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${SMRPG_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
  target_sources(smrpg_boot PRIVATE ${_ovl}/overlay_apply.cpp ${_ovl}/overlay.cpp ${_imgui_srcs})
  target_include_directories(smrpg_boot PRIVATE ${SMRPG_IMGUI_DIR} ${SMRPG_IMGUI_DIR}/backends "${NVN_VULKAN_SDK}/Include")
  target_compile_definitions(smrpg_boot PRIVATE SMRPG_OVERLAY=1)
  set_source_files_properties(${_imgui_srcs} ${_ovl}/overlay.cpp PROPERTIES
      COMPILE_DEFINITIONS "VK_NO_PROTOTYPES;IMGUI_IMPL_VULKAN_NO_PROTOTYPES;IMGUI_DISABLE_WIN32_FUNCTIONS;IMGUI_DISABLE_DEMO_WINDOWS;IMGUI_DISABLE_DEBUG_TOOLS")
  set_source_files_properties(${_imgui_srcs} PROPERTIES COMPILE_OPTIONS "-w")       # the vendored source is not ours to clean up
  message(STATUS "options overlay: Dear ImGui from ${SMRPG_IMGUI_DIR}")
else()
  message(STATUS "options overlay: Dear ImGui not found under ${SMRPG_IMGUI_DIR} (run tools/thirdparty/fetch_imgui.py); building without the overlay")
endif()

if(SMRPG_GAME_OBJECTS)
  file(STRINGS ${SMRPG_GAME_OBJECTS} _objs)
  set(_game_objs "")
  foreach(o IN LISTS _objs)
    get_filename_component(_n ${o} NAME)
    if(_n MATCHES "^(main|sdk|subsdk0)_[0-9][0-9][0-9][0-9]_" OR _n MATCHES "^regs_")
      list(APPEND _game_objs ${o})
    endif()
  endforeach()
  list(LENGTH _game_objs _ng)
  message(STATUS "game objects: ${_ng}")
  include(decomp/game.cmake)
  set_source_files_properties(${_game_objs} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  target_sources(smrpg_boot PRIVATE ${_game_objs})
  set_target_properties(smrpg_boot PROPERTIES OUTPUT_NAME smrpg)
endif()
