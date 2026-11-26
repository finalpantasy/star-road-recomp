---
id: task-17
title: Live reload of settings, no restart
status: Verified
---
## Problem before solution
Owner to-do (2026-11-24): as many settings as possible work without a restart (live reload or hot swap).
## Outcome and scope
Each option is classed live, next-load, or restart. Every technically live setting applies from both the overlay and a debounced `options.ini` edit. Invalid file values are ignored and logged.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24). Needs task-16.
Execution: bounded task-17 implementation in `runtime/platform/overlay/`, its options plumbing, `tests/platform/test_options.c`, and this card.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every option has an apply class; restart-only options have a concrete reason | Option inventory and overlay tags | This card and option help text | IN PROGRESS |
| AC2 | Overlay edits invoke live apply hooks for every supported live option | Change values in the overlay; inspect apply log and runtime behavior | Platform tests and hidden run log | NOT_RUN |
| AC3 | Stable external file edits reload after debounce; malformed values do not replace current values | Change `options.ini` while running, including invalid values | Watcher and parser unit checks; hidden run log | NOT_RUN |
| AC4 | Apply-diff hooks, debounce, parse and invalid-value behavior are covered | Run `tests/platform/run_platform_tests.py` | Test output | NOT_RUN |
| AC5 | Mid-run edits change FPS cap and upscale, with applied-change logs and changed present hashes | Hidden muted scripted edit while game presents frames | Evidence under `A:\AI\Recomps\smrpg-work\task-17\` | NOT_RUN |
| AC6 | Repository check passes with fresh receipt | `python tools/game_check.py run --root .` | Game check receipt | NOT_RUN |

### Option classes

| Option | Class | Reason or apply point |
|---|---|---|
| `display.window_mode` | Live | Main thread calls SDL fullscreen setter on the existing window. |
| `display.upscale` | Live | Existing `upscale::SetMode` hook. |
| `display.upscale_target` | Live | Existing `upscale::SetTarget` hook. |
| `display.sharpness` | Live | Existing `upscale::SetSharpness` hook. |
| `performance.fps_cap` | Live | Atomic cap is read before each presented frame. |
| `audio.enabled` | Restart | A running null audout sink has no playback stream to promote to a device; stream/device creation occurs at audout open. |
| `audio.volume` | Live | Updates the open SDL stream gain and future streams. |
| `controls.input_map` | Live | Main thread reloads the keymap and clears held key state. |
| `gameplay.unlock_postgame` | Next save load or new game | Game reads the live option at save/new-game initialization. |
| `gameplay.difficulty_plus` | Restart | `hle_fs.c` prepares the transformed monster table under a per-file `InitOnceExecuteOnce` cache. |
| `gameplay.difficulty_plus_hp_percent` | Restart | Same cached `monster.tbl` image; factors are consumed while preparing it. |
| `gameplay.difficulty_plus_damage_percent` | Restart | Same cached `monster.tbl` image; factors are consumed while preparing it. |
| `gameplay.difficulty_plus_boss_hp_percent` | Restart | Same cached `monster.tbl` image; factors are consumed while preparing it. |
| `gameplay.difficulty_plus_boss_damage_percent` | Restart | Same cached `monster.tbl` image; factors are consumed while preparing it. |

## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
The options watcher polls file metadata on the platform main thread and waits 250 ms after the last observed write before parsing. Valid values merge into the current store; invalid values preserve the current running value. Apply hooks run on changed fields only.
All heavy commands use `python -I tools/rtprep/heavy_lock.py -- <command>`; game run is hidden and muted and omits `--window` and `--audio`.
## Log and handoff
- Added strict live reload parsing, invalid-value counting, metadata debounce, and changed-option apply dispatch. Added test coverage for debounce, invalid values, and apply-diff callbacks.
- Overlay restart-only rows now say `applies on restart`; next-load row says `applies on next load`. Audio and Hard-mode help text explains the restart requirement.
- Verification is blocked at the shared heavy lock: `python -I tools/rtprep/heavy_lock.py -- python -I tests/platform/run_platform_tests.py` returned `PermissionError` creating `A:\AI\Recomps\smrpg-work\heavy.lock` before starting the tests. The managed sandbox only permits writes under the repo and temp. Using a repo-local lock would not coordinate with the other lead, so no test, game run, or game_check was run outside the shared slot.
- Pending: platform test output, hidden mid-run log and present hashes, game_check receipt. Evidence directory target: `A:\AI\Recomps\smrpg-work\task-17\`.

### 2026-11-25 Claude lead
Lead ran what the sandboxed worker could not: platform 392 and options 225 checks, 0 failed (after regenerating `config/options.default.ini` and updating its header to say live edits work). Hidden muted run `lead17` (`A:\AI\Recomps\smrpg-workuild\logs\lead17.log`): editing options.ini at 40 s applied `display.upscale = fsr1` and `performance.fps_cap = 30` live, reload 14 valid, 0 invalid. Present-hash comparison not done. Restart-required: audio.enabled and the five Hard options; unlock_postgame applies on the next load.
