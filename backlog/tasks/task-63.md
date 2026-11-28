---
id: task-63
title: exec_each_targets callees ported (got_physics_attack, resister_thank_you_message, heal_calc)
status: Review
---

## Observed behavior

Task-62 ported `exec_each_targets` (0x71021fe920); its seven callees were still lifted. Sizes of the lifted C: `got_physics_attack` 122 lines, `resister_thank_you_message` 140, `heal_calc` 203, `is_hit_calc_succeed` 816, `skill_calc_exception` 514, `exec_calc_type` 1136, `exec_koudou_kouka` 4193. The three smallest are ported here.

## What was built

- `decomp/smrpg_decomp/src/battle/exec_steps.rs` (new): `BattleCharacterParam.got_physics_attack` 0x71021bdef0 (`smrpg_param_got_physics_attack`), `SkillExecter.resister_thank_you_message` 0x7102200060 (`smrpg_skill_resister_thank_you_message`) and `SkillExecter.heal_calc` 0x71021fff20 (`smrpg_skill_heal_calc`), each a `QLEAF` with a pre-state hook. Register flushes follow the lifted C; the lazy-initialization arms, the class-init arms and the GC polls are ported. `heal_calc` calls `set_acction_command_ratio` through its shim (`Frame::call_port`, nested hook called from its own hook), keeps the int cell on the stack journaled and passes it to the `Int32.ToString` out-row. 12 unit tests (62 in total).
- `runtime/decomp/leaves.def`: three `PLEAF` rows became `QLEAF`; new `PLEAF` rows 0x71021f7690, 0x71018bfa40, 0x71021c3a20.
- `runtime/decomp/swap.c`: `is_nested_row` (damage_calc, heal_calc, got_physics_attack, resister_thank_you_message) replaces the damage_calc-only test in `dispatch` and `smrpg_guest_bl`, so each is recorded as one call of `exec_each_targets` and compared on its own; selftest graphs and saved/restored globals (`t63_save_globals`) for the three rows.
- `decomp/smrpg_decomp/src/bridge.rs` (test fake only): `fake_guest_bl_out` reads the slot pointer before the fake runs and falls back to x0.
- `exec_each_targets` itself is unchanged: it calls the callees through the bridge, which reaches the Rust row in `on` mode and the nested comparison in `ab` mode (a direct `call_port` would lose the live per-function comparison). `docs/DECOMP.md`, "exec_each_targets callees (task-63)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 62 passed |
| `build_game.py` | built; the final build is after all mutations were reverted (`cmp` of `exec_steps.rs` with the saved good copy identical) |
| A/B route `t63_fin_bh` (fresh copy of the battle route in `task63_rt\bh`, final build, 200 s, max 120) | 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none; `exec_each_targets` 15 calls, 0 differences; `damage_calc` 15, 0 differences, 1 deferred; `resister_thank_you_message` 15, 0 differences, 1 deferred (first run); `got_physics_attack` 10, 0 differences; `heal_calc` 0 calls (the route never heals). Earlier runs of the same code: `t63_bh` 18/18/18/7, `t63_self` 15/15/15/9, `t63_fin` 15/15/15/8 live calls, all 0 differences |
| Selftest `t63_fin` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`) | all 13,680 selftest lines 0 differences; `heal_calc`, `resister_thank_you_message`, `got_physics_attack`, `exec_each_targets`, `damage_calc` each 25,625 calls, 0 differences; 0 `decomp mismatch`; raced 0; IPC 99 / 0 unknown; misses none |
| `on` mode `t63_fin_on` (260 s, max 150) | 0 mismatch, IPC 99 / 0 unknown, misses none; `decomp on` calls: `exec_each_targets` 15, `damage_calc` 15, `resister_thank_you_message` 15, `got_physics_attack` 9, `heal_calc` 0 |
| Mutation `got_physics_attack` (logger setter argument 1 to 2) | selftest 7,980 differences (`call 1 to 0x71021f7690, x1: lifted=0x1 rust=0x2`) |
| Mutation `resister_thank_you_message` (the OR bit 2 to bit 4) | selftest 2,886 differences (store journal: `port stored 0x4 ..., memory has 0x2`) |
| Mutation `heal_calc` (skill id limit 1000 to 1001) | selftest 1,149 differences (store journal and call mismatches) |
| `python tools/game_check.py run --root .` | PASS |

The three mutations ran as three separate builds because a selftest failure stops the run at the first failing row; each was reverted and the final build, selftest, route and `on` runs above were made afterwards.

## Limits and still lifted

- `heal_calc` was never reached live (route, `on` run or `ab` run): its evidence is the selftest (25,625 calls on a fake graph with the nested `set_acction_command_ratio` real), the unit tests and the mutation. The route needs a healing skill or item.
- `resister_thank_you_message` and `heal_calc` defer the first run of a process in `ab` mode (flag byte clear, class cell still a token), so their first-call arms are compared by neither the selftest nor the route; they are unit tested and run in `on` mode (resister: first run happened, 0 mismatch). `got_physics_attack` does not defer, so its first-call arm is compared in the selftest.
- The names of the three helper callees 0x71021f7690 (a setter of logger +0x60), 0x71018bfa40 (string literal lookup) and 0x71021c3a20 (acter/target check) are unknown; named by what they do.
- Still lifted: `is_hit_calc_succeed` (0x71021fe9d0), `exec_calc_type` (0x71021fee50), `skill_calc_exception` (0x71021f8040), `exec_koudou_kouka` (0x71021f6340), and `exec_each_targets`'s caller in main_0321.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t63_fin_bh and t63_fin: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none. Live, all 0 differences: exec_each_targets 15, damage_calc 15, resister_thank_you_message 15, got_physics_attack 10 and 8, heal_calc 0.
- t63_fin selftest: every line 0 differences. Mutations t63_mut, t63_mut2, t63_mut3: 1,149, 2,886 and 7,980 selftest differences, each run exit 10.
- t63_fin_on: got_physics_attack 9, resister_thank_you_message 15 in `on` mode.
- `cargo test`: 62 passed. game_check PASS. Open: heal_calc has no live coverage (route has no healing); four callees still lifted.
