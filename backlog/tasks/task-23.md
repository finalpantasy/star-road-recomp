---
id: task-23
title: Upscaling with AMD FSR (MIT) behind an option, faithful mode default (D-004, D-012)
status: Planned
---
## Problem before solution
D-004 wants 4K and high resolution. The game renders at Switch resolutions; presenting at 4K needs a good upscaler as one route (the other route, native higher-resolution render targets, is its own card later). FidelityFX FSR is MIT and runs on Vulkan compute.
## Outcome and scope
An option `upscale = off | fsr1 | fsr2` (FSR 2 only if motion vectors are reachable; FSR 1 spatial first) applied at present time to the game's final image, sized to the SDL window or a target resolution, with sharpening as a setting. `off` is the faithful mode and the default. FSR source is fetched at a pinned release into `A:\AI\Recomps\thirdparty\fsr\` by a committed script; its shaders compiled at build time; nothing from it is committed.
## Dependencies and ownership
Depends on task-22 (SDL window and swapchain size). Owns the present and blit section of `runtime/nvn/nvn_replay.cpp`, `runtime/gfx/upscale/`, `tools/thirdparty/fetch_fsr.py`.
Execution: subchat, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Faithful default | `off` gives a byte-identical present to the current build at the same frame | hashes | |
| AC2 | FSR 1 works | 1080p game image to a 4K target, no validation errors | PNG pair on A:, validation log | |
| AC3 | Cost | GPU time of the pass at 4K measured | log | |
| AC4 | No regressions | NVN tests, game_check | outputs | |
## Human gates
The owner judges the look (ART_DIRECTION); technical PASS does not approve it.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Same build and hidden muted run as task-22.
## Log and handoff
