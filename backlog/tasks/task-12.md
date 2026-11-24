---
id: task-12
title: NVN on Vulkan: device, memory, command buffers, present
status: Verified
---
## Problem before solution
Unity calls 277 NVN procs through nvnBootstrapLoader. The runtime needs a Vulkan implementation.
## Outcome and scope
`runtime/nvn/` (our own): bootstrap loader and proc table, device, queue, memory pools, buffers, textures and samplers (handles separate), command buffers and recording, sync, window and present to a hidden window or offscreen target. Draws use shaders from task-10 through a stable interface; stub it until task-10 lands.
## Dependencies and ownership
Depends on task-3, task-4. Interfaces with task-10 and task-11. Owns `runtime/nvn/`, `tests/nvn/`.
Execution: subchat, Sonnet high (Opus for unclear failures via the lead).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | All 277 procs Unity imports are implemented or logged as stubs with a reason | Table | Doc | PASS: docs/NVN.md. 535 table procs (332 implemented, 203 logged stubs with reasons); the measured used list has 265 names (card said 277): 263 implemented, 2 stubbed with reasons. Checked each run by nvn_unit. |
| AC2 | Standalone test: clear, textured quad and readback with a hidden window, Vulkan validation layers clean | Test | Report | PASS: tests/nvn/reports/NVN_TEST_REPORT.md. nvn_gpu 75 checks, 0 failed, validation 0 errors and 0 warnings on an RTX 4090; window never visible (IsWindowVisible false before and after 3 presents). Also passes with --no-present and --no-validation. |
| AC3 | RGBA4 order, depth range 0 to 1, NVNtexture 0x80 and view 16 bytes checked by unit tests | Test | Report | PASS: nvn_unit 2161 checks, 0 failed (sizes, canaries, RGBA4 to A4B4G4R4, depth enums); nvn_gpu reads back RGBA4 texel order and depth under both depth modes. Same report. |
## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Generated output and caches on A: only. Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-008: our own code; Snap (A:\AI\Recomps\new-pokemon-snap) and Ryubing (A:\AI\Recomps\new-pokemon-snap\tools\ref\ryubing-ryujinx) may be read for lessons and run as test oracles only, never copied.
Run: `python -I tests/nvn/run_nvn_tests.py` (builds into A:\AI\Recomps\smrpg-work\build\nvn, runs both tests, writes the report). Doc: docs/NVN.md.

## Log and handoff
Record failed hypotheses and measured numbers.
- 2026-11-24 done. First GPU run crashed inside the driver: the device feature chain wrote its pNext link at offset 4 instead of 8 (fixed with VkBaseOutStructure). Replay recorded layout changes and a dummy-image clear inside a render pass (validation errors); resources are now prepared before BeginRendering. The present semaphore was reused across swapchain images (now one per image). No device teardown of pipelines, shader modules, dummies or scratch (30 leaked objects; now ReplayShutdown). The swapchain device extension was enabled without the surface instance extension when presenting was off.
- Measured: nvn_unit 0.2 s, nvn_gpu about 2 s. Depth NEAR_IS_ZERO: z 0.5 and 0.25 drawn with depth 0.5 and 0.25, z -0.5 and 1.5 clipped. NEAR_IS_MINUS_W: depth 0.75, 0.25, 0.625, z 1.5 clipped.
- Open for later tasks: the runtime build still has to add the three lines in docs/NVN.md (task-5); partial clear masks, alpha test, ReportCounter and vertex divisor above 1 are listed limitations.
