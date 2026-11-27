---
id: task-58
title: calc_renkei_ratio ported and compared; group_a_b and the damage_calc driver not ported; build rebuild cause found and narrowed
status: Review
---

## Observed behavior

Task-57 left three pieces for `damage_calc` (0x71021ff580): `calc_renkei_ratio`, `group_a_b` and the 613 instruction driver, and a first build of 15 minutes. This task ports one of the three (the cleanest, 284 instructions, float math) with a register-exact comparison, finds why builds were slow and removes one cause. It does not finish the driver.

## What was built

- `decomp/smrpg_decomp/src/battle/renkei.rs`: `SkillExecter.calc_renkei_ratio` 0x71021f5ed0 (`QLEAF`), IL2CPP names and offsets, float math through `fp.rs` (`f32_arith`, new `f32_div`, `f32_from_i32`, `i32_from_f32_trunc`, `f32_compare`), 3 unit tests.
- `frame.rs`: vector locals (`v`, `set_s`, `call_v`, `call_out_v`, `leave_v8`), x30 written at every call, `leave` restores x19 to x29. `bridge.rs`: `Regs::v_high`, `f32_div`, `safepoint_nv`, `prestate::store_u64`.
- `leaves.def`: 0x71021f5ed0 is QLEAF; new rows `OLEAF 0x710213a840` (Int32.ToString, x0, 4 bytes, in), `OLEAF 0x7101b4fcc0` (List<int> ctor, x0 + 0x10, 16 bytes, out), `PLEAF` 0x71020147d0, 0x7102023cb0, 0x71000c44d0, 0x71000c4090.
- `swap.c`: `smrpg_guest_safepoint_nv`, selftest graph for 0x71021f5ed0 (stand-in `List<int>` from `Object.new`, 13 metadata slots saved and restored), caller stack 16 to 64 words (the old one overflowed into `leaves[]` for a 0x70 byte frame), "callee differs" now names the callee, `on` mode prints `decomp on addr=... calls=...` for replay rows.
- `game.cmake`: `list(SORT chunks)` (build time fix, see below).
- `docs/DECOMP.md`: section "damage_calc (task-58)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean; 44 tests passed (41 before, 3 new) |
| `check_pure.py` | all PASS |
| `tools/rtprep/build_game.py` | see "Build times": first build with the sort fix 11 min 27 s (over the 10 minute limit, one time); later builds 28 s to 577 s |
| Selftest `t58f_self` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`) | renkei 25,625 calls, 0 differences; fp shadow 382,981 checked, 0 differences; 0 `decomp mismatch`; raced 0; IPC 99 distinct, 0 unknown; interpreter misses none; 12,090 presents |
| A/B battle route `t58f_bh` (own save, copy of task57's `bh`) | 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none, 10,504 presents; live: `damage_calc` 14, `calc_renkei_ratio` 14 (1 deferred), `calc_belome_barrior_ratio` 14 (1 deferred), `group_a_b` 14, Random.Range 281, List<int>.Add 134,818 traced, all 0 differences |
| A/B task42 save `t58f_t42` | 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none, 13,917 presents, new rows 0 calls |
| `on` mode battle `t58f_on` | 0 mismatch, IPC 99 / 0 unknown, misses none, 6,985 presents; `decomp on addr=0x71021f5ed0 calls=6`, belome 6 (an earlier `on` run, `t58_on`: 15 and 15, 13,718 presents): the route reached the battle |
| Mutation 1 (`HIT_BASE` 0x3f666666 to 0x3f666667, 0.9f) | 1,715 `decomp mismatch` lines in a selftest run; x28 at the first `Random.Range` call differs |
| Mutation 2 (the 0x2ba case shifts by 1 instead of 2) | 779 mismatch lines |
| `python tools/game_check.py run --root .` | PASS |

Both mutations were reverted (`cmp` with the saved good copy identical, no MUTATION marker, the final build and runs `t58f_*` came after). A third mutation (`< 0` to `<= 0` in the same case) was an equivalent mutant (0 and 3 shift to the same value): 0 mismatches, discarded.

## Build times and what forces a full rebuild

| Build | Time | Why |
|---|---|---|
| no source change, `prelude.h` and `ops_gen.h` touched at 12:41 by another lifter run | 7 min 57 s | every chunk includes them (`tools/lifter` rewrites them unconditionally) |
| first row set, order bug (killed by me) | over 11 min | new hand-written rows moved chunks in `flags.make` |
| first build with `list(SORT chunks)` | 11 min 27 s | `flags.make` changed once more (order) |
| `swap.c` only | 1 to 2 min | one object plus link |
| Rust only | 28 s | cargo plus link |
| one row in one existing chunk (main_0262) and back | 577 s and 225 s | one 2.3 MB lifted C file at -O2 under 60 percent CPU load from other programs |

Root cause of task-57's 15 minutes: CMake writes a `Custom options` line per wrapped chunk into `flags.make` in the order of the chunk list, every object depends on `flags.make`, and the list is ordered by first appearance (hand-written rows first). A first hand-written row for a chunk that only had generated rows reordered the list. `wrap_chunk.py` was not the cause (it does not rewrite unchanged output). Fixed by sorting the list. Remaining causes, not fixed: `tools/lifter` rewriting `prelude.h` and `ops_gen.h` without a content check (outside this task's files), and the cost of one big chunk.

## Deviations and discarded runs

- Not done: `group_a_b` and the `damage_calc` driver. Mutations are in `calc_renkei_ratio`, not in the driver.
- Live coverage of renkei is on the paths the route takes (14 calls; which skill ids they carry is not logged, the combo ids 0x2ba to 0x2d2 are covered by the selftest only). The selftest hits every case and the loop.
- Discarded: the first selftest attempt (stack overflow, exit 10), the mutation runs (expected failures), and an earlier A/B run `t58_bh1` (GPU was 97 percent busy from another program before it; the final runs started at 20 percent).
- Presents vary between runs with the machine load (6,985 to 13,917); only the battle live counts decide whether the route reached the battle.

## Not done

1. `group_a_b` 0x7102201aa0 (390 instructions): callees, footprints and the 24 byte enumerator struct are listed in `docs/DECOMP.md`.
2. The `damage_calc` driver (613 instructions): needs 1 and the footprints listed there (belome x2, renkei and `group_a_b` x1 = `x29 - 4`, `Single.ToString` x0 = `sp + 8`).
3. `prelude.h` and `ops_gen.h` write-if-changed in `tools/lifter` (owner of that code to decide).

## Dependencies and ownership

Task-44, 51 to 57. Owns `decomp/smrpg_decomp`, `runtime/decomp/`, `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.

## Lead verification (2026-11-27)
- Reread t58f_bh and t58f_t42 (0 `decomp mismatch`, raced 0, 99 IPC with 0 unknown, misses none) and t58f_on (on mode, 99 IPC, 0 unknown, misses none).
- Partial: group_a_b and the damage_calc driver stay lifted. Lifter header rewrites fixed by the lead in a separate commit.
- Diff scanned for em dashes and personal details: none.
