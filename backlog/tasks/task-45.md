---
id: task-45
title: Rust decomp generator, exact trivial-function patterns in bulk
status: Review
---

## Observed behavior

Tasks 40 to 44 ported 28 functions by hand, five per batch. IL2CPP emits thousands of functions of identical shape (field getters, setters without write barriers, constant returns, empty bodies), so hand picking does not scale.

## Outcome and scope

`tools/decompgen/` (Python, stdlib only) reads the decoded ARM64 of every main-module function with the lifter's own inventory and decoder, matches seven exact patterns (`patterns.py`, one Rust template each) and writes Rust, manifest rows and an index for the top matches by direct call sites. A function that is not the exact shape is skipped. First batch: 300 functions, capped to 12 lifted chunks (`--limit`, `--max-chunks` raise it). Functions already in `leaves.def` are excluded. Names come from the IL2CPP method map where one exists (190 of 300), otherwise the symbol carries the pattern and address.

Matches over the whole binary (functions of at most 5 instructions, 157,221 functions scanned):

| Pattern | Shape | Matches in binary | With direct call sites | Generated | Live in A/B (of generated) | Live calls | Differences |
|---|---|---|---|---|---|---|---|
| empty | `ret` | 3,629 | 465 | 127 | 36 | 27,702,274 | 0 |
| const | movz, movn, movk, orr from zero, then `ret` | 1,988 | 296 | 44 | 6 | 124,250 | 0 |
| get | `ldr` family into x0 from `[xN, #imm]` | 4,892 | 467 | 98 | 12 | 1,112,845 | 0 |
| get_nullsafe | `cbz x0, +8; ldr x0, [x0, #imm]; ret` | 21 | 20 | 1 | 0 | 0 | not claimed (selftest only) |
| set | `str`, `strb`, `strh` to `[xN, #imm]` | 940 | 184 | 23 | 2 | 132,261 | 0 |
| add_imm | `add` or `sub` x0, xN, `#imm` | 96 | 40 | 7 | 0 | 0 | not claimed (selftest only) |
| mov_reg | `mov x0, x1` or `x2` | 33 | 1 | 0 | 0 | 0 | not generated (rank) |
| total | | 11,599 | 1,473 | 300 | 56 | 29,071,630 | 0 |

Five more matches were already hand-written (counted in 11,599, not generated). The 300 generated functions have 12,718 direct call sites. Per-variant counts (u8, u16, u32, u64, s8w, s32x, zero, wide, and so on) are in `gen.report.txt`.

Skipped on purpose and counted nowhere above: float getters and setters (`ldr s0`, vector registers), write-back and register-offset addressing, flag-setting adds, and the common boolean setter `and w8, w1, #1; strb w8, [x0, #imm]` (it also writes x8, so it needs a register-file row with a store shadow).

Where the generated code lives: `A:\AI\Recomps\smrpg-work\decompgen\` (`gen.rs`, `gen.def`, `gen.index.tsv`, `gen.report.txt`), not in Git. It holds game addresses, field offsets and method names, i.e. extracted game data in the sense of the "nothing from the game enters Git" rule, and it grows with the flags. The generator, templates, tests, `decomp/smrpg_decomp/build.rs` and the CMake hooks are in Git. `build.rs` includes `gen.rs` through `SMRPG_DECOMPGEN_DIR` (default the A: directory); without the file the crate builds with the 28 hand-written functions only. `game.cmake` generates once if the files are missing and concatenates `leaves.def` plus `gen.def` into `manifest.def` in the build directory, rewritten only when the text changes.

Runtime side: new row kinds `GLEAF` and `NLEAF` (loads; the selftest points `x[base] + offset` into a private fake region, NLEAF also passes null one time in eight). Generated stores reuse `SLEAF`; the store selftest now places the base register so that `x[base] + offset` lands in the fake region, which lets offsets up to 64 KB work. `check_log.py --summary` keeps the selftest and mismatch checks for every row and reports live against selftest-only counts instead of failing on 0 calls.

## Dependencies and ownership

Task-29, tasks 40 to 44. Owns `tools/decompgen/`, `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Matcher unit tests (`python -I tools/decompgen/test_decompgen.py`) | PASS (8 tests: accepted words, near misses refused, extension forms, constant folding, manifest row format) |
| Rust type-check, fmt and unit tests | PASS (`cargo check --release --locked --offline` no warnings with and without `gen.rs`, `cargo fmt --check`, `cargo test --release --locked --offline` 4 tests). `rustfmt --check` on the generated `gen.rs` also clean |
| Selftest per function, 10,000 random plus 15,625 edge, fake region | PASS (all 328 rows, 0 differences). All 300 generated selftests took 1.4 s (0.363 s to 1.792 s after start) |
| Build time added | First build with 12 rewrapped chunks, cargo and the C compile: 2 min 29 s wall (it ended on a swap.c macro error, fixed afterwards); the rebuild of swap.c and the link: 4.9 s. Total about 2.6 min over a build with no swapped chunks |
| Hidden muted 200 s A/B run `task45_ab` (fresh copy of the task-42 save, task-24 options and input, in `task45_rt\`) | 0 `decomp mismatch` lines, `decomp mode=ab registered=328`, exit code 4 (watchdog stop). Log `A:\AI\Recomps\smrpg-work\build\logs\task45_ab.log` |
| `python -I runtime/decomp/check_log.py --summary` | PASS, output below |
| Locked project check | PASS (1795756404531758400-c892f949) |

```
group               rows  live  selftest-only   live calls
hand-written          28    19              9     43807604
generated            300    56            244     29071630
  add_imm              7     0              7            0
  const               44     6             38       124250
  empty              127    36             91     27702274
  get                 98    12             86      1112845
  get_nullsafe         1     0              1            0
  set                 23     2             21       132261
selftest clean for 328 rows; live differences: 0
```

Not live in this run (selftest only): 244 generated functions, including all add_imm and the one null-safe getter. Live counts come from one 200 s run on the opening save; battle and menu code paths were not exercised, so a second route (as in task-44) would raise coverage. The strict `check_log.py` mode still fails on any function with 0 live calls, as before.

Residual risks: a getter or setter on an invalid pointer faults inside Rust where the lifted body faulted inside C; both reach the same host crash handler (`rt_veh`), which logs and exits for either. The 16-bit and 8-bit access helpers (`guest_ld16`, `guest_ld8`, `guest_st16`) are relaxed atomics like `ir_ld` and `ir_st`.

## Reproduction

See [the decomp guide](../../docs/DECOMP.md), section "Bulk generator".

## Log

- 2026-11-27. Survey of the 157,221 main-module functions of at most 5 instructions by shape, then the seven patterns above. The first full build failed in `swap.c` because two of four macro blocks were not updated for the new row kinds; fixed and rebuilt (no change to any comparison). Run `task45_ab`: 0 mismatches. Not committed.

### 2026-11-27 Claude lead
Lead checked: generated Rust and rows stay on A: (decompgen\), only the generator, templates and tests are in Git, and they hold no game data beyond one test address already public in task-43. Matcher tests rerun OK. task45_ab.log has 0 mismatch lines; exit 4 and the one interpreter miss (0x71012a0ed4) match the task-44 runs, so they are not new. game_check PASS (1795756404531758400-c892f949).
