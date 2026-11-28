---
id: task-66
title: exec_each_targets callees (exec_calc_type ported)
status: Review
---

## Observed behavior

Task-65 left `exec_calc_type` (0x71021fee50) and `exec_koudou_kouka` (0x71021f6340) lifted. `exec_calc_type` (about 1,100 lifted lines, a 14-entry jump table on the skill's calc type) is ported here. `exec_koudou_kouka` (4,193 lifted lines) is over the size limit and was not started.

## What was built

- `decomp/smrpg_decomp/src/battle/exec_type.rs` and `exec_type/tests.rs` (new): `SkillExecter.exec_calc_type` 0x71021fee50 (`smrpg_skill_exec_calc_type`, hook `_pre`), a `QLEAF`. All arms ported: skill 0x286, physical, magic, sum, counter, value only, result A and B, prepare-then-call, call, exit. Stat getters are called through `Frame::call_port`. 11 unit tests, 92 in total.
- `runtime/decomp/leaves.def`: the row became a `QLEAF`; new `PLEAF` rows 0x7101887ac0, 0x7101887b00, 0x710188bf40, 0x710188dd30, 0x71021b7fb0, 0x7102200150.
- `runtime/decomp/swap.c`: `is_nested_row` gains 0x71021fee50; `t63_save_globals` covers six flag bytes and 23 slots; selftest graph for the row (executer, result object, skill object, acter and target params with buff managers and weapons, BattleManager statics and counter object, GlobalWork and strawberry cells, nine FPCR modes).
- `docs/DECOMP.md`, "exec_each_targets callees (task-66)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check`, `cargo test` (in `decomp/`) | clean; 92 passed |
| Selftest, first build (`t66_self1`) | 25,625 calls, 0 differences |
| Selftest final (`t66_fin_self`) | 13,679 selftest lines, 0 nonzero; `exec_calc_type` 25,625 calls, 0 differences; fp selftest 2,732,380 cases 0 differences; fp shadow 700,107 differences 0; 0 mismatch; IPC 99 / 0 unknown; misses none |
| A/B battle route (`t66_fin_bh`, 320 s, max 200) | 0 `decomp mismatch`, IPC 99 / 0 unknown, misses none, fp shadow 79,348 / 0 differences; live calls all 0 differences: `exec_calc_type` 15 (1 deferred), `exec_each_targets` 15, `damage_calc` 15 (1 deferred), `is_hit_calc_succeed` 15, `skill_calc_exception` 15, `resister_thank_you_message` 15, `got_physics_attack` 9. `decomp raced total=1`: addr 0x7100492560 at 13 s (startup thread, a function not in this task), "first pair differed, retry agrees"; the earlier run `t66_bh1` had raced 0 |
| `on` mode (`t66_fin_on`) | 0 mismatch, IPC 99 / 0 unknown, misses none; `exec_calc_type` 15 calls, `exec_each_targets` 15 |
| Mutation 1 (`wrapping_sub` to `wrapping_add`, physical arm) | selftest exit 10, 1,898 mismatch lines (`t66_mut1`) |
| Mutation 2 (TAIL_MAGIC `AddIfByte(2)` to 3) | selftest exit 10, 1,229 mismatch lines (`t66_mut2`) |
| `python tools/game_check.py run --root .` | PASS after the last edit |

## Limits and still lifted

- The route reaches the arms its battle uses; which calc types ran live is not logged. All arms are covered by the selftest (25,625 calls) and the two mutations.
- Still lifted: `exec_koudou_kouka` (0x71021f6340, 4,193 lifted lines), `heal_calc` live coverage, and `exec_each_targets`'s caller in main_0321.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t66_fin_bh, t66_fin_self, t66_fin_on: 0 `decomp mismatch`, 0 unknown IPC, no selftest line with differences; exec_calc_type 15 live calls, 0 differences, and 15 calls in `on` mode.
- Mutations t66_mut1 and t66_mut2: 1,898 and 1,229 mismatch lines, both exit 10.
- t66_fin_bh `decomp raced total=1`: 0x7100492560, a generated gen.def row on startup thread T31 at 13 s, first pair differed and the retry agreed. This is the first raced event in any log in build/logs; not caused by this task's rows. Watch it in later runs.
- `cargo test`: 92 passed. game_check PASS. Open: exec_koudou_kouka (4,193 lifted lines) still lifted.
