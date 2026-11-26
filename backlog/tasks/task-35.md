---
id: task-35
title: Verify a fresh Windows runtime through the Bandit's Way reload
status: Verified
---

| ID | Criterion | Result |
|---|---|---|
| AC1 | Build the current worktree with the current lifted objects | PASS. The isolated build used `c263850` with the worktree changes and the shared `link.rsp`. Executable SHA-256 `ae5fbca20f9765e562e0a52531e6ce693b5cc828798e13e7c1aa99f8f1512ae6`. |
| AC2 | Survive the startup copy with Vulkan validation enabled | PASS. The bounds guard rejected an out-of-range buffer-image copy after present 840. The executable reached the 95-second cutoff with zero Vulkan errors, unknown IPC commands, skipped draws, or pipeline failures. |
| AC3 | Reload the Bandit's Way checkpoint and move Mario | PASS. Both party members appear in the field HUD. The game read the stick input at present 4202, and Mario changed position between captures 4080 and 4560. |
| AC4 | Run the renderer's unit and GPU regression checks | PASS. The unit suite passed 2,168 checks and the GPU suite passed 183, with zero Vulkan validation errors or warnings in the GPU suite. |

## Evidence

- `scratch/codex_bandits_fresh_runtime` on A: contains the executable, build log, input script, captures, and `nvn-reports/NVN_TEST_REPORT.md`. The hidden, muted replay used a copy of task 34's `d03out` autosave. Present 7920 logged 1,472,903 draws and 45 pipelines. The validator reported the existing unused fragment output warning during gameplay.
- The shared executable still has the SHA-256 recorded for task 34's startup failure. The verified executable is in the isolated `bin` folder. Same-version Switch visual parity remains open.
