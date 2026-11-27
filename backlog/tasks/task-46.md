---
id: task-46
title: Rust decomp generator, scale to every matching function
status: Review
---

## Observed behavior

Task-45 generated 300 functions over 12 chunks and skipped the boolean setter (`and w8, w1, #1; strb w8, [x0, #imm]; ret`). The binary holds 12,037 matches.

## Outcome and scope

`tools/decompgen/` now generates every match: 12,032 functions in 351 lifted chunks (`--limit 100000 --max-chunks 0`; the other 5 were hand-written in tasks 29 to 44). A new pattern `set_bool` adds 438 matches. Four small flags and one build fix make big sets practical: `--chunks LO:HI`, `--exclude-chunks N,N`, and `runtime/decomp/wrap_chunk.py` no longer rewrites an unchanged wrapped chunk (an appended manifest row for another chunk used to touch every wrapped file and recompile all of them). The selftest in `swap.c` saves and restores the other rows' counters only for a caller row (it was 4 loops over every row for every row, which grows with the square of the row count) and the arrays are static.

Boolean setter finding: the first version wrote x8 through a register-file row. The whole-context selftest failed at once (`decomp mismatch addr=0x710015d090 regs=1 memory=0`): the lifted reference does not write x8 back. At `ret` the lifter flushes only x0..x7 and x19..x29 (`MASK_RET`, `tools/lifter/lift.py`), so x8 is dead scratch under the same ABI assumption as every other lifted function. The exact port is a plain store of the low bit with the ordinary `SLEAF` store shadow, whole context compared, no new row kind (the BLEAF row and its `swap.c` code were removed again). 438 matches, 63 in the built set.

What is built: not all 12,032. Build times (`build_game.py --jobs 16`, BelowNormal, one process at a time, A: drive):

| Stage | Chunks wrapped | Time |
|---|---|---|
| 0:30 (133 functions) | 15 | 321 s at -j 8 (about 60 s of compile, 255 s of link) |
| 0:200 (4,049 functions) | 185 | killed at 641 s: main_0169 (7.3 MB of C) was still compiling after 8 minutes. Excluded (4 functions) |
| 0:200 without main_0169 | 185 | 792 s, over the limit: link alone 9 min, `smrpg.exe` 1.13 GB |
| best 120 of those chunks (3,778 functions), rewrap and relink | 120 | 495 s |

