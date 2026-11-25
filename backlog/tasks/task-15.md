---
id: task-15
title: Baseline: build the released SMRPG recomp from source and capture frames (D-010)
status: Verified
---
## Problem before solution
D-010 makes the public MIT recomp (ElKiro1311/smrpg-recompiled, LASRecomp base, plays 1.0.0) our playable baseline and a reference oracle. Until it builds and runs here from the owner's own dump, we have no images and no oracle to test our runtime against.
## Outcome and scope
The recomp builds from source on A: from the owner's 1.0.0 dump, runs on a hidden desktop and muted, and leaves PNG frames of boot, title screen, difficulty menu and the new-game cutscene. Out of scope: our own code, 1.0.1 support, playing past the cutscene, installing anything from its prebuilt installer or releases (none was run).
## Dependencies and ownership
Depends on D-010 and task-1 (extraction). Owns `A:\AI\Recomps\smrpg-elkiro` (clone, builds, caches) and this card. Nothing from it enters the F: repo (D-009).
Execution: subchat smrpg-baseline, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Builds from source | `make gen` then `make -j8` exit 0, `build/smrpg-recomp.exe` and `build/lasgame.dll` exist | `A:\AI\Recomps\smrpg-work\logs-elkiro\gen.log`, `make.log` | PASS |
| AC2 | Runs hidden | Ghost desktop launch, 150 s, no window on the owner's desktop, 60 fps, 0 skipped draws | `logs-elkiro\cap1.log` | PASS |
| AC3 | Title-screen PNG on A: | Frame captures at fixed frame numbers | `captures\baseline\frame_00400.png` (title) and 9 more | PASS |
| AC4 | Runs muted, proven | Windows audio session API on all 7 active render endpoints, polled every 0.25 s from launch, game tree has 0 sessions for 150 s | `elkiro-run\launch_silent.py` output in the log below | PASS (run cap1 only; see Log) |
## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Everything is on A:. Rebuild: `make gen`, `make -j8` in the clone. Return once at done or blocked.
## Approach and commands
Pin: `62a83d09922bf8f72d7c0650b5f64141ba54c290` (2026-11-23, "0.52.0-beta", base beta_5.4, game 1.0.0, main build ID e968832cade2ad7c). No local patches (`git status` clean; `build/`, `generated/`, `extracted/`, `cache/`, `save/`, `tools/venv/` are ignored).

