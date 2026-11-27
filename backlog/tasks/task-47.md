---
id: task-47
title: Wrapped chunk size and link time, build every generated function
status: Review
---

## Observed behavior

Task-46: each wrapped lifted chunk added about 4.5 MB to `smrpg.exe` (948 MB with 120 wrapped chunks) and the link of more than 120 chunks took over 10 minutes, so only 3,778 of 12,032 generated functions were built.

## Cause (measured)

Object of `main_0001`, unwrapped (`build\obj\main_0001_*.o`, compiled by `tools/lifter/build.py`) against wrapped (`rt-game\CMakeFiles\smrpg_boot.dir\main_0001_decomp.c.obj`, compiled by `runtime/decomp/game.cmake` through the target flags):

| Object | Total | .text | DWARF (.debug_info, .debug_loc, .debug_line, .debug_str, .debug_ranges, .debug_abbrev) |
|---|---|---|---|
| unwrapped | 55,333 B | 53,695 B | none (156 B CodeView stub) |
| wrapped | 604,849 B | 53,695 B | about 550 KB (288 KB info, 200 KB loc, 35 KB line, 14 KB str, 11 KB ranges) |

Code is byte-for-byte the same size, so there is no second copy of the chunk, no changed inlining and no duplicate COMDAT; `-O2 -march=x86-64-v3 -ffp-contract=off -fno-strict-aliasing` are the same on both sides. The only difference is `-g`: the runtime target passes `-O2 -g` to every source, the lifted objects are built without `-g`. A wrapped chunk therefore carried 10x its own size in DWARF, and the linker had to merge it all.

## Fix

`runtime/decomp/game.cmake` line 72: wrapped chunk source options `-w;-fno-exceptions` became `-w;-fno-exceptions;-g0`. Only wrapped lifted chunks are affected; no optimization flag changed and no unwrapped object changed. `swap.c` and the Rust archive keep `-g`.

## Result

| Step | Result |
|---|---|
| Same 120 chunks, 3,778 functions, `-g0` (recompile 120 chunks and relink) | 208 s, `smrpg.exe` 947,760,128 B to 311,558,656 B |
| All generated functions: `generate.py --limit 100000 --max-chunks 0 --exclude-chunks 169`, 12,028 functions in 350 chunks, then `build_game.py --jobs 16` (recompile 230 more chunks, relink) | 322 s in total, `smrpg.exe` 314,381,312 B (350 wrapped chunks add about 13 KB each over 120: link time is no longer the cap) |
| Registered rows | 12,056 (28 hand-written, 12,028 generated) |

main_0169 stays excluded (4 functions, its 7.3 MB of C was not tried again). Every build or link ran under 10 minutes (longest 322 s, run with a 590 s `timeout`).

## Acceptance and evidence

| Check | Result |
|---|---|
| Hidden muted A/B run with selftest, fresh copy of the task-42 save in `task47_rt\t42` (`task47_ab2.log`) | `decomp mode=ab registered=12056`, selftest clean for 12,056 rows (about 60 s startup), 0 `decomp mismatch`, exit code 4 (watchdog), 97 distinct IPC commands, 0 unknown, interpreter misses none, live differences 0 |
| First A/B attempt (`task47_ab1.log`) | 0 mismatch lines but no EXIT line: `--seconds 215` of `run_game.py` killed it at 245 s because the game watchdog starts after the 60 s selftest startup. Not used as evidence; `task47_ab2` passes `--max-seconds 150` after `--` |
| Default-mode run, 120 s, fresh save copy (`task47_on2.log`) | `decomp mode=on registered=12056`, exit code 4, 99 IPC commands, 0 unknown, the same single interpreter miss 0x71012a0ed4 as task-46, frame 27,631. A first default run (`task47_on.log`) ended at frame 925 with 97 IPC and no miss: slow machine run (same effect seen in task-46), repeated and not accepted |
| Locked project check | PASS (1795762401869922800-c3e27a77) |

Live generated rows in `task47_ab2` (one run, 150 s of guest time):

| Pattern | Rows | Live | Selftest only | Live calls |
|---|---|---|---|---|
| empty | 3,629 | 351 | 3,278 | 24,689,474 |
| const | 1,988 | 144 | 1,844 | 2,272,251 |
| get | 4,892 | 248 | 4,644 | 11,657,946 |
| get_nullsafe | 20 | 1 | 19 | 141,040 |
| set | 934 | 53 | 881 | 174,025 |
| set_bool | 438 | 6 | 432 | 7,330 |
| add_imm | 94 | 16 | 78 | 4,906,868 |
| mov_reg | 33 | 0 | 33 | 0 |
| generated total | 12,028 | 819 | 11,209 | 43,848,934 |
| hand-written | 28 | 18 | 10 | 10,433,354 |

Residual risks: main_0169 (4 functions) not built; `-g0` removes DWARF for wrapped chunks (the lifted objects never had it, so debugging them is no worse than before); the ab live counts come from one 150 s run, not three save sets as in task-46.

## Reproduction

Generate: `python -I tools/decompgen/generate.py --out A:\AI\Recomps\smrpg-work\decompgen --limit 100000 --max-chunks 0 --exclude-chunks 169`, then `python -I tools/rtprep/build_game.py --jobs 16`. The previous 120-chunk set is kept in `decompgen_t46\`.

## Log

- 2026-11-27. Diagnosed the DWARF cause, added `-g0`, rebuilt 120 chunks (208 s) then all 350 (322 s), A/B and default runs clean.

### 2026-11-27 Claude lead
Lead checked the one-line fix (game.cmake, -g0 on wrapped chunks only) and reread task47_ab2 and task47_on2: 0 mismatch lines, 0 unknown IPC. Shared exe is 314,381,312 bytes with 12,056 rows. game_check PASS (1795762401869922800-c3e27a77). Open: the slow first default-mode run (frame 925) happened twice now (task-46, task-47); cause not found.
