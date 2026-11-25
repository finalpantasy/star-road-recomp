# Star Road Recomp: present-time upscale pass and overlay hook (task-23). Our own code. Included by runtime/nvn/CMakeLists.txt after add_library(smrpg_nvn).
#
# upscale.cpp and overlay_stub.cpp are compiled into the NVN library. When the pinned FSR 1 sources are present (python -I tools/thirdparty/fetch_fsr.py puts
# them in SMRPG_FSR_DIR) and glslc is found, fsr_pass.comp is compiled to SPIR-V at build time (glslc -mfmt=c writes a C array that upscale.cpp includes) and
# SMRPG_UPSCALE_FSR is defined. Without them the library still builds; upscale = fsr1 then logs why and falls back to the plain blit. Nothing from FSR is committed.
set(_up_dir ${CMAKE_CURRENT_LIST_DIR})
set(SMRPG_FSR_DIR "A:/AI/Recomps/thirdparty/fsr" CACHE PATH "pinned AMD FidelityFX FSR 1 sources (tools/thirdparty/fetch_fsr.py)")
target_sources(smrpg_nvn PRIVATE ${_up_dir}/upscale.cpp ${_up_dir}/overlay_stub.cpp)
find_program(SMRPG_GLSLC glslc HINTS ${NVN_VULKAN_SDK}/Bin A:/msys64/ucrt64/bin)
if(EXISTS ${SMRPG_FSR_DIR}/ffx_fsr1.h AND EXISTS ${SMRPG_FSR_DIR}/ffx_a.h AND SMRPG_GLSLC)
  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/upscale_gen)
  file(MAKE_DIRECTORY ${_gen})
  set(_deps ${_up_dir}/fsr_pass.comp ${SMRPG_FSR_DIR}/ffx_a.h ${SMRPG_FSR_DIR}/ffx_fsr1.h)
  add_custom_command(OUTPUT ${_gen}/fsr_easu.inc
    COMMAND ${SMRPG_GLSLC} -fshader-stage=compute --target-env=vulkan1.3 -O -I ${SMRPG_FSR_DIR} -DSTAGE_EASU=1 -DOUT_FMT=rgba16f -mfmt=c ${_up_dir}/fsr_pass.comp -o ${_gen}/fsr_easu.inc
    DEPENDS ${_deps} VERBATIM)
  add_custom_command(OUTPUT ${_gen}/fsr_rcas.inc
    COMMAND ${SMRPG_GLSLC} -fshader-stage=compute --target-env=vulkan1.3 -O -I ${SMRPG_FSR_DIR} -DSTAGE_RCAS=1 -DOUT_FMT=rgba8 -mfmt=c ${_up_dir}/fsr_pass.comp -o ${_gen}/fsr_rcas.inc
    DEPENDS ${_deps} VERBATIM)
  set_property(SOURCE ${_up_dir}/upscale.cpp APPEND PROPERTY OBJECT_DEPENDS ${_gen}/fsr_easu.inc ${_gen}/fsr_rcas.inc)
  target_include_directories(smrpg_nvn PRIVATE ${SMRPG_FSR_DIR} ${_gen})
  target_compile_definitions(smrpg_nvn PRIVATE SMRPG_UPSCALE_FSR=1)
  message(STATUS "upscale: FSR 1 from ${SMRPG_FSR_DIR}, shaders by ${SMRPG_GLSLC}")
else()
  message(STATUS "upscale: FSR sources or glslc not found (run tools/thirdparty/fetch_fsr.py); upscale = fsr1 will fall back to the plain blit")
endif()
