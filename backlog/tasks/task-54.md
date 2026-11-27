---
id: task-54
title: Stricter float flag check in the A/B, and the Battle defence stat ported readably
status: Review
---

## Observed behavior

Task-53 found that a mutation dropping IXC for one sign of result was caught only by the startup float selftest, never by the live A/B: FPSR cumulative flags are sticky and the game sets IXC early. A port that forgets a flag was invisible live. Task-53 also left no damage-related method ported; the Battle damage math itself had not been looked at.

## Part A: the stricter check

Three layers, `ab` mode only, game behavior unchanged (original bits ORed back). Details in `docs/DECOMP.md` ("Stricter float flag check").

- Row level (`compare_replay`, `swap.c`): replay rows (`ELEAF`) start both sides with the cumulative FPSR bits (mask 0x0800009f) cleared, compare what each raised, then OR the original bits back into the live context.
- Callee flags hidden (`dispatch`, `Recording.raised`): a traced callee runs with the cumulative bits cleared, what it raised is held aside, and the body's own bits are put back; the held bits are ORed back after the compare. Found by measurement: with the start-cleared compare alone the mutation below was still invisible (`task54_mut_ixc2`, 4,047 camera calls, 0 differences), because the lifted callees (`set_eulerAngles` and others) raise IXC themselves and the replay copied their post-call FPSR into the port's context, masking any omission after them.
- Operation level (`Regs::fp_op` in `bridge.rs`, `smrpg_fp_shadow` in `fp_selftest.c`): every float operation a shim runs (fadd, fsub, fmul, scvtf, fcvtzs, fcmp) also runs on the lifted C helper with the same FPCR, FPSR and operands and must agree on result and FPSR. This catches a flag dropped for one sign even when another operation of the same call raises the bit. Exit summary: `decomp fp shadow checked=N differences=N`.

## Part B: the port

Search (probe rows `PLEAF` in `leaves.def`, build 1, run `task54_probe_bh`, copy of `codex_battle_hard_02`, 150 s of game, lifted body unchanged):

| Method | Insns | Live calls | Verdict |
|---|---|---|---|
| `SkillExecter.damage_calc` 0x71021ff580 | 613 | 18 | the damage formula driver; over the size limit |
| `SkillExecter.is_hit_calc_succeed` 0x71021fe9d0 | 286 | 18 | hit rate; not taken (kept to one stat family) |
| `CharaSkillResultLogger.calc_damage_normal` 0x71022028c0 | 303 | 347 | damage number display (stack structs, vector math, UI calls), not the math |
| `CharaSkillResultLogger.get_total_value` 0x7102202740 | 96 | 347 | calls the two above, lazy static init, stores |
| `BattleCharacterParam.apply_damage_and_status_eff` 0x71021bf9f0 | 277 | 18 | logging and lazy static initialization |
| `BattleCharacterParam.get_calc_attack` 0x71021c3ea0 | 96 | 18 | non-player branch has class-init checks and two GC polls; skipped |
| `BattleCharacterParam.get_calc_defence` 0x71021bc270 | 28 | 18 | **taken** |
| `BattleCharacterParam.get_calc_magic_defence` 0x71021bc440 | 28 | 0 | **taken** (same shape; selftest only) |
| `BattleCharacterParam.get_calc_magic_attack` 0x71021bc2e0 | 85 | 57 | same issues as `get_calc_attack` |
| `BuffManager.attack_up_ratio` / `defense_up_ratio` | 106 / 100 | 31 / 25 | lazy init and static-field debug paths |
| `BattleCharacterParam.calc_element` 0x71021be3c0 | 21 | 18 | traced only |
| `BattlePointParam.damage` 0x7101c06100 | 9 | 13 | traced only (below any useful size) |
| `BattlePointParam.poison_damage_value`, `mushroom_heal_value` | 39, 32 | 0, 0 | not reached on the route |
| `SkillExecter.heal_calc`, `CharaSkillResultLogger.calc_damage_kobetsu` | 77, 207 | 0, 0 | not reached on the route |

