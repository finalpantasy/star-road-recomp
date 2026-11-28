---
id: task-65
title: exec_each_targets callees (is_hit_calc_succeed ported)
status: Review
---

## Observed behavior

Task-64 left three callees of `exec_each_targets` (0x71021fe920) lifted: `is_hit_calc_succeed` (0x71021fe9d0), `exec_calc_type` (0x71021fee50) and `exec_koudou_kouka` (0x71021f6340). `is_hit_calc_succeed` is the float-heavy one (hit rate in percent: `scvtf`, `fmul`, `fadd`, `fcmp`, `fcsel`, three `Single.ToString` debug lines) and is ported here. The other two were not started.

## What was built

- `decomp/smrpg_decomp/src/battle/exec_hit.rs` and `exec_hit/tests.rs` (new): `SkillExecter.is_hit_calc_succeed` 0x71021fe9d0 (`smrpg_skill_is_hit_calc_succeed`, hook `_pre`), a `QLEAF`. 13 unit tests, 81 in total. Every float operation goes through `Regs` (`f32_from_i32`, `f32_arith`, `f32_compare`), so results and FPSR flags are the lifted helpers' (fp shadow in `ab` mode). No new `fp.rs` operation was needed.
- `runtime/decomp/leaves.def`: the row became a `QLEAF`; new `PLEAF` rows 0x7101892aa0 (a singleton getter) and 0x71018bfce0 (the random check).
- `runtime/decomp/swap.c`: `is_nested_row` gains 0x71021fe9d0; `t63_save_globals` covers five flag bytes and nineteen slots (flag 0xd70 and the nine literal slots are new); stand-in singleton `tl_stub_singleton`; selftest graph for the row (float edge cases: zeros, negative zero, subnormals, NaNs with payloads, signaling NaN, infinities, rounding boundaries around 100 and 2^24, integer extremes, nine FPCR modes, random FPSR).
- `decomp/smrpg_decomp/src/bridge.rs` (test fakes only): `fake_pre_read` answers a cell the port stored itself first (as the replay journal does); `OUT_FROM_X0` makes `fake_guest_bl_out` take the out pointer from x0.
- `docs/DECOMP.md`, "exec_each_targets callees (task-65)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release --offline`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 81 passed |
| `build_game.py` | built; the final build (`t65_build_final`) is after the mutations were reverted (`cmp` with the saved good copies identical) |
| Selftest, first build (`t65_self0`) | row `0x71021fe9d0`: 25,625 calls, 0 differences on the first build; all 13,681 selftest lines 0 differences |
| Selftest final (`t65_fin_self`, `SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`) | all 13,681 selftest lines 0 differences; `is_hit_calc_succeed` 25,625 calls, 0 differences; fp selftest 2,732,380 cases 0 differences; fp shadow checked=611,123 differences=0; 0 `decomp mismatch`; raced 0; IPC 94 / 0 unknown; misses none |
| A/B battle route (`t65_fin_bh`, fresh copy of the route, 320 s, max 200) | 0 `decomp mismatch`, raced total=0, IPC 99 / 0 unknown, interpreter misses none, fp shadow checked=40,756 differences=0; live calls, all 0 differences: `is_hit_calc_succeed` 18 (1 deferred, first run), `exec_each_targets` 18, `damage_calc` 17 (1 deferred), `resister_thank_you_message` 17 (1 deferred), `skill_calc_exception` 17 (1 deferred), `got_physics_attack` 9, `heal_calc` 0 (the route never heals); no row in the log has a nonzero difference count. The earlier route run on the first build (`t65_bh1`) had 15 calls, 0 differences |
| `on` mode (`t65_fin_on`, fresh copy, 320 s, max 200) | 0 mismatch, IPC 99 / 0 unknown, misses none; `decomp on` calls: `is_hit_calc_succeed` 15, `exec_each_targets` 15, `damage_calc` 14, `skill_calc_exception` 14, `resister_thank_you_message` 14, `got_physics_attack` 7 |
| Mutation 1, float op (`fadd` to `fsub` in the rate sum) | selftest exit 10, 1,050 `decomp mismatch` lines (`t65_mut1`) |
| Mutation 2, branch (forced-miss target id 0x434 to 0x435) | selftest exit 10, 341 `decomp mismatch` lines (`t65_mut2`) |
| `python tools/game_check.py run --root .` | PASS (after the last edit) |

## Limits and still lifted

- The route runs reached the battle at 320 s / max 200 (and also the first try at that setting). Which arms of the function the route hit live is not logged: the live count is calls, not paths. The rate arithmetic, the forced modes, the 0x434 forced miss and the 0x7db certain-hit arm are covered by the selftest (25,625 calls on a fake graph), the unit tests and the two mutations.
- The first run of a process is deferred in `ab` mode like its siblings (flag byte clear, class cells still tokens); the init arm is covered by the selftest (flag clear one time in sixteen is deferred there too) and a unit test.
- Names of 0x7101892aa0 (a singleton getter with flag +0x5c and mode +0x68), 0x71018bfce0 (a random check taking the monitor, the rate and 100.0) and 0x71018bebf0 (an empty debug float writer) are unknown; named by what they do.
- Still lifted: `exec_calc_type` (0x71021fee50), `exec_koudou_kouka` (0x71021f6340), `heal_calc` has no live coverage (task-63), and `exec_each_targets`'s caller in main_0321.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t65_fin_bh: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none; is_hit_calc_succeed 18 live calls, 0 differences; no selftest or live line with differences.
- t65_fin_self: is_hit_calc_succeed 25,625 selftest calls, 0 differences; every line 0 differences.
- Mutations: t65_mut1 (float op) 1,050 mismatch lines, t65_mut2 (branch) 341, both exit 10.
- t65_fin_on: 15 calls in `on` mode, 0 mismatch.
- `cargo test`: 81 passed. game_check PASS. Open: exec_calc_type and exec_koudou_kouka lifted.
