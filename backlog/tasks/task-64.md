---
id: task-64
title: exec_each_targets callees (skill_calc_exception ported)
status: Review
---

## Observed behavior

Task-63 left four callees of `exec_each_targets` (0x71021fe920) lifted: `is_hit_calc_succeed` (0x71021fe9d0, 576 lines of C after dropping register reloads), `exec_calc_type` (0x71021fee50), `skill_calc_exception` (0x71021f8040, 272) and `exec_koudou_kouka` (0x71021f6340). `skill_calc_exception` is the smallest; it is ported here. The other three are not: they were not started (see Limits).

## What was built

- `decomp/smrpg_decomp/src/battle/exec_exception.rs` and `exec_exception/tests.rs` (new): `SkillExecter.skill_calc_exception` 0x71021f8040 (`smrpg_skill_calc_exception`, hook `_pre`), a `QLEAF`. 6 unit tests, 68 in total.
- `runtime/decomp/leaves.def`: the row became a `QLEAF`; new `PLEAF` rows 0x7101887b40, 0x7101887b50, 0x71021ba580, 0x7101bf43d0, 0x71021bde90, 0x71021bddd0.
- `runtime/decomp/game.cmake`: a generated manifest row is dropped when `leaves.def` declares the same address (0x7101887b40 was a generated `DLEAF` getter; a replay trial would run it after the lifted body and see the value the setter wrote).
- `runtime/decomp/swap.c`: `is_nested_row` gains 0x71021f8040; selftest graph for the row; `t63_save_globals` covers the new flag byte and three slots.
- `docs/DECOMP.md`, "exec_each_targets callees (task-64)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release --offline`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 68 passed |
| `build_game.py` | built; the final build is after the mutation was reverted (`cmp` with the saved good copy identical) |
| Selftest, first build (`t64_self1`) | found one bug (x0 at the 0x25d helper call is the acter, not the class): 4,085 differences; fixed |
| Selftest final (`t64_fin_self`, `SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`) | all 13,680 selftest lines 0 differences; `skill_calc_exception` 25,625 calls, 0 differences; 0 `decomp mismatch`; raced 0; IPC 99 / 0 unknown; misses none |
| A/B battle route, final build, fresh copy of the route (`t64_fin_bh5`, 320 s, max 200) | 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none; live calls, all 0 differences: `skill_calc_exception` 18 (1 deferred, first run), `exec_each_targets` 18, `damage_calc` 18 (1 deferred), `resister_thank_you_message` 18 (1 deferred), `got_physics_attack` 12, `heal_calc` 0 (the route never heals); no row in the log has a nonzero difference count. An earlier run on the pre-final build (`t64_bh2`) had 11 calls, 0 differences |
| `on` mode (`t64_fin_on`, 320 s, max 200, fresh `bh_on` copy) | 0 mismatch, IPC 99 / 0 unknown, misses none; `decomp on` calls: `skill_calc_exception` 17, `exec_each_targets` 18, `damage_calc` 17, `resister_thank_you_message` 17, `got_physics_attack` 10 |
| Mutation (`RANGE_HIGH` 0x49 to 0x4a) | selftest 248 differences (`t64_mut`, exit 10); reverted, rebuilt, then all final runs above |
| `python tools/game_check.py run --root .` | PASS |

## Limits and still lifted

- Several A/B route runs never reached the battle (calls 0 everywhere, including `exec_each_targets`: `t64_bh`, `t64_fin_bh`, `t64_fin_bh2`, `t64_fin_bh3`, `t64_fin_bh4` at 200 s / max 120); the 320 s / max 200 runs reached it. Wall-clock dependent, not a decomp difference.
- The route reached only the default path of `skill_calc_exception` live (no skill 0x67, no skill 0x25d, no exception arm); those arms are covered by the selftest (25,625 calls on a fake graph), the unit tests and the mutation. The first run of a process is deferred in `ab` mode like its siblings.
- Names of the helper callees are unknown; named by what they do.
- Still lifted: `is_hit_calc_succeed` (0x71021fe9d0), `exec_calc_type` (0x71021fee50), `exec_koudou_kouka` (0x71021f6340), `heal_calc` has no live coverage (task-63), and `exec_each_targets`'s caller in main_0321.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t64_fin_bh5: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none; skill_calc_exception 18 live calls, 0 differences, 1 deferred. t64_fin_bh3 and bh4 never reached the battle (0 calls, otherwise clean).
- t64_fin_self: skill_calc_exception 25,625 selftest calls, 0 differences. Mutation t64_mut: 248 differences.
- t64_fin_on: 17 calls in `on` mode, 0 mismatch.
- game.cmake override: only 0x7101887b40 appears in both leaves.def and gen.def, so it drops one generated row.
- `cargo test`: 68 passed. game_check PASS. Open: is_hit_calc_succeed, exec_calc_type, exec_koudou_kouka lifted.
