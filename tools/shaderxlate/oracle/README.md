# vkdiff

Headless Vulkan shader differential harness. Renders SPIR-V variant A (our translator) and
variant B (reference translator) with identical inputs on lavapipe (CPU Vulkan) and compares
every 32-bit output component. It refuses to run on a real GPU (needs a CPU-type device and
both `VK_ICD_FILENAMES` and `VK_DRIVER_FILES` set). No window, no surface extensions.

## Build

    set bin=<llvm-mingw>\bin
    cmake -S . -B A:\AI\Recomps\smrpg-work\build\vkdiff\cmake -G "MinGW Makefiles" ^
      -DCMAKE_CXX_COMPILER=%bin%\clang++.exe -DCMAKE_MAKE_PROGRAM=%bin%\mingw32-make.exe ^
      -DVULKAN_INCLUDE=<vulkan_sdk>\Include
    cmake --build A:\AI\Recomps\smrpg-work\build\vkdiff\cmake

The loader (vulkan-1.dll) is loaded at run time, so no import library is needed.

## Run

    set VK_ICD_FILENAMES=<mesa>\lvp_icd.x86_64.json
    set VK_DRIVER_FILES=<mesa>\lvp_icd.x86_64.json
    vkdiff.exe jobs.tsv results.tsv [--validate] [--dump DIR] [--append] [--time]

`--validate` needs `VK_LAYER_PATH` pointing at the SDK `Bin` directory. `--append` keeps an
existing results file (use it when restarting after exit code 3). `--time` prints ms per job.

Exit codes: 0 done, 2 usage or init failure, 3 a job timed out (20 s fence wait; results are
flushed, restart from the next case), 4 device lost.

## Files

jobs.tsv, tab separated, one job per line:
`case_id mode helper_vs helper_gs helper_fs a_spv a_layout b_spv b_layout seed`
(`mode` is `fs` or `vs`; unused helper paths are `-`).

results.tsv, one line per job:
`case_id status values exact close bad max_abs_diff first_bad message`
Status is `equal`, `differ` or `error`. `first_bad` is `rtN:x,y,c:a=..:b=..`. The clear value
is the NaN `0x7FC0DEAD`; it must match exactly, so discarded pixels show up as differences.

Layout files list `ubo SET BINDING SLOT`, `support SET BINDING`, `tex SET BINDING DIM SHADOW KEY`,
`attr LOC`, `rt N`, `fb W H`. Unknown lines are ignored with a warning.

## Self-test

    powershell -File selftest.ps1 [-Validate]

Compiles the GLSL in `selftest\` with glslangValidator into
`A:\AI\Recomps\smrpg-work\build\vkdiff\selftest\` and runs four jobs: fs equal, fs differ
(B scaled by 1.01), vs equal (GS helper, `fb 2 64`), and a missing-file error.