Environment (all on A: where possible):
- MSYS2 UCRT64 already at `A:\msys64` (clang 23.1.5, lld, llvm, SDL3 3.4.20, FFmpeg 9.1, shaderc, spirv-tools). Added with pacman (no -Syu): `make`, `mingw-w64-ucrt-x86_64-vulkan-devel` pieces (headers, utility libs), `vulkan-memory-allocator`, `nasm`, `vulkan-validation-layers`. `gdb` not installed (not needed).
- Python venv `A:\AI\Recomps\smrpg-elkiro\tools\venv` from Python 3.13 (their script wants 3.12; 3.13 worked) with lz4, capstone, unicorn. Did not run `tools/setup_windows.ps1` (it installs MSYS2 to C:).
- Game files: `A:\AI\Recomps\smrpg-work\base\exefs` existed; the base 1.0.0 romfs was missing, so I extracted it from the cached XCI program NCA with hactool (`xci_secure`, filtered keys, key values never printed): 1,934 files, 6,820,987,323 bytes, 33 s. Then `extracted\exefs` and `extracted\romfs` in the clone are NTFS hard links to `smrpg-work\base\...` (1,939 files, no extra space). The v101 romfs is not used (the recomp targets 1.0.0).
- Build: `export PATH=/a/msys64/ucrt64/bin:/a/msys64/usr/bin:$PATH; make gen; make -j8` from the clone. `make gen`: 2 min 17 s (869 chunks, 154,120 main files). `make -j8`: 14 min 03 s (the runtime plus `lasgame.dll`, 270 MB). Sizes: generated 732 MB, build 638 MB.
- Run: only via `A:\AI\Recomps\smrpg-work\elkiro-run\launch_silent.py <log-name> <max-seconds> [KEY=VALUE ...]` (see below). Capture run used `LARECOMP_INPUT="1300:A:8,1600:A:8,1900:A:8,2200:A:8"` (from their STATUS.md route) and `LARECOMP_SHOT_AT="120,400,700,1100,1450,1750,2600,3600,4800,5400"`, which writes `<clone>\build\frames\frame_N.png` (1280x720, copied to `captures\baseline\`, plus `contact_sheet.png`).
- Patches needed: none. Deviations: BelowNormal was NOT applied to the build or to the first runs (my `/proc/$$/winpid` call failed); the later runs through `launch_silent.py` are BelowNormal.
## Log and handoff
Frames (`A:\AI\Recomps\smrpg-work\captures\baseline\`): `frame_00120.png` black "Loading", `frame_00400.png` to `frame_01100.png` title screen (logo over Bowser's Keep, with the copyright line), `frame_01450.png` difficulty menu (Breezy / Normal), `frame_01750.png` Peach cutscene, `frame_02600.png` lightning, `frame_03600.png` and `frame_04800.png` and `frame_05400.png` the Keep. First gameplay scene not captured (needs more scripted input; their doc: `LARECOMP_INPUT`, `LARECOMP_WARP_AT` needs a save).

Logs in `A:\AI\Recomps\smrpg-work\logs-elkiro\`: `gen.log`, `make.log`, `run1.log` (first 8 min run), `cap1.log` (the muted capture run), `guard1.log`.

Measured: boots in about 4.5 s to the first presented frame, reaches 60 fps by frame 340, then 60.0 fps with about 600 draws, 0 skipped, 1,089 passes per frame (RTX 4090, Vulkan 1.3 driver 621.24), 386 MB texture upload by the title. First run took frame 28,000 in 476 s (60.0 fps).

Incident and what we learned (record, do not repeat):
- The game made sound on the owner's real audio device during some of my early runs (6:52 and 6:53 PM). Cause not proven. Env vars exported in bash did not take effect in several launches (a `LARECOMP_FPS_INTERVAL`, `LARECOMP_DATA_ROOT`, `LARECOMP_SHOT_AT` check showed no effect), while the same vars reached a test exe and a python child through the ghost launcher. They worked once I passed an explicit environment dict from Python. So `LARECOMP_NO_AUDIO` was probably not applied in those runs. I also ran one 8 s env test before the mute proof existed, and one guard run in which the guard crashed (see next) so the game ran about 65 s unguarded; I killed it by PID. Sound during those two cannot be excluded.
- The first audio guard segfaulted after a few polls (a ctypes `cast` double-released the COM interface). Fixed with `QueryInterface`; soak-tested 115 polls plus a selftest that detects a silent `ffplay anullsrc` stream as Active.
- Current protocol (all in `smrpg-work\elkiro-run\`): `launch_silent.py` sets `SDL_AUDIO_DRIVER=dummy`, `SDL_AUDIODRIVER=dummy`, `LARECOMP_NO_AUDIO=1`, `LARECOMP_NO_MOVIE_AUDIO=1`, `LARECOMP_HIDDEN=1`, a settings file (`larecomp.ini`: volume 0.00, movie_audio 0, reverb 0), runs the ghost launcher at BelowNormal, puts the launcher in a kill-on-close job, polls IAudioSessionManager2 on all active render endpoints every 0.25 s from launch and kills the tree by PID on any Active session (exit 3) or an API error (exit 4), and prints a verdict at 5 s. `audio_guard.py selftest` proves the detector. Run cap1: sessions ever seen: none; exit 0.
- The game logs `[audout] opened 48000 Hz, 6 ch` even with `LARECOMP_NO_AUDIO=1`; that is the HLE service state, not a device (`runtime/services/audio.c` returns before `SDL_OpenAudioDeviceStream`).

## Worth using as a reference oracle for our runtime (all in `A:\AI\Recomps\smrpg-elkiro`; read, do not copy into F: until D-009 ledger entries exist)
1. Frame oracle: `captures\baseline\*.png` at fixed frame numbers with a fixed `LARECOMP_INPUT` route; their hooks `LARECOMP_SHOT_AT`, `LARECOMP_SCREENSHOT_EVERY`, `LARECOMP_WINDOW_SHOT`, `LARECOMP_SHOT_RAW` (exact texture contents), `LARECOMP_HOST_INPUT` (`frame:shot:<name>`) give repeatable images to diff our first frames against (title at frame about 400, difficulty menu about 1450, Peach cutscene about 1750).
2. Service call trace: the log prints `[sm] OpenSession`, `[ipc]`, `[am]`, `[kernel] CreateThread`, `[input]`, `[audout]`; `runtime/services/{am,acc,fs,hid_set,time,audio,hwopus,misc}.c` and `runtime/hle/` list which services and commands the game needs to reach the title (first unhandled ones are only logged: `ectx:aw` gets a logging stub). Direct comparison for task-13: our boot should open the same sessions in the same order.
3. NVN to Vulkan layer: `runtime/nvn/` (`cmdbuf.c`, `objects.c`, `tegra.c`, `formats.c`, `astc.c`, `rqueue.c`) and `runtime/nvn/vulkan/` (`device.c`, `exec.c`, `record.c`, `textures.c`, `texpack.c`, `shaders.c`); log line `[vk] ... draws, skipped, passes, flushes, upload` per 2 s is a per-frame cost oracle for task-12.
4. Shader handling: Maxwell to SPIR-V/GLSL translator `runtime/nvn/maxwell/translate.c` with a cache in `cache\shader_code` (102 files after one run), `cache\spirv`, `cache\vk_pipelines.bin`, `shader_seen.txt` (the shaders actually used on the path to the cutscene) and `runtime/nvn/shader_variants.txt`. Task-10 can compare its translator output and shader coverage against `shader_seen.txt`; their title and cutscene draw with 0 skipped draws.
5. Recompiler oracle: `tools/gen.py` (lifter, `--simd=c` for x86), `tools/game_addresses.py` and `runtime/game_addresses/*.h` (address tables by build ID e968832cade2ad7c), `tools/game_overrides.c` (their replacements of generated functions), `generated\main` (871 chunks, rebuilt from our dump): function boundaries and counts (154,122 + 39,483 functions in `lasgame.dll`) to cross-check task-5 coverage.
6. Behavior fixes worth knowing about for 1.0.1 and the decomp: STATUS.md lists the guest-memory, condvar and memory-ordering fixes that got it past level0 (versions 11-16), CRIWARE VP9 movie decode through FFmpeg and the `hwopus` service, and `runtime/hle/unity_screen.c`, `movie.c` (screen and movie handling).
7. Timing: 60 fps locked from frame 340, about 4.5 s to the first frame, 14 min full build, 2 min generate; a performance floor for our own runtime.

## Open items
- First gameplay scene frame (needs a save or longer input route).
- Per-service trace file (a clean `[sm]`/`[ipc]` list from `cap1.log`) not extracted yet; a one-line grep of `cap1.log` gives it.
- Whether the earlier sound came only from missing env vars is unproven; the muted protocol is the standing rule for every launch.