Taken: the two defence stat methods, 28 lifted instructions each (below the 50 asked; the damage driver is 613 and the 50 to 400 methods are display or logging code, table above). `battle.rs`: `calc_defence`, `calc_magic_defence`, `calc_defence_stat`, `scale_by_ratio`, `DefenceStat`, `StatCalls` (ratio call, index throw, leave), typed `BuffManager.add_status` and `BattleCharacterParam.is_player_byte`; `Int32Array` in `object.rs`; `Float32Convert` in `fp.rs`. Shim `LiftedStat`. Rows `ELEAF(main_0376, 71021bc270 and 71021bc440)`, traced callees `PLEAF` for `defense_up_ratio` (main_0377, from the probe set), `magic_defense_up_ratio` (main_0377) and the index thrower 0x71000c4510 (main_0016). Live call count: `get_calc_defence` 18 (selftest run), 15 (A/B run); `get_calc_magic_defence` 0 live.

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean; 33 tests (2 new: the readable body against an independent host-float reference over 2 stats x 4 player bytes x 7 lengths x 9 x 9 ints x 14 ratios including NaN, infinities and 2^31; the shim against a transcription of the lifted C, 2 x 3 x 6 x 7 x 24 = 6,048 graphs, whole register file and call order). One older warning in `lib.rs` (unused assignment in a test) is not from this task |
| `python -I runtime/decomp/check_pure.py` | PASS for both entries |
| `python -I tools/rtprep/build_game.py` | build 1 (19 probe rows, 4 chunks) 7 min 50 s; build 2 (ELEAF rows, 2 more chunks) 7 min 1 s; later rebuilds 28 to 35 s |
| Selftest run, copy of `codex_battle_hard_02` in `task54_rt\bh`, `task54_final_selftest` (`--seconds 340 -- --max-seconds 150`; 260 timed out once because the startup selftest took 176 s on the loaded machine) | 0 `decomp mismatch`; raced 0; "IPC: 99 distinct commands used, 0 unknown"; "interpreter misses: none"; 9,249 presents; `decomp fp selftest cases=2732380 differences=0`; both new rows 25,625 calls, 0 differences; `check_log.py --summary`: selftest clean for 13,666 rows, live differences 0; `decomp fp shadow checked=218432 differences=0` |
| A/B, same copy, `task54_final_bh` | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 13,118 presents; `get_calc_defence` 15 calls 0 differences, `defense_up_ratio` 21, `magic_defense_up_ratio` 16, camera 7,489, 0 differences; shadow 44,954 operations, 0 differences |
| A/B, copy of the `task42_rt` save in `task54_rt\t42`, `task54_final_t42` | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 27,355 presents; defence rows 0 live calls (field route), camera 2,241 calls 0 differences; shadow 13,446 operations, 0 differences |
| `python tools/game_check.py run --root .` | PASS (1795785262417244600-88a5ef4d) |

Runs shared the GPU with other work (up to 100 percent). Two runs fell below 5,000 presents (`task54_mut_ixc5` 2,758 and `task54_mut_ixc6` 4,516) and were discarded and rerun; every run quoted above is over 5,000.

## Mutation tests (all reverted)

| Mutation | Result |
|---|---|
| Part A: `round` in `fp.rs` sets IXC only for negative results (drops it for positive results), the mutation of task-53 | Live A/B, `task54_mut_ixc7`: 1,237 `decomp mismatch` lines, first at 48 s: `decomp mismatch addr=0x7101e2cfd0 replay: recorded=6 used=6 call_mismatch=1` (row level, FPSR differs at the first `Quaternion.Internal_FromEulerRad` call) and `decomp mismatch fp shadow op=0 fpcr=0 fpsr_before=0 a=0x41dc0000 b=0x3c8efa35: rust=0x3ef5be0b/0 lifted=0x3ef5be0b/0x10`, shadow 2,442 differences in 7,334 operations. Same mutation with the start-cleared compare only: 0 mismatches (`task54_mut_ixc2`). |
| Part A, opposite sign: IXC only for positive results | Not seen live by any layer (`task54_mut_ixc3`, `task54_mut_ixc4`: camera 4,836 calls, shadow 44,890 operations, 0 differences): no operation on the hard route returns a negative inexact result, so the DBG print showed the lifted body raising IXC from positive results only. The startup selftest still catches it; unit test `fp::tests::rounding_modes` fails too. |
| Part B: `is_player_byte == 0` changed to `!= 0` in `calc_defence_stat` | Live A/B, `task54_mut_defence`: 11 of 11 `get_calc_defence` calls mismatched ("replay: recorded=1 used=0", "the port made an extra call to 0x71021c4240"). |

`fp.rs` was restored from a saved copy (`cmp` identical) and `battle.rs` likewise; the final runs came after the reverts.

## Not shown

- The defence methods' positive-ratio path is live with ratios that the game computes (mostly 1.0 and small buff values); NaN, infinity, 2^31 and the `int.MinValue` quirk are covered by `cargo test` and the selftest (random ratios from the stub callee), not by the route. `get_calc_magic_defence` is covered by the selftest and `cargo test` only.
- The index-throw path (`_add_status` shorter than needed) is selftest and `cargo test` only.
- The shadow check covers single precision fadd, fsub, fmul, scvtf, fcvtzs and fcmp, the operations the shims use. Double precision and the other conversions are in the startup selftest only.
- Replay still shows the port matches the lifted body given the callees' recorded results; it does not show the Unity natives or the buff manager ratio methods are right (unchanged lifted code).
- Damage formula (`damage_calc`, 613 instructions) and `get_calc_attack` (class-init checks, GC polls) are not ported.

## Dependencies and ownership

Task-44, task-51, task-52, task-53. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.

## Lead verification (2026-11-27)
- Reread task54_final_bh, task54_final_t42 and task54_final_selftest logs: 0 `decomp mismatch` lines each, `decomp raced total=0`, 99 IPC commands with 0 unknown, interpreter misses none; selftest log shows `decomp fp selftest cases=2732380 differences=0`.
- Diff scanned for em dashes and personal details: none.
- Not done: the damage formula (damage_calc, 613 instructions) and get_calc_attack.
