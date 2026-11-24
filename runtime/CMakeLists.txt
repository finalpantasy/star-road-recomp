# Star Road Recomp runtime (task-5). Our own code. C11, clang (LLVM-MinGW), Windows x64.
# cmake -S runtime -B A:/AI/Recomps/smrpg-work/build/rt-cmake -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang
# Plain clang works too: clang -std=c11 -O1 runtime/{rt_core,loader,svc,hle_nn,main,rt_weak}.c -o smrpg_boot.exe
# Link the generated game library and the interpreter by passing their sources or an object/archive:
#   -DSMRPG_EXTRA_SOURCES="A:/.../gen/chunk_0.c;...;runtime/interp.c"   or   -DSMRPG_EXTRA_LIBS="path/libgame.a"
cmake_minimum_required(VERSION 3.20)
project(smrpg_runtime C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(SMRPG_EXTRA_SOURCES "" CACHE STRING "generated chunks, interpreter and other sources linked into smrpg_boot")
set(SMRPG_EXTRA_LIBS "" CACHE STRING "extra libraries or objects")
add_executable(smrpg_boot rt_core.c loader.c svc.c hle_nn.c main.c rt_weak.c ${SMRPG_EXTRA_SOURCES})
target_include_directories(smrpg_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(smrpg_boot PRIVATE _WIN32_WINNT=0x0A00)
target_compile_options(smrpg_boot PRIVATE -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-comment)
target_link_libraries(smrpg_boot PRIVATE ${SMRPG_EXTRA_LIBS})
