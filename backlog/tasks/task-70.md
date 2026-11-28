---
id: task-70
title: exec_koudou_kouka split step 1 (prologue, lazy init, first loop, deferring pre-state hook)
status: Review
depends: task-67, task-69
---

## Goal

Step 1 of the split plan in docs/DECOMP.md ("Plan: splitting exec_koudou_kouka"): an exact Rust port of `exec_koudou_kouka` (0x71021f6340) from the prologue through the first loop (labels up to 0x71021f67d0), wired as a QLEAF-style row whose pre-state hook defers (body run lifted, counted as deferred) whenever the path would leave the ported part. Arms of table 1 and later code stay lifted.

## Acceptance

- Port in `decomp/src/battle/` with unit tests; `cargo check`, `cargo check --release`, `cargo fmt --check`, `cargo test` pass.
- Startup selftest for the row: 0 differences.
- At least two mutations caught at the row, reverted, rebuilt.
- Three `ab` runs of route `bwf` with `SMRPG_DETERMINISTIC=1`: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none; live and deferred counts for the new row reported with live_rows.py, identical across runs.
- `python tools/game_check.py run --root .` PASS. DECOMP.md note.

## Design

An exact Rust port cannot hand control to the middle of a lifted body, so "port the head, run the rest lifted" is not expressible. Smallest exact design: the port covers the head plus the early exit to the epilogue (types that do not take table 1, `type - 5` above 0x5a), and the hook defers every call that would enter table 1, plus a clear lazy-initialization flag, null links, zero literal cells and a type past the list length (the lifted thrower never returns in the selftest). In `on` mode the port runs the whole lifted body (`run_lifted`) for a table type before touching anything. Ported part: 0x71021f6340 to 0x71021f67d0 plus the epilogue 0x71021f7610 and 0x71021f7624, `decomp/smrpg_decomp/src/battle/exec_koudou_head.rs` (768 lines with 13 unit tests in `exec_koudou_head/tests.rs`). `bd6c0` and `use_damage_guard` run through their shims inside the port; the row is a nested row (`is_nested_row`). Post-call register reloads were read per call site from the raw lifted C (x2 and x8 after `bd6c0`, x8 after `get_value` and `is_barriered`, x8 and x9 after `use_damage_guard`). `bridge::safepoint_n` takes up to 12 registers.

## Evidence

| Check | Result |
|---|---|
| `cargo check`, `cargo check --release`, `cargo fmt --check`, `cargo test` (114 tests) | pass |
| Startup selftest (`t70_self7`) | `addr=0x71021f6340 random=10000 edge=15625 calls=25625 differences=0` |
| Mutation: first `bd6c0` flag code 5 to 6 (`t70_mut3`) | caught: `decomp mismatch ... call_mismatch=1` |
| Mutation: logger +0x47 store 1 to 2 (`t70_mut4`) | caught: store journal mismatch |
| Mutation: close_loop +0x60 store, +1 (`t70_mut1`) | Not observable, by construction. `close_loop` stores `x19` to logger +0x60 and the next stores are the epilogue's identical `x19` store to the same cell, with no callee, read or exception between. The store journal skips a store that a later store to the same cell supersedes (task-56 rule), and memory ends equal. The path is reached (x19 nonzero is common because the stand-in `7101e30240` returns arbitrary values), and the final value is compared: mutating the epilogue store (+1, `t70_mut5`) is caught as `store journal ... port stored 0x..bc, memory has 0x..bb`. |
| Mutation: `get_value` return address (`t70_mut2`) | Not observable. 0x71018876b0 is not a traced row; the port runs the lifted function live. It is two loads (`x8 = [x0+0x130]; w0 = [x8+0x30]`) and a return: it never reads or writes x30 (the raw C only copies `x30 = c->x[30]`) and makes no call, so no unwind or return-address walk can see x30 there. The next call in the port sets x30 again before any use. Where a callee is a traced row x30 is compared: changing the singleton call's return address (`t70_mut6`) is caught as `call 4 to 0x7101892aa0, x30: lifted=0x71021f66dc rust=0x71021f66e0`. |
| Three final `ab` runs of `bwf` (`t70_fin_ab1..3`, plus `t70_fin_ab4`) | 0 `decomp mismatch`, raced 0, 99 IPC commands 0 unknown, interpreter misses none |
| Row counts (live/deferred), accepted as a task-69 limit | ab1 12/0+2d, ab2 16/0+1d, ab3 12/0+2d, ab4 16/0+2d, rerun after the review changes (`t70_fin_r2`) 12/0+2d. The route is not fully repeatable (task-69 limit): in ab2 and ab4 it diverged (`exec_each_targets` 16 versus 12, `damage_calc` 15 versus 10). The row's call count equals `exec_each_targets` in every run, so the variance is upstream of this row. |
| Rerun after review (`t70_self_r2`, `t70_fin_r2`) | selftest `calls=25625 differences=0`; `ab` run of `bwf`: 0 `decomp mismatch`, raced 0, 99 IPC 0 unknown, interpreter misses none |
| `python tools/game_check.py run --root .` | see the result line below |

Selftest graph note: it generates only types the port compares (0 to 4, 0x60 to 0x6f) and lists of 0x80 or more, because a deferred call runs the lifted body on the stand-in graph, which has no objects for table types (the first selftest crashed there).

## Lead verification

- Logs checked: t70_fin_ab1 to ab3 and t70_fin_r2 each 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none. t70_self_r2: row 0x71021f6340 `calls=25625 differences=0`, no selftest line with differences.
- Uncaught mutations accepted as unobservable: the +0x60 `close_loop` store is superseded by the epilogue store to the same cell with nothing in between (the epilogue mutation, t70_mut5, is caught), and 0x71018876b0 never reads x30 (a traced callee's x30 mutation, t70_mut6, is caught).
- Count variance across runs (12 or 16 calls) is upstream of the row and recorded as a task-69 limit. Diff touches only task-70 files; no em dashes or personal details. game_check rerun by the lead before commit.
