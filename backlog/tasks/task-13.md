---
id: task-13
title: Horizon HLE services: sm, fs, os, time, settings, applet, hid, audio
status: Verified
---
## Problem before solution
After task-5 the boot stops at the first service request. Unity needs these services to reach level0.
## Outcome and scope
`runtime/hle/` (our own): service manager, fs (RomFS from the owner's extraction, save data), os sync and threads, time, settings, applet and account stubs, hid, audout. Each service logs unknown commands.
## Dependencies and ownership
Depends on task-5 (Verified). Owns `runtime/hle/`.
Execution: subchat, Sonnet high; split per service if it helps.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Boot passes every service call up to level0 load with 0 unknown commands on the path | Run | Log | PASS (technical) |
| AC2 | Memory-ordering and condvar tests pass before the first threaded boot | Test | Report | PASS |
## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Generated output and caches on A: only. Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-009: reuse first, then rewrite. Code reused from Snap (`A:\AI\Recomps\new-pokemon-snap`) or Ryubing (`A:\AI\Recomps\new-pokemon-snap\tools\ref\ryubing-ryujinx`) goes only in the gitignored `reuse/` folder, is listed in docs/REUSE_LEDGER.md, carries the reuse marker defined in `tests/test_no_reuse.py`, and gets its own rewrite card. Writing it ourselves directly is preferred when it is about as fast. Guard: `python -I tests/test_no_reuse.py`.
## Log and handoff
Record failed hypotheses and measured numbers.

### Result (worker, 2026-11-24)
**AC1 PASS.** Lifted build, NON-permissive (no `SMRPG_HLE_PERMISSIVE`): `A:\AI\Recomps\smrpg-work\build\logs\ac1_lifted.log` (and `disc76.log`). `BOOT MARKER: level0 read (first read of Data/level0)` at 0.58 s log time, `IPC: 61 distinct commands used, 0 unknown`. Services answered: sm, lm, apm, appletOE (ICommonStateGetter, ISelfController incl. cmd 44, IWindowController, ...), aoc:u, ectx:aw, pctl (pctl:s/a/r refused by the SAC as on console), time:u, fsp-srv (RomFS from `smrpg-work\v101\romfs`, save on A:), nvdrv, vi:u (vi:m/vi:s refused by the SAC), ldr:ro, nifm:u, bsd:u, ssl, hid. The same run in the IR interpreter (`SMRPG_NO_LIFTED=1`, `ac1_interp.log`) reaches the marker at 3.7 s with the same service sequence.
**AC2 PASS.** `tests/test_sync.py` (check id `sync`): 6 consumers / 2 producers condvar queue, timeout, signal and WaitForAddress cases pass. `python tools/game_check.py run --root .` PASS, `verify` PASS (includes `lifter`, `crypto`, `sync`).
**Not done / out of the AC1 path.** After level0 the lifted boot idles (main thread loops on a condvar wait with a 15 ms timeout, all other guest threads parked; WATCHDOG at 90 s). The interpreter run instead gets to `GetService("audren:u")` and sends IAudioRendererManager cmd 4 (GetAudioDeviceServiceWithRevisionInfo), which is logged as the one unknown command and ends in a guest panic (result 0x1ba0a). So audio is the next service work (null sink unless `--audio`); `audren:u`, `audout:u`, `hwopus`, `acc:u0` are registered in `runtime/hle/hle_audio.c` with no commands so the first unknown command of each is logged. Why the lifted run idles instead of reaching audren is not yet diagnosed (separate from services).
**Root cause that blocked the lifted boot (fixed in the lifter).** A deterministic `indirect call to null` inside a Unity formatter lookup (`f_7100478390`). Found by replaying lifted calls in the interpreter (`rt_diffcall`, hand-edited generated C): the jump-table `switch` used the table index register, which the compiler had reused (`mov x8, x0`) between the table load and the `br`, so every lookup took case 0. `tools/lifter/lift.py` now switches on the `br` target register itself (case per distinct target address, default falls to the dispatcher). All 2675 jump-table functions were relifted and recompiled. This bug could have hit any computed switch in the game.

### Deviations and decisions (all in our own files)
- Thread/suspend API is one header: `runtime/hle/hle_thread.h`.
- `runtime/svc.c`: SAC enforcement in `sm:GetService`; `commit_exact` commits per 64 KiB block (Windows cannot commit across separate reservations; svcMapSharedMemory failed with Win error 487); WaitProcessWideKey fixes; TIPC error convention; `SMRPG_DUMPADDR` (also at fatal errors, `rt_core.c`).
- `runtime/loader.c`: `nvnBootstrapLoader` HLE overrides the sdk's own export (the sdk carries the real NVN driver and talks to nvdrv); `provide_rtld_vars` (rtld normally fills the nn::ro list pointers).
- `runtime/hle/hle_gfxsvc.c`: `nn::vi::native::DeserializeNativeWindow` is overridden with an inert native window (function pointers are no-ops) because the real parse needs a live buffer queue on the relay; `ISelfController` cmd 44 (CreateManagedDisplaySeparableLayer) answers two layer ids. NVN presents through our own layer, not through this window.
- `SMRPG_HLE_PERMISSIVE=1` (unknown services/commands answer success with zeroes) exists for discovery runs only; the AC1 evidence does not use it. A global `g_ipc` critical section serializes IPC.
- `tools/lifter/ops.py`: CNTPCT gap closed; `tools/lifter/lift.py`: jump-table guard, `ret` flushes sp (a restored callee-saved register came from a deeper frame), jump-table switch on the target register (above). `runtime/crypto_a64.c`: interpreter crypto fallback; lifter crypto gap recorded.
- Build path for the game is `tools/rtprep/build_game.py` (CMake in `build
t-game`); `python tools/lifter/build.py link` fails to link the runtime (unchanged since task-5) and is not used. Flaky exit code 10 (heap reserve) is retried once by `run_game.py` users.
- Debug aids: `SMRPG_TRACE`, `SMRPG_MISSREGS`, `SMRPG_BREAKDBG`, `SMRPG_DUMPADDR`, `SMRPG_SLOTS`, `SMRPG_WDUMP`, `SMRPG_NO_LIFTED=1`, and `rt_diffcall` (interpreter replay of a lifted call, for hand-edited generated C only).
- Tests: `tests/test_runtime_boot.py` updated (boot no longer stops at the first nn symbol; mock throw expects the eh/ record; builds eh/ and gc/); `tests/test_sync.py` builds eh/ and gc/.

### Follow-on: lifted idle, audio, first frame (lead step list 1 to 3)
- **Idle after level0 (root cause, fixed in `runtime/svc.c`).** `svc_wait_pwk` registered the waiter after releasing the lock, so a signal between unlock and wait was lost; the main thread then slept out its 15 ms timeouts forever. Registration now happens under the lock before it is released. The interpreter run did not idle because its slower timing hid the window.
- **Audio.** `audren:u`, `audout:u`, `hwopus`, `acc:u0` answer their commands in `runtime/hle/hle_audio.c` with a null sink; `--audio` is the opt-in for a real device (not wired yet). `GetReleasedAudioOutBuffer` zero-fills its out buffer.
- **NVN wired.** `runtime/gfxcap.c` (`gfxcap_init`, called from `main.c`) registers `nvn_rt_register`, restores the `nvnBootstrapLoader` HLE slot (the nvn stub would override it), loads the shader cache and hooks `nvnQueuePresentTexture` for PNG capture. `runtime/nvn_host.cpp` holds the cache load and PNG writer. `rt_weak.c` carries a weak `gfxcap_init` so the mock boot test still links.
- **Shader cache.** `python -I tools/rtprep/merge_shader_cache.py` builds `A:\AI\Recomps\smrpg-work\shaders\cache\merged-v1` (25703 own v1 entries, 51 from reuse-v1, 0 missing). Default root; `SMRPG_SHADER_CACHE=off` or another path overrides.
- **Device info 0x5E** (SEPARATE_TEXTURE_BINDINGS_PER_STAGE) unanswered made the game leave per-program binding tables null and crash in lifted code. `gfxcap.c` answers 128 as a shim (`SMRPG_DEVINFO_5E` overrides). Proper fix belongs in `runtime/nvn/nvn_api_device.cpp` InfoValue (task-12).
- **Capture.** `SMRPG_CAPTURE_DIR`, `SMRPG_CAPTURE_FRAMES` (comma list of present numbers, default 1), `SMRPG_CAPTURE_EVERY`. Hidden run: `python -I tools/rtprep/run_game.py --seconds 40 --env SMRPG_CAPTURE_DIR=... --env SMRPG_CAPTURE_FRAMES=30,40`. Result: `A:\AI\Recomps\smrpg-work\captures\final2\frame00040.png` (1920x1080, 5979 non-black pixels, "Loading..." with spinner); frames 1 to 36 are black (the game clears before the loading screen). Exit 4 is the 40 s watchdog.
- **Open after this.** Game stalls after about 40 presents at `nn::hid::ShowControllerSupport` / `ILibraryAppletCreator` cmd 0 (controller-support applet HLE needed). nvn warnings: draw primitive 7 has no Vulkan topology; texture ids 0, 448, 2816, 3360, 3392 and sampler ids 0, 320, 576 not registered; the 11 embedded built-in programs (T2) are not in the cache, so draws using them are skipped.
