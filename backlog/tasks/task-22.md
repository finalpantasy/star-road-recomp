---
id: task-22
title: Platform layer on SDL3: window, controllers and keyboard into hid, audio output behind --audio (D-012)
status: Ready
---
## Problem before solution
The runtime has no host input at all (hid hands the game empty pad state), its window is a hidden Win32 popup made inside `runtime/nvn/nvn_replay.cpp`, and audio only goes to a null sink. Nobody can play it. SDL3 (zlib license, no credit needed) gives window, controllers with hot-plug and the community mapping database, keyboard and audio output, and it is what the overlay (task-16) and upscaling (task-23) build on.
## Outcome and scope
A small platform layer `runtime/platform/` on SDL3: one window (hidden by default for test runs, shown only with `--window`), Vulkan surface from SDL, controller and keyboard state written into the hid shared memory the game reads (Pro Controller layout, default keyboard map in a config file), and audout/audren mixed output to a real device only with `--audio` (null sink stays the default). Out of scope: the settings overlay (task-16), upscaling (task-23), rebinding UI.
## Dependencies and ownership
Depends on task-12 (window and swapchain code), task-13 (hid and audio HLE), D-012. Owns `runtime/platform/`, the window and surface section of `runtime/nvn/nvn_replay.cpp` (window creation and swapchain only; texture upload there belongs to task-21 while it runs), `runtime/hle/hle_hid.c`, `runtime/hle/hle_audio.c` output path, `runtime/main.c` flags, additive lines in `runtime/CMakeLists.txt`. SDL3 comes from `A:\msys64\ucrt64` (headers, import lib, `SDL3.dll` copied beside the exe by `build_game.py`); nothing from SDL is committed.
Execution: subchat smrpg-platform, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Same picture through SDL | Hidden muted 45 s run shows the Loading screen exactly as before (present 300 capture) | PNG on A:, log | |
| AC2 | Hidden by default | Default run creates no visible window (check window style and position from the log, and that no window appears on the desktop list) | log, `--window` off | |
| AC3 | Input reaches the game | Scripted input (env or file, e.g. `SMRPG_INPUT=frame:button` like the baseline's route) writes hid state the game reads: a unit test on the shared memory layout, plus a log line from the game side showing the pad state changed | test output, log | |
| AC4 | Controllers and keyboard | Real SDL gamepad and keyboard events map to the Pro Controller layout (unit test with synthetic SDL events) | test output | |
| AC5 | Muted unless --audio | Default run opens no audio device (SDL audio driver dummy and our null sink); `--audio` is the only path to a device and is not run by the worker | log | |
| AC6 | No regressions | `tests/nvn/run_nvn_tests.py`, `tools/game_check.py run` pass | outputs | |
## Human gates
Playing with a visible window is the owner's to do (`--window --audio`). Feel of the controls stays the owner's call.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Build `python -I tools/rtprep/build_game.py`; run `python -I tools/rtprep/run_game.py --seconds 45 --env SMRPG_CAPTURE_DIR=<dir> --env SMRPG_CAPTURE_EVERY=N --env SMRPG_NVDEC_OK=1`. Baseline recomp `A:\AI\Recomps\smrpg-elkiro` may be read for ideas on input routing (D-009), never copied.
## Log and handoff
