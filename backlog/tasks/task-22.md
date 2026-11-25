---
id: task-22
title: Platform layer on SDL3: window, controllers and keyboard into hid, audio output behind --audio (D-012)
status: Verified
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
| AC1 | Same picture through SDL | Hidden muted 45 s run shows the Loading screen exactly as before (present 300 capture) | PNG on A:, log | PASS: `A:\AI\Recomps\smrpg-work\build\cap_t22\frame00300.png` (1920x1080, 2366 non-black pixels, same as the earlier baseline capture `captures\t21b\frame00600.png`; the only differing pixels, 900 of them in a 34x34 box, are the spinner). Log `build\logs\t22_final.log` |
| AC2 | Hidden by default | Default run creates no visible window (check window style and position from the log, and that no window appears on the desktop list) | log, `--window` off | PASS: log `window state (created)` and `(present 300)`: `IsWindowVisible=0 sdl_hidden=1`; `python -I tests/platform/run_platform_tests.py --game` polled `EnumWindows` 122 times for the process: window exists, never visible |
| AC3 | Input reaches the game | Scripted input (env or file, e.g. `SMRPG_INPUT=frame:button` like the baseline's route) writes hid state the game reads: a unit test on the shared memory layout, plus a log line from the game side showing the pad state changed | test output, log | PASS: unit test (hid layout through an independent reader that follows the SDK, 389 checks). Game side, run `SMRPG_INPUT="100:A:30,200:STICK_UP:40,260:PLUS:5"`: `hid read: nn::hid::GetNpadStates ... buttons=0x1`, then `buttons=0x20000 lstick=(0,32767)`, then `buttons=0x400`, each back to 0 after its length |
| AC4 | Controllers and keyboard | Real SDL gamepad and keyboard events map to the Pro Controller layout (unit test with synthetic SDL events) | test output | PASS: `tests/platform/test_platform.c`, 389 checks, 0 failed: every keyboard and gamepad button, triggers, dead zone, Y flip, hot-plug, two gamepads, key map ini and `config/input.default.ini` |
| AC5 | Muted unless --audio | Default run opens no audio device (SDL audio driver dummy and our null sink); `--audio` is the only path to a device and is not run by the worker | log | PASS: log `audio off: null sink, no device` and no `audio device open`; the unit test shows no SDL audio subsystem without `--audio`; the `--audio` path was tested only on the dummy driver. Real sound is not run |
| AC6 | No regressions | `tests/nvn/run_nvn_tests.py`, `tools/game_check.py run` pass | outputs | PASS: nvn unit 2177, gpu 101, cache 38+6 checks, 0 failed (`build\logs\t22_nvn_tests2.txt`); `game_check run` PASS (after `hle_hidshm.c`, see Log) |
## Human gates
Playing with a visible window is the owner's to do (`--window --audio`). Feel of the controls stays the owner's call.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Build `python -I tools/rtprep/build_game.py`; run `python -I tools/rtprep/run_game.py --seconds 45 --env SMRPG_CAPTURE_DIR=<dir> --env SMRPG_CAPTURE_EVERY=N --env SMRPG_NVDEC_OK=1`. Baseline recomp `A:\AI\Recomps\smrpg-elkiro` may be read for ideas on input routing (D-009), never copied.
## Log and handoff
2026-11-25 smrpg-platform: done, not committed. What was built: `runtime/platform/` (plat_core.c, plat_nvn.cpp, plat_audio.c, pad.c, hidshm.c, headers), `config/input.default.ini`, `docs/PLATFORM.md`, `tests/platform/` (unit test, runner with `--game` window check), `runtime/hle/hle_hidshm.c`.
Edits in other tasks' files, all small: `runtime/nvn/nvn_core.h` and `nvn_replay.cpp` (window provider hook, new `EnsureSwapchain`, window and swapchain section only), `nvn_api_device.cpp` (one line: surface extensions when a provider exists), `hle/hle_hid.c` (publish), `hle/hle_audio.c` (sink, `#ifdef SMRPG_PLATFORM`), `hle_nn.c` (guest-side `hid read` log for `GetNpadStates`), `main.c` (flags), `runtime/CMakeLists.txt` (additive platform block), `tools/rtprep/build_game.py` (copies SDL3.dll, libiconv-2.dll, input.default.ini), `tools/rtprep/run_game.py` (sets the dummy SDL audio driver for every run without `--audio`).
Finding: the hid lifo entry is 0x30 bytes (a u64 atomic sampling number before the 0x28 byte state), not 0x28. With the wrong stride the SDK spins in `GetNpadStates` (the game then never presented). Layout confirmed by disassembling sdk+0x4a2d10; it is written up in `hidshm.h` and `docs/PLATFORM.md`.
Finding: `test_runtime_boot.py`, `test_rthard.py`, `test_sync.py` compile `hle/*.c` without CMake; `hle_hid.c` needs the writer, so `hle/hle_hidshm.c` includes `../platform/hidshm.c` and every harness picks it up.
Finding: pad ticks need a 1 ms timer (`timeBeginPeriod(1)` plus the Windows 11 flag that makes a hidden-window process keep it); without it the 5 ms publish fell to about 90 per second.
The first full `run_nvn_tests.py` timed out in the cache sweep (file reads took 46 s per 4000 files, transient disk slowness; the same sweep takes 38 s now). The re-run passed.
Open: audren has no mixer, so `--audio` plays nothing yet (only the audout PCM path is wired). Needs its own card.
Owner gates: run `smrpg.exe --window --audio` once to see the window and feel the controls (not run here).
