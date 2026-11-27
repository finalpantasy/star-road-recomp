---
id: task-56
title: damage_calc callees ported (action command ratios, barrier test, damage guard); the driver not ported
status: Review
---

## Observed behavior

Task-55 traced the callees of `SkillExecter.damage_calc` (0x71021ff580, 613 instructions) and left the driver and four callees. This task was to port the driver. It did not finish it: the driver needs the callees' out-parameter handling, a recorded-call limit above 12 and a ten-class selftest graph (sizes in `docs/DECOMP.md`, "damage_calc (task-56)"). A coherent compared part is done instead and the rest is listed below.

## What was built

- `decomp/smrpg_decomp/src/battle/damage.rs`: `SkillExecter.set_acction_command_ratio` 0x71021f7ea0 (`QLEAF`: reads the acter and skill id, stores five result fields through the store journal), `BattleCharacterParam.use_damage_guard` 0x71021be280 (`QLEAF`, one flag byte) and `is_barriered` 0x71021c3bb0 (`LEAF`), IL2CPP names and offsets, the 129 entry jump table written as `CommandRule::of`. Unit tests (`damage/tests.rs`): every skill id from -3 to 139 plus the edges against a reference written from the table indexes, the barrier count and the guard store.
- `bridge.rs`: `prestate::store_u32`.
- `swap.c`: `check_store_journal` now requires a journaled store to match memory only when it is the last store to that cell (a cell the port stores twice, default then real value, was a false difference); selftest graphs for the three rows.
- `leaves.def`: the three PLEAF rows became ported rows. `docs/DECOMP.md`: the store-through-pointer and out-parameter mechanism, the driver breakdown and design of what remains.

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean; 37 tests (2 new); the one warning (`lib.rs:504`) is old |
| `python -I runtime/decomp/check_pure.py` | 0 failures |
| `python -I tools/rtprep/build_game.py` | build 1 (3 new rows, no new chunks) 410 s; mutation and final rebuilds 27 s to about 2 min |
| Selftest, `task56_self` (`--seconds 340 -- --max-seconds 150`) on a copy of the battle route in `task56_rt\bh` | the three new rows 25,625 calls each, 0 differences; 0 `decomp mismatch`, raced 0, IPC 99 distinct 0 unknown, misses none, 13,413 presents; `check_log.py --summary`: "selftest clean for 13672 rows; live differences: 0; raced loads (retry agreed): 0" |
| A/B, battle route, `task56_bh3` and the post-revert `task56_final` | 0 mismatch, raced 0, IPC 0 unknown, misses none, 10,506 and 10,346 presents |
| A/B, copy of the `task42_rt` save, `task56_t42` | 0 mismatch, raced 0, IPC 0 unknown, misses none, 26,912 presents (field route: the new rows 0 calls) |
| `SMRPG_DECOMP=on`, battle route, `task56_on2` | no crash, 0 mismatch lines, IPC 0 unknown, misses none, 10,881 presents; battle reached: `nvnCommandBufferCopyTextureToTexture` x611 and `nvnTextureGetDepth` x610, identical to the A/B battle run (`task56_on`, the first try, 6,046 presents, never reached the battle: x5; discarded) |
| `python tools/game_check.py run --root .` | PASS (1795796875329874200-29be52b6) |

Live counts on the battle route (`task56_bh3`; `task56_final` the same except `Random.Range` 351): `set_acction_command_ratio` 15, `is_barriered` 15, `use_damage_guard` 0 (no barrier hit on the route), `damage_calc` 15, `calc_renkei_ratio` 15, `calc_belome_barrior_ratio` 15, `group_a_b` 15, `calc_element` 15, `get_calc_attack` 15 (1 deferred), `get_calc_magic_attack` 48 (1 deferred), `Random.Range` 419, `initialize_runtime_metadata` 16,026, class init 319. All 0 differences.

Runs discarded: `task56_bh1` (21,157 presents) and `task56_bh2` (11,596) never reached the battle (0 live battle calls): the save I copied from `task42_rt` is not the battle route's save (the route needs the two file save of task55's `bh`); rerun from that save. A blender render took about 9 cores during all runs (CPU 88 to 100 percent, GPU 30 to 39 percent); the later runs still reached the battle with 10,000+ presents.

## Mutation tests (all reverted; `cmp` of `damage.rs` with the saved good copy identical, final build and `task56_final` came after)

| Mutation | Result |
|---|---|
| `_hosei_ratio` default 1.0 changed to 1.5; `is_barriered` `> 0` changed to `>= 0` (one build) | Live A/B `task56_mut`: `set_acction_command_ratio` 15 of 15 and `is_barriered` 15 of 15 mismatched, 30 `decomp mismatch` lines, 25 store journal lines ("port stored 0x3fc00000 ..., memory has 0x3f800000") |
| Skill id 119 dropped from the Geno rule | Not hit by the route; caught by the selftest (`task56_mut2`: 125 differences of 25,625 in `set_acction_command_ratio`, the run stops there, exit 10) |

## Not done

- The `damage_calc` driver and the three big callees `calc_belome_barrior_ratio` (0x71021f5c50), `calc_renkei_ratio` (0x71021f5ed0), `group_a_b` (0x7102201aa0) stay lifted (traced `PLEAF` rows with live counts). Order and designs (out footprint capture, `REC_MAX`, shadow stack) are in `docs/DECOMP.md`; the out footprint and `REC_MAX` change are not built.
- `use_damage_guard` has no live call on this route (selftest only); skill ids other than those the route uses are covered by the selftest and `cargo test` only.
- Replay shows the three ports match the lifted bodies given what they read; it does not show the ratio constants are the intended game design.

## Dependencies and ownership

Task-44, 51 to 55. Owns `decomp/smrpg_decomp`, `runtime/decomp/`, `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.

## Lead verification (2026-11-27)
- Reread task56_final (0 `decomp mismatch`, raced 0, 99 IPC with 0 unknown, misses none), task56_on2 (on mode, 99 IPC, 0 unknown, misses none) and the mutation logs (30 and 125 mismatch lines).
- Partial: the damage_calc driver stays lifted; REC_MAX 12 is too small for it and the out-footprint capture is designed, not built. Next task.
- Diff scanned for em dashes and personal details: none.
