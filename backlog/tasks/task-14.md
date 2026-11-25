---
id: task-14
title: Runtime hard parts: exceptions, GC safepoints, generic method slots
status: Verified
---
## Problem before solution
RECOMPILER.md 4.4, 4.5 and the 7,212 runtime-filled generic method slots from task-2.
## Outcome and scope
Unwind-by-return exceptions with LSDA landing pads, cooperative GC safepoints with SuspendThread and GetThreadContext HLE, and the generic slot fill path, in the lifter and runtime.
## Dependencies and ownership
Depends on task-5 (Verified). Coordinates with task-13 on `nn::os`. Owns `runtime/eh/`, `runtime/gc/` and lifter changes it lists.
Execution: subchat, Opus high (reason: correctness-critical runtime design).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Throw, catch, finally and rethrow tests pass; 0 uncaught exceptions on boot | Test | Report | PASS (23/23, 0 uncaught; game boot 0 UNCAUGHT) |
| AC2 | GC stress test with live-object checker passes | Test | Report | PASS (0 violations; control run catches 1) |
| AC3 | Boot route shows 0 interpreter fallbacks for generic slots | Run | Log | PASS (static: 84,993 slot pointers, all lifted starts; boot: 0 generic slot misses, boot does not reach IL2CPP yet) |

Fast test: `python -I tests/test_rthard.py --real` (wired as `rthard` in tools/check-config.json, JSON report of 32 tests, exit 0 PASS, 1 FAIL, 3 NOT_RUN). Options: `--only eh|gc`, `--report out.json`. Run time about 1 to 2 minutes with a warm object cache.

### Evidence (2026-11-24)
| Check | Result |
|---|---|
| AC1 exceptions, test bed | 23/23 checks pass: catch by value, base (si), vmi base at an offset, virtual base, catch-all, skipped clauses, cleanup (finally), rethrow, throw inside a catch, object lifetime, pointer to base, const char*, callee-saved restore after a clobbering thrower, cross-module throw and catch both ways, cleanup in a foreign frame, 2 frames left to the interpreter, 2,000 throws in a loop. 0 uncaught. Status line `eh: runtime/eh active=yes ... vtables ok ... sdk redirects 11 (missing 0)` |
| AC1 LSDA cross-check, game images | `SMRPG_EH_SELFTEST`: 73,695 call sites, lifter tables vs runtime LSDA walk, 0 mismatches (FDEs: main 152,217, sdk 30,550, subsdk0 9,835) |
| AC2 GC stress | 3 mutators x 60,000 allocations, list heads only in registers and on the stack, loops with no svc: 21 rounds, about 176k nodes freed, 486 lists verified, 0 violations; 63 pauses, 60 parked at a back-edge poll, max wait under 1 ms |
| AC2 negative control | Same run with registers not scanned: 1 violation, as required (proves the roots are register-only at the safepoint and that the checker sees a premature free) |
| AC3 static | `tools/lifter/check_generic_slots.py`: 84,993 unique code pointers across module methodPointers, genericMethodPointers, invokers, module and generic adjustor thunks, unresolved virtual call stubs, interop and reverse P/Invoke; all are lifted starts, 0 listed with a reason, 0 bad generic method table rows |
| Lifter regression | `tests/test_lifter.py`: instruction oracle vs Unicorn 37,920 compared, 0 mismatches; lift and compile smoke PASS |
| Sample lift | sdk 4,032 call sites with 1,632 pads; main 4,478 sites with 2,043 pads; both chunks compile clean |

| Full relift | main 157,215 functions (1 listed with a reason), 67,017 EH sites, 25,137 pads, 164,506 polls; sdk 39,508 (42 listed), 6,678 sites, 2,618 pads, 35,846 polls; subsdk0 13,625, 22,653 polls; lift wall 16 s. All 529 chunks compile, 0 failures |
| Game boot (`build/logs/task14_boot.log`, 60 s hidden, 2026-11-24) | `eh: runtime/eh active=yes (HLE_OVR order), vtables class ok si ok vmi ok ptr ok, eh_frame_hdr indexed 3/3 modules ..., sdk redirects 25 (missing 0)`; 0 UNCAUGHT lines; ends at the same point as task-13's latest boot (vi relay, watchdog); interpreter misses: the same 2 mid-function indirect targets (main+0x529144, sdk+0x3a5b5c) already present in task-13's disc10 to disc23 boots, both at 0.09 s, neither a generic slot |
| With task-13 hooks (real svc.c, rt_core.c, hle_nn.c; 2026-11-24) | `test_rthard.py --real`: AC1 23/23, AC2 0 violations (control 1), AC3 PASS, no patched copies. Boot `build/logs/task14_boot2.log`: eh active, 25 redirects; summary lines `eh: throws 0 ... no-site 0` and `gc: pauses 0 ...`; 0 UNCAUGHT. Boot stops at 0.129 s on an sdk Panic svcBreak after `sm:GetService("hid")` returns 0x1015 (not registered): task-13 frontier, not an exception path |
| Compile time | Polls first doubled clang time (mean 13.2 s to 28.9 s per chunk, max 526 s to 842 s). Fixed: a poll flushes only dirty AND live registers (backward liveness, `poll_liveness`) and stores 0 into dead dirty ones; sp always flushed. Sample chunk main_0083: poll overhead 2.0x to 1.3x CPU, register allocator time equal to no polls. Compile now starts the biggest chunks first: slowest chunk 546 s (main_0208), first pass 130 chunks in 549 s, second pass 290 chunks in 170 s |

