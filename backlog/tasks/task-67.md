---
id: task-67
title: exec_koudou_kouka callees (nine small ones ported) and the raced getter
status: Review
---

## Observed behavior

Task-66 left `exec_koudou_kouka` (0x71021f6340, 4,193 lifted lines) as the last lifted callee of `exec_each_targets`, and its A/B run `t66_fin_bh` logged `decomp raced addr=0x7100492560` once (13 s, thread T31).

## Part A: the raced getter (diagnosis only, no code change)

`swap.c` `compare` runs a load row twice per call: the Rust port, then the lifted C, on the same registers. When the two contexts differ and the row is a load (`load_kind`), it restores the input registers and runs the pair again; if the second pair agrees it counts a race (`leaf->raced`, logged as `decomp raced`) and does not count a difference; if the second pair differs too it is a mismatch as before. A real port bug repeats on the retry, so the retry cannot hide one.

0x7100492560 is the generated row `GLEAF(main_0076, 7100492560, smrpg_g_get_7100492560, 0, 104, 4)` (`gen.def` line 1833): Rust `guest_ld32(a + 0x68)`, lifted C a single `OP_ld(4, ..., 0, 0x68)` and `return`, no side effects and no flags. The only way the two reads can differ is another thread writing that field between them. The log shows it at 13.39 s on T31, a worker thread (guest thread 46, started at 0.888 s), in the same second the main thread tore down the movie decoder (13.36 s). Verdict: benign race, the retry result is correct, the compare is sound. No `swap.c` change. The four later runs of this task (`t67_ab1`, `t67_ab2`, `t67_fin_ab`, `t67_fin_self`) logged `raced total=0`, so it is rare; the code keeps counting it on its own.

## Part B: what was built

- `decomp/smrpg_decomp/src/battle/exec_koudou_leaves.rs` and `exec_koudou_leaves/tests.rs` (new): nine rows on `BattleCharacterParam` and friends, all `BuffManager` accessors or tail-call loaders (`+0x28` is `_invincible_turn`): 0x71021be0f0 clear the turn, 0x71021ba6d0 byte +0x11, 0x71021ba6a0 byte +0x10 (with a collection safepoint), 0x71021be1f0 `int[]` element add with bounds check, 0x71021bd6c0, 0x71021be2f0, 0x71021c3c00, 0x71021c68c0 (tail calls), 0x71021bf040 (call, u16 copy, tail call). 9 unit tests, 101 in total.
- `runtime/decomp/leaves.def`: five `QLEAF` and four `ELEAF` rows for the nine; `PLEAF` rows for their six callees (0x71021be300, 0x7101c060b0, 0x71021c68d0, 0x7101c06250, 0x7101e22a40, 0x7101e30240) and 22 probe `PLEAF` rows for the other unported callees of `exec_koudou_kouka` (list in docs/DECOMP.md).
- `runtime/decomp/swap.c`: `is_deep_row` and the depth-1 nested replay in `dispatch` (a replay row of the set reached from the lifted body of a traced row inside a recording is compared on its own); 2-byte cells in `raw_load` and `raw_store`; a `prepare_caller` graph for the nine rows (param, status manager, buff manager, `int[]` with in-range, at, past and far-past indexes, tail objects).
- `decomp/smrpg_decomp/src/bridge.rs`: `prestate::store_u16`.
- `docs/DECOMP.md`, "exec_koudou_kouka callees (task-67)": the callee table with sizes, what is ported, and the plan to split `exec_koudou_kouka`.

## Callees of exec_koudou_kouka (lifted lines)

See docs/DECOMP.md for the full table. Unported and now probed: 0x71021f76a0 (825), 0x7101cd4790 (644), 0x71021f7b40 (585), 0x7101e30a60 (362), 0x7100185130 (320), 0x7101c2e370 (229), 0x71021be470 (222), 0x71018bfbc0 (202), 0x7101b6b8b0 (156), 0x71021be5c0 (129), 0x71021b9c50 (107), 0x7101e287b0 (64), 0x7101b51870 (55), 0x7101e30400 (34), 0x71021ba3b0 (5) and seven rows of 12 to 16 lines.

## Split plan for exec_koudou_kouka

