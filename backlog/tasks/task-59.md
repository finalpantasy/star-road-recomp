---
id: task-59
title: group_a_b ported and compared; the damage_calc driver not ported
status: Review
---

## Observed behavior

Task-58 left `group_a_b` (0x7102201aa0, 390 instructions) and the `damage_calc` driver (0x71021ff580, 613 instructions) lifted. This task ports `group_a_b` with a register-exact comparison and stops there: the driver is not started (what it needs is in `docs/DECOMP.md`, section "damage_calc (task-59)").

## What was built

- `decomp/smrpg_decomp/src/battle/damage.rs`: `SkillExecter.group_a_b` as readable Rust with IL2CPP names (`_skill_type == RENKEI`, `is_fear`, `is_defense_up`, `is_attack_up`, `_is_guard`, `BattleManager._instance._pc_list`), `Frame` register tracking, floats through `fp.rs`, the `foreach` as `open_character_list` / `count_and_scale` / `scale_by_count`, shim `smrpg_skill_group_a_b` and pre-state hook `smrpg_skill_group_a_b_pre`. Two unit tests (`halve`, `three_halves`, the metadata return addresses).
- `leaves.def`: `QLEAF` 0x7102201aa0 (was `PLEAF`), `PLEAF` 0x71021be020 and 0x7100183d20, `OLEAF` 0x7101b52040 (x1, 24 bytes, out), 0x71018fe040 (x0, 24 bytes, in and out), 0x7101d6a050 (x0, 4 bytes, in). `is_defense_up`, `is_attack_up` and the enumerator `Dispose` were already generated rows.
- `swap.c`: `OUT_MAX` 16 to 24, selftest graph for 0x7102201aa0 (`prepare_caller`), stand-in `get_Current` (`tl_stub_current`), `restore_group_globals`.
- `bridge.rs`: the `invoke_out` assert 16 to 24 (it was a panic: the first selftest aborted with exit code 0xC0000409 and no log line). `frame.rs`: `call_out`, `call_v`, `call_out_v` write x30 themselves (the first selftest showed `x30` wrong at every call whose list did not contain 30).
- `docs/DECOMP.md`: section "damage_calc (task-59)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean; 46 tests passed (44 before, 2 new) |
| `check_pure.py` | all PASS |
| `tools/rtprep/build_game.py` | first build with the new rows about 7 min 46 s (six chunks and `swap.c`); `swap.c` only 3 min 23 s; Rust only 23 to 26 s |
| Selftest `t59_final_self` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`, battle route copy) | `group_a_b` 25,625 calls, 0 differences; 0 `decomp mismatch`; raced 0; IPC 99 distinct, 0 unknown; interpreter misses none; 13,131 presents; `damage_calc` 14 and `group_a_b` 14 live calls (1 deferred), 0 differences |
| A/B battle route `t59_final_bh` (copy of task58's `bh` in `task59_rt`) | 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none, 11,703 presents, `group_a_b` 15 live (1 deferred), 0 differences |
| Same before the mutations: `t59_bh` | 0 mismatch, 10,244 presents, `group_a_b` 15 |
| A/B task42 save `t59_t42` (copy in `task59_rt`) | 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none, 20,675 presents, `group_a_b` 12 live |
| `on` mode battle `t59_on` | 0 mismatch, IPC 99 / 0 unknown, misses none, 12,134 presents, `decomp on addr=0x7102201aa0 calls=15`: the route reached the battle |
| Mutation 1 (`three_halves` shifts by 2 instead of 1) | 13,440 `decomp mismatch` lines in a selftest run, exit 10 |
| Mutation 2 (`COUNT_B` divisor -3f to -2f) | 570 mismatch lines, first one on the `Single.ToString` call (v0, fpcr/fpsr, the bytes passed by reference) |
| `python tools/game_check.py run --root .` | PASS |

Both mutations were reverted (`cmp` with the saved good copy identical, no MUTATION marker; the final build and the `t59_final_*` runs came after).

## Limits of the evidence

- The route's 14 to 15 live calls all take the non-renkei branch (`is_fear` 28 live calls, the `foreach` never runs live in a compared call). The renkei branch, both `foreach` copies, the lazy class and flag initialization paths and the float scaling are covered by the 25,625 selftest cases only (and by mutation 2 there), not by a real battle.
- A clear lazy-initialization flag byte defers the call (1 deferred per run, not compared), as for `calc_renkei_ratio`.
- The `foreach` rows are hot: `GetEnumerator` and `MoveNext` are shared generic functions (589,727 and 813,881 calls in one 258 s run). They are traced rows now (an atomic add per call in `ab` mode); presents stayed at 10,000 to 20,000 per run, as before.
- Exception landing pads of the enumerator `finally` are not ported (a callee that throws makes the port return at once).

## Not done

1. The `damage_calc` driver (613 instructions, `damage.rs`). Needs: the 29 first-call metadata calls, 19 debug writes, 8 class checks with polls, the formula, footprints belome x2 (4 bytes, in), renkei and `group_a_b` x1 (= `x29 - 4`, 4 bytes, in), `Single.ToString` x0 (= `sp + 8`, 4 bytes, in). `group_a_b` is now a replay row, so the driver port calls its Rust body directly; see the end of the task-59 section in `docs/DECOMP.md`.
2. A live (non-selftest) check of the renkei branch of `group_a_b`.

## Dependencies and ownership

Task-44, 51 to 58. Owns `decomp/smrpg_decomp`, `runtime/decomp/`, `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.
- Discarded runs: `t59_self` (host crash: the selftest graph had no valid buff and status managers), `t59_self2` (x30 mismatches), `t59_self3` (panic abort, `invoke_out` assert), the mutation runs (expected failures).

## Lead verification (2026-11-27)
- Reread t59_final_bh and t59_final_self (0 `decomp mismatch`, raced 0, 99 IPC with 0 unknown, misses none) and the selftest mutation logs (13,440 and 570 mismatch lines).
- Partial: the damage_calc driver is not started; next task.
- Diff scanned for em dashes and personal details: none.