The boot does not reach IL2CPP yet, so AC3 at run time stays trivially 0 until it does; the static check covers every pointer that can be stored in a slot. The eh and gc summary lines appear in the boot log once the rt_core.c hook below lands.

## Design as built
- Exceptions (RECOMPILER.md 4.4): every `bl`/`blr` is followed by `if (c->exc) ...`. `tools/lifter/eh_tables.py` maps each call site to its FDE, LSDA call-site entry and landing pad. Site kinds: no LSDA (propagate), pad (call `eh_land(c, lsda, action)`, which walks the action chain at run time; on a match restore x19 to x29, sp and v8 to v15 from the function's locals, reload x0/x1 and jump to the pad, or `musttail rt_dispatch` when the pad is outside the function), no pad (propagate), no call-site entry (`eh_no_site` logs and propagates, like std::terminate would be reached). Functions with pads keep callee-saved registers as locals so the restore is exact.
- The sdk calls its own `__cxa_*` and `_Unwind_*` directly; 27 names (`REDIRECT_NAMES`) are redirected: direct `bl` becomes `rt_icall`, tail `b` becomes `rt_dispatch`, and `reg_zz_post` (last registry chunk) calls `eh_post_register`, which registers runtime/eh implementations at those addresses.
- The interpreter snapshots x19 to x29, sp and v8 to v15 before each call and lands through `eh_after_call`, so interpreted frames catch and clean up too.
- GC (4.5): cooperative safepoints. Lifted code polls `g_gc_pending` on every backward branch and back-edge jump table (flushing dirty registers first); the interpreter does the same on taken backward branches. svcSetThreadActivity(pause) returns once the target is SAFE (inside an svc or a blocking HLE wait), PARKED (at a poll), not started or exited. svcGetThreadContext3 returns the Ctx only for a stopped thread. Host SuspendThread was rejected: the Ctx is stale between flushes (Boehm would free live objects) and the thread can be stopped inside host malloc.
- Generic slot fill path (4.6): il2cpp::vm::MetadataCache in lifted libil2cpp fills slots from the registration tables; calls through them are `blr` resolved by `rt_icall` in the dispatch table. 0 fallbacks holds when every non-null table pointer is a lifted start, which check_generic_slots.py proves from main.img, registrations.json, the inventory and lift_main.json.

## Files changed
- Lifter: `tools/lifter/lift.py` (pads, polls, redirects, counters), `tools/lifter/build.py` (redirect table, `reg_zz_post`, eh and gc in RT_SRC, counters), new `tools/lifter/eh_tables.py`, new `tools/lifter/check_generic_slots.py`. (`tools/lifter/ops.py` has a CNTPCT change that is not task-14's.)
- Runtime: new `runtime/eh/eh.c`, `eh.h`, `runtime/gc/gc.c`, `gc.h`; `runtime/interp.c` (landing and back-edge polls); `runtime/rt.h` (declarations).
- Lifter (compile time): `tools/lifter/lift.py` `poll_liveness` and dead-register zeroing (A/B switch `SMRPG_POLL_LIVE=0`); `tools/lifter/build.py` compiles biggest chunks first and prints the 5 slowest.
- Tests: new `tests/rthard/` (mkimg.py and the AArch64 guest sources; built and lifted on A: only), new `tests/test_rthard.py`; `tools/check-config.json` (`rthard` entry).

## Task-13 hooks
All landed (2026-11-24, smrpg-hle): CMake sources, svc.c gc include, safe region around the svc handler, svcSetThreadActivity through gc_set_activity, GetThreadContext3 stopped check, eh and gc summaries in rt_summary, rt_poll to gc_poll, old EH block in hle_nn.c removed. test_rthard.py still patches test copies only if a hook is missing.

## Recovery and return condition
Generated output and caches on A: only (`A:\AI\Recomps\smrpg-work\build\rthard\`). Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-009: reuse first, then rewrite. All task-14 code is our own; nothing in `reuse/`. Guard: `python -I tests/test_no_reuse.py`.
## Log and handoff
Failed hypotheses and fixes:
- sdk trap stubs in the same file as the throwers were inferred noreturn, so the throw functions fell through; moved to `sdk_abi.c`.
- GC control run hung on a cyclic list built from freed nodes; verify is bounded and the control stops at the first violation.
- AC3 first reported 1,992 bad generic method table rows from a wrong field order; the row is (spec index, pointer index, invoker index, adjustor index); now 0.
- Windows CreateProcess needs `ld.lld.exe`; tools are resolved with `shutil.which`.
- Poll liveness, first try: flushing only live registers (skipping dead dirty ones) livelocked the GC stress test: a mutator spinning in alloc never calls, so the stale context values of dead registers kept old garbage reachable on every round. Fix: store 0 into dead dirty registers. Boehm in main (f_7101108040) pushes x0 to x28, fp, lr, sp and the low half of v0 to v31 from GetThreadContext, so v registers cannot be skipped.
- `-fno-slp-vectorize` saves only about 10% of compile time and changes codegen everywhere; not applied.
- `tools/lifter/build.py link` (legacy link) misses task-13's runtime/hle symbols (`hle_ipc_summary` and others); `tools/rtprep/build_game.py` is the working path.
- `tests/test_runtime_boot.py` (mock game) now fails on `missing in log: FIRST NN SERVICE REQUEST`: task-13 removed that log line in hle_nn.c; the test expectation is theirs to update. Not caused by task-14 (the test does not build interp.c, eh or gc).