Each wrapped chunk object adds about 4.5 MB to `smrpg.exe` (437 MB with 15, 1.13 GB with 185) and the link is the cost that cannot be split into stages; 351 chunks cannot link inside the 10 minute limit. So the built and tested set is 3,778 generated functions in the 120 best chunks (functions plus call sites) of chunks 0 to 199; the other 8,254 generated functions are not linked. All 12,032 are generated and pass `cargo check --release --locked --offline` and `rustfmt --check` (`decompgen_all\`, not the build set). Next lever is the link (split link, less debug info), not the generator.

Selftest: every one of the 3,806 registered rows (28 hand-written, 3,778 generated) runs 10,000 random plus 15,625 edge cases against the lifted C. It costs about 4 ms per row: 15.3 s for 3,806 rows (all 12,032 would be about 50 s). It already runs only with `SMRPG_DECOMP=ab` and `SMRPG_DECOMP_SELFTEST=1`, never in the default mode, so nothing moved. Default (on) mode startup is 0.35 s with all rows registered.

## Dependencies and ownership

Task-45. Owns `tools/decompgen/`, `decomp/smrpg_decomp`, `runtime/decomp/`, `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Matcher unit tests (`python -I tools/decompgen/test_decompgen.py`) | PASS (9 tests; the new one pins the boolean setter: accepted words, the exact Rust body, and 10 near misses refused: wrong store register, wider mask, 64-bit and flag-setting `and`, destination x0, halfword store, write-back, no `ret`, source not an argument) |
| Rust (`cargo test --release --locked --offline`, `cargo fmt --check`, `cargo check`) | PASS: 5 tests (new: `store_low_bit_matches_reference`), no warning in `cargo check`. One `unused_assignments` warning in the test profile comes from the task-44 caller test, not from this change |
| Boolean setter in the live selftest against the lifted C | PASS: 63 rows, 0 differences in 3 runs (and the wrong x8 write was caught, see above) |
| Hidden muted A/B runs with selftest, fresh copies in `task46_rt\` | 3 runs, 0 `decomp mismatch` lines each, `decomp mode=ab registered=3806`, exit code 4 (watchdog stop), 99 distinct IPC commands, 0 unknown |
| Default-mode run, 120 s, task-42 save copy, no `SMRPG_DECOMP` | `decomp mode=on registered=3806`, exit code 4, 99 distinct IPC commands, 0 unknown, the same single interpreter miss 0x71012a0ed4 as the ab runs and task-45, frame 15,514 at 111 s. A first attempt (`task46_on`) ended with 97 distinct commands and frame 1,285: the run was far slower for an unknown reason (nothing of mine was running) and was repeated, not accepted as the comparison |
| Locked project check | PASS (1795760672683661800-4186c471) |

Runs (logs in `A:\AI\Recomps\smrpg-work\build\logs\`; saves, options and `input.txt` copied from the sources into `A:\AI\Recomps\smrpg-work\task46_rt\`; `bh`, `br` options as in task-44; captures not modified):

| Log | Source | Seconds | Live generated | Live calls (generated) |
|---|---|---|---|---|
| `task46_ab1.log` | task-42 save, task-45 options and input (`t42`) | 260 (selftest 15.3 s) | 457 | 95,218,761 |
| `task46_ab2.log` | `codex_battle_hard_02` (`bh`) | 215 | 464 | 38,991,203 |
| `task46_ab3.log` | `codex_bandits_route_22` (`br`) | 215 | 457 | 24,933,080 |
| `task46_on.log`, `task46_on2.log` | task-42 save, default mode | 120 | not counted in default mode | |

Per pattern: matches in the binary, generated (all), built and tested (rows in the exe), live in the union of the three runs, live calls (sum of the three), differences.

| Pattern | Matches | Generated (all) | Built and tested | Live (union) | Live calls (3 runs) | Differences |
|---|---|---|---|---|---|---|
| empty | 3,629 | 3,629 | 1,388 | 183 | 21,342,175 | 0 |
| const | 1,988 | 1,988 | 1,073 | 104 | 5,206,047 | 0 |
| get | 4,892 | 4,892 | 875 | 133 | 109,738,138 | 0 |
| get_nullsafe | 21 | 20 | 10 | 0 | 0 | not claimed (selftest only) |
| set | 940 | 938 | 291 | 26 | 638,407 | 0 |
| set_bool | 438 | 438 | 63 | 3 | 40,248 | 0 |
| add_imm | 96 | 94 | 73 | 16 | 22,178,029 | 0 |
| mov_reg | 33 | 33 | 5 | 0 | 0 | not claimed (selftest only) |
| total | 12,037 (5 hand-written) | 12,032 | 3,778 | 465 | 159,143,044 | 0 |

The generator skips a match when its lifted chunk file has not exactly one definition of the function (3 patterns lose 1 or 2: 1 get_nullsafe, 2 set, 2 add_imm). Live is the union of addresses with at least one call in any of the three runs; selftest-only is the rest. `check_log.py --summary` over each log: PASS, "selftest clean for 3806 rows; live differences: 0". Hand-written rows: 21, 20 and 20 live in the three runs.

Residual risks: the boolean setter is exact against the lifted C, not against the original ARM64 in x8 (dead under the lifter's ABI assumption, see above); 8,254 generated functions are unlinked and untested at run time (their Rust type-checks and the selftest machinery is the same); the same invalid-pointer behavior as task-45 (fault in Rust instead of C, same host handler).

## Reproduction

See [the decomp guide](../../docs/DECOMP.md), section "Bulk generator". Built set: `python -I tools/decompgen/generate.py --limit 100000 --max-chunks 120 --chunks 0:200 --exclude-chunks 169`, then `python -I tools/rtprep/build_game.py --jobs 16`; runs as `python -I tools/rtprep/run_game.py --seconds 215 --log NAME --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SMRPG_SAVE=... --env SMRPG_OPTIONS=... --env SMRPG_INPUT_FILE=... --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --continue`.

## Log

- 2026-11-27. Generated all matches, added set_bool, staged builds (killed main_0169 after 8 min, one 792 s build over the limit before the set was cut to 120 chunks), found the x8 difference in the first selftest run. Runs `task46_ab1` to `task46_ab3` and `task46_on2`: 0 mismatches. Not committed.

### 2026-11-27 Claude lead
Lead reread the four logs: task46_ab1 to ab3 and task46_on2 have 0 mismatch lines and 0 unknown IPC. Matcher tests rerun OK. Kept at 3,778 built functions: no link over 10 minutes (owner rule). Two worker builds ran past 10 minutes (641 s killed, 792 s); that breaks the rule and is recorded here. Open: the exe grows about 4.5 MB per wrapped chunk (now 948 MB), so task-47 fixes the size before more chunks are linked.
