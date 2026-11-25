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

# NVN line 2 of 3: ${SMRPG_NVN_REGISTER_SOURCE} joins the runtime sources
add_executable(smrpg_boot ${SMRPG_RT_SOURCES} ${SMRPG_HLE_SOURCES} ${SMRPG_NVN_REGISTER_SOURCE} ${SMRPG_EXTRA_SOURCES})
target_include_directories(smrpg_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SMRPG_GEN_DIR})
target_compile_definitions(smrpg_boot PRIVATE _WIN32_WINNT=0x0A00)
# same code generation settings the lifter uses for the game objects (build.py CFLAGS); -w there, warnings kept here for our own files
target_compile_options(smrpg_boot PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -march=x86-64-v3 -ffp-contract=off -fno-strict-aliasing -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -Wno-comment -Wno-unused-function>)
# NVN line 3 of 3: link the library (the LinkAnchor functions keep its objects in the static archive)
target_link_libraries(smrpg_boot PRIVATE smrpg_nvn ${SMRPG_EXTRA_LIBS})

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
  set_source_files_properties(${_game_objs} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  target_sources(smrpg_boot PRIVATE ${_game_objs})
  set_target_properties(smrpg_boot PROPERTIES OUTPUT_NAME smrpg)
endif()
