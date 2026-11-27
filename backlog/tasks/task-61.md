---
id: task-61
title: damage_calc enemy rules ported (the four character ids no longer run lifted)
status: Review
---

## Observed behavior

Task-60 ported the `damage_calc` driver (0x71021ff580) except the `switch` cases for the target character ids 0x5dd, 0x5ec, 0x615 and 0x653, which ran the lifted body in `on` mode and were deferred in `ab`. This task ports them.

## What was built

- `decomp/smrpg_decomp/src/battle/damage.rs`: the four cases, written from the disassembly and the lifted C (call sites, flush lists, safepoints).
  - 0x5dd: `BattleManager` class and flag checks, then `_instance` int at +0xd0; at 7 or more: debug line, `_result_logger` byte +0x4a = 1, `damage = 0`.
  - 0x5ec and 0x653: debug line, byte +0x4a = 1, `damage = 0`, then the inlined `List<int>.Clear` of the logger's `_multi_number` and one `Add(0)` per entry it held (`IsReferenceOrContainsReferences`, `Array.Clear`, `List<int>.Add` through the bridge; the list header is the out footprint of `Add`, as in `calc_belome_barrior_ratio`).
  - 0x615: the class and flag checks, `_instance._unnamed_60` byte +0x20; when set: debug line, byte +0x4a = 1, `damage = 0`, skill 0x75 sets the `GlobalWork` instance byte +0xd8, then the same Clear and Add loop (second compiled copy, other return addresses).
  - The `UNPORTED_CHARACTERS` list and the `run_lifted` entry are gone.
  - Pre-state hook: snapshots the logger byte +0x4a, the list header, the `GlobalWork` class word and instance byte; defers skill 0x2c3 on 0x5ec, 0x653 and 0x615 (as on Belome: `calc_renkei_ratio` replaces the list first, so the header at the clear is not the pre-state one).
- `belome.rs`: `ListInt` and its header accessors, `multi_number` are `pub(crate)`.
- `runtime/decomp/swap.c`: selftest graph picks the four ids one case in eight and varies +0xd0, the +0x60 object's byte, the logger byte and the instance byte; GlobalWork stand-in static fields; a live counter `decomp damage_calc by target: 0x5dd=.. 0x5ec=.. 0x615=.. 0x653=.. other=..` in the summary (counts only after the startup selftest).

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 47 passed |
| `build_game.py` | built; final build after both mutations were reverted (`cmp` with the saved good copy identical) |
| A/B route `t61_bh2` (copy of `codex_battle_hard_02` in `task61_rt\bh`, own save, 200 s, max 120, final build) | 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none, `damage_calc` 15 calls, 0 differences, 1 deferred; by target: 0x5dd 5, 0x5ec 0, 0x615 0, 0x653 0, other 10 |
| Selftest `t61_fin` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`, final build) | driver 25,625 calls, 0 differences; 0 `decomp mismatch`; raced 0; IPC 99 / 0 unknown; misses none; live 6 calls, 0 differences (0x5dd twice) |
| `on` mode `t61_on2` (260 s, max 150) | 0 mismatch, IPC 99 / 0 unknown, misses none, `decomp on addr=0x71021ff580 calls=10` (all other ids; `t61_on`, 200 s, reached no battle: calls 0) |
| Mutation 1 (`MANAGER_COUNT_LIMIT` 7 to 8) | 98 differences in the selftest, mismatch lines logged |
| Mutation 2 (clear path `x9 = version + 2`) | 1,352 differences in the selftest (`call_mismatch=1`) |
| `python tools/game_check.py run --root .` | PASS |

Route variance: the same route gave 10, 0, 6, 10 and 15 live `damage_calc` calls over the runs of this task (the battle is reached late or not at all in some runs). Only 0x5dd was ever hit live (7 compared calls over two runs, 0 differences). 0x5ec, 0x615 and 0x653 were never hit by the route: they are covered by the selftest only (about one case in eight per run is one of the four ids, split evenly).

## Limits

- The first call per process stays deferred (lazy init pending), as in task-60; the hook also defers skill 0x2c3 on 0x5ec, 0x653 and 0x615.
- The `GlobalWork` instance byte (+0xd8) is watched only when the static field is already set at the hook; otherwise only the port's own store is checked.
- Names of the fields at BattleManager +0xd0 and +0x60 and logger +0x4a are unknown; the rules are named by their offsets.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t61_bh2: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none; damage_calc 15 calls, 0 differences, 1 deferred; by target 0x5dd=5, other ids 0.
- t61_fin selftest: driver 25,625 calls, 0 differences. Mutations t61_mut1 and t61_mut2: 98 and 1,352 selftest differences.
- t61_on2: damage_calc 10 calls in `on` mode, 0 mismatch.
- `cargo test` passed, game_check PASS. Open: 0x5ec, 0x615 and 0x653 have selftest coverage only.