Full text in docs/DECOMP.md. In short: (1) prologue, lazy init and the first range test as one port whose pre-state hook defers when the arm it would pick is not ported; (2) jump table 1 (31 or more arms) by skill family; (3) the `List<T>` enumerator loop with its exception landing, and table 2 with its arms (the bulk of the lines); (4) table 3 (`skill_id - 0x2d9 < 0x12`) and the epilogue; larger traced callees (0x71021f76a0, 0x71021f7b40) ported first when an arm needs them; a route that reaches the arms is needed for live counts.

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release`, `cargo test` (in `decomp/`) | clean; 101 passed. Plain `cargo check` (dev profile) fails with "unwinding panics are not supported without std", with or without this task's module (the crate is `panic = "abort"` in release only); not caused by this task |
| Build (`t67_build3`) | ok |
| Selftest first build (`t67_self1`) | all nine rows 25,625 calls, 0 differences |
| Selftest final (`t67_fin_self`, SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1, 340 s, max 150) | 13,688 selftest lines, 0 with differences; the nine rows 25,625 calls each, 0 differences; fp selftest 2,732,380 cases 0 differences; fp shadow 692,373 / 0; 0 mismatch; raced 0; IPC 99 / 0 unknown; misses none |
| A/B battle route (`t67_fin_ab`, 320 s, max 200) | 0 `decomp mismatch`; raced total 0; IPC 99 / 0 unknown; misses none; live calls, all 0 differences: `exec_each_targets` 15, `is_hit_calc_succeed` 15, `damage_calc` 14 (1 deferred), `exec_calc_type` 14, `skill_calc_exception` 14, `resister_thank_you_message` 14, `got_physics_attack` 6, `calc_renkei_ratio` 1, `set_acction_command_ratio` 1, `calc_belome_barrior_ratio` 1, `group_a_b` 1, `calc_attack` 1, `calc_defence` 1; `heal_calc` and `use_damage_guard` 0. The nine new rows: 0 live calls (the route runs `exec_koudou_kouka` 15 times but its only traced calls are the `is_invincible` tests) |
| `on` mode (`t67_fin_on`) | 0 mismatch, IPC 99 / 0 unknown, misses none; `exec_each_targets` 15 calls; the nine new rows 0 calls |
| Mutations, each caught at its own row, selftest exit 10 (`t67_mut1` to `t67_mut9`) | 1 store 1 for 0 in 0x71021be0f0 (25,626 lines); 2 `<= 0` to `< 0` in 0x71021ba6d0 (5,610); 3 `!=` to `==` on the status in 0x71021ba6a0 (14,085); 4 add to subtract in 0x71021be1f0 (13,861); 5 `> 0` to `>= 0` in 0x71021bd6c0 (5,781); 6 x0 plus 8 in 0x71021be2f0 (25,626); 7 `w1` to `x1` in 0x71021c3c00 (15,626); 8 offset 0x18 for 0x10 in 0x71021c68c0 (23,907); 9 offset +0x14 for +0x12 in 0x71021bf040 (15,889). All reverted (source identical to the saved good copy) and rebuilt before the final runs |
| `python tools/game_check.py run --root .` | PASS after the last edit (see the lead note below if this line says otherwise) |

## Limits and still lifted

- The nine rows have no live calls on this route. Their proof is the selftest (all branches, 25,625 calls each) and the mutations. The depth-1 comparison path in `dispatch` (`is_deep_row`) was never reached live, so it is checked by reading only; the first route that reaches one of these rows inside `exec_koudou_kouka` is its live test.
- Probe-only `PLEAF` rows were added for 22 unported callees; none was called at depth 0 of a recording (0 mismatch).
- Still lifted: `exec_koudou_kouka` itself and its larger callees, `heal_calc` live coverage, `exec_each_targets`'s caller in main_0321.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t67_fin_ab, t67_fin_self, t67_fin_on: 0 `decomp mismatch`, raced total=0, 0 unknown IPC, interpreter misses none, no selftest line with differences; exec_each_targets 15 live calls, 0 differences.
- Nine mutation runs, each exit 10.
- The nine new rows have 0 live calls on this route; selftest, unit tests and mutations are their only evidence.
- Dev-profile `cargo check` ("unwinding panics are not supported without std") fails on HEAD 3549cbb as well, so it predates this task; release check and `cargo test` (101 passed) are clean. game_check PASS.
