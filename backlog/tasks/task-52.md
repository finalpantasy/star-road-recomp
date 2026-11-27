---
id: task-52
title: Guest-call bridge for readable ports, and Battle.BattleHit.exec_hit through it
status: Review
---

## Observed behavior

The readable ports of task-51 could only call other Rust code, so any game method with a lifted callee stayed lifted. This task builds the bridge (Rust calls a lifted function by guest address), fixes the `ab` rule for it, and ports one larger Battle method with it.

## Part A: the bridge

- `decomp/smrpg_decomp/src/bridge.rs` (206 lines with 4 tests): `Regs` over the register file the port edits, `Regs::invoke(target)` (a lifted `bl` on the caller's own context: x0 to x7, x30 and sp as the port staged them, callee residue left in place), `Regs::call` (stages x0 to x7 and x30), float helpers (`set_f32`, `f32_result`), `GuestException` for a pending exception. `smrpg_bridge_selftest_thunk` is the selftest row.
- `runtime/decomp/swap.c` `smrpg_guest_bl` (about line 400): direct call of the registered entry (`f_<addr>(c)`, so a callee that is a Rust row goes through its wrapper), interpreter fallback `rt_icall` when there is no lifted entry (it sets c->pc like a miss; a direct call does not, matching the lifted `bl`), returns `c->exc != 0`.
- ab rule, chosen: two row kinds. `CLEAF` ports may call only the pure allowlist (`runtime/decomp/pure.def` plus the ported load rows); any other target in `ab` mode exits 10 before running. Every `pure.def` entry is proven from its lifted chunk by `runtime/decomp/check_pure.py` (stores only in its own stack frame, no indirect call, no atomic or system operation, callees pure in turn). Why this and not lifted-only: the whole `Ctx` is still compared byte for byte, so the residue the callee leaves is checked for real. `ELEAF` ports (callees with side effects) are compared by replay, see below. Never run a side effect twice: pure callees cannot have one, replay executes each callee once.
- What the compare covers is written out in `docs/DECOMP.md` ("Guest-call bridge"). Short form: pure rows, whole `Ctx` equality after both bodies ran (x, v, flags, fpcr, fpsr, pc, exception fields, scratch), not stack memory below sp and not heap memory (the proof says the callee writes none; the port must write none). Replay rows, order, number, callee and full register state at every call, final `Ctx`, every recorded call used; not what the callees do, and reads that happen after the callees ran.
- Replay machinery (`ELEAF`, `PLEAF` rows, `compare_replay`, `replay_call`): added because no Battle method in the size range has a provably pure callee set (next section), so `ab` needed a way to check a port with effectful callees without running them twice.
- Fix found on the way: `runtime/decomp/game.cmake` rebuilt the Rust archive only when `lib.rs` changed, so a change to `battle.rs` or `bridge.rs` alone left a stale archive (the first link of this task failed with an undefined `smrpg_hit_exec_hit`). It now depends on every `src/*.rs`.
- Selftest row: `0x7101b8abd0`, a thunk (`b 0x7100049adc`) over the pure chain `0x7100049adc` and `0x710006b41c`, fake graph in `prepare_caller`. It is live in the game too: 26,384, 30,258 and 32,039 calls in the three battle-route runs, 0 differences, with the callee run twice (by Rust, then lifted) under `ab`.

## Part B: candidate search

Scan of all 1,383 `Battle*` methods of 50 to 300 instructions in `Assembly-CSharp` (lifted C parsed for stores, indirect calls, atomics and callees, `tools` not touched; scratch scripts only). Result: 12 have no store outside the stack frame, no indirect call and no atomic; 0 have a callee set that is itself provably pure (the callees are Unity transform natives behind a lazy icall thunk, virtual getters such as `get_IsPc`, lazy class-init stores, or effectful game calls). Pure methods exist only below 27 instructions (28 of the 2,428 methods of 12 to 300). So the "pure callees" route is empty at this size and the replay row kind was needed.

Live evidence (probe rows, hidden muted `ab` run `task52_ab1`, copy of `codex_battle_hard_02`, 15,729 presents, 99 IPC, 21 Battle methods traced for 150 s of game):

| # | Method | Insns | Live calls | Verdict |
|---|---|---|---|---|
| 1 | `BattleCamera.adjust_camera_transform` (0x7101e2cfd0) | 69 | 10,287 | Camera follow, 11 float ops, Unity transform setters. Not taken: float ports must also reproduce FPSR sticky flags (the lifted `fadd`, `fsub`, `fmul` set IXC and UFC in the context), a model of its own. Best next target. |
| 2 | `BattleHit.exec_hit` (0x7101cf7650) | 57 | 20 | Damage and hit sequence: sound-only hit, third reserve hit, motion notifications, damage and weak/resist popups, hit count, combo, hit. Integer only, readable. **Taken.** |
| 3 | `BattleCharacterAnimator.add_anims_in_layer` (0x7101e341c0) | 95 | 9 | Loop with float and array argument. Not taken. |

Also traced: `BattleCharacterMotion.get_euler_angles_to` (57 insns, 5 calls), `BattleCharacterParam.is_delete_gameobject_after_act` (20 insns, pure, 14 calls), 16 more with 0 calls (`get_hit_valid_state`, `get_diversion_hit_effect_id`, `is_disp_damage_value`, and others).

## Part B: the port

`decomp/smrpg_decomp/src/battle.rs` (990 lines with the tests of task-51 and this task): classes `BattleHit` (`_target` +0x20, `_skill_params` +0x28, `_is_only_se_hit` +0x81), `BattleCharacterMotion`, `BattleSkillParams`, `BattleCharacter._motion` +0x18, `MotionSpecialTiming` (`HIT_BEFORE` 4, `HIT` 5, from the enum order).

Readable versus shim. Readable: `exec_hit` (29 lines, lines 274 to 302), the two enums, `HitExit` and the `HitCalls` trait that names the eight callees, about 135 lines with the classes. Shim: `LiftedHit` and `smrpg_hit_exec_hit`, about 210 lines, which stage x0 to x4, x8, x19, x29, x30 and sp at each call site exactly as the lifter flushes them (frame push at the first call, the entry values back before a tail call, x0 on the three exits). The shim is larger than the readable part: that is the cost of whole-context equality with lifted code.

Row: `ELEAF(main_0331, 7101cf7650, smrpg_hit_exec_hit)`; traced callees `PLEAF` for `only_se_hit`, `is_renkei_3_reserve_hit`, `hit`, `disp_damage`, `disp_weak_resist`, `disp_hit_count`, `set_disp_combo` (main_0331) and `BattleCharacterMotion.calc_motion` (main_0345).

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean, 15 tests (6 new: bridge call and exception, tail call and thunk, float argument, `exec_hit` call order over 2x5x2x3 graphs against an independent table, `exec_hit` shim staging through a fake bridge). One older warning in `lib.rs` (unused assignment in a test) is not from this task. |
| `python -I runtime/decomp/check_pure.py` | PASS for both `pure.def` entries; negative checks fail as they should (a store through x0, `rt_icall`, an unlisted callee) |
| Build `python -I tools/rtprep/build_game.py` | build 1 (bridge, thunk row, 21 probe rows, 12 chunks) 7 min 53 s; build 2 (ELEAF) failed at link after 8 min 54 s (stale archive, see above), relink 43 s; final rebuilds 26 s |
| Selftest, 10,000 random + 15,625 edge each, fake graph | thunk row 25,625 calls, 0 differences; `exec_hit` 25,625 calls, 0 differences (callees are stand-ins) |
| Mutation: `RET_HIT` changed by 4 | selftest failed at startup, exit 10, "call 6 to 0x7101cf7aa0, x30: lifted=0x7101cf76fc rust=0x7101cf76f8" (`task52_mut1`) |
| Mutation: the thunk calls an unlisted function | exit 10 "callee 0x7101cf7aa0 is not on the pure allowlist" before it ran (`task52_mut2`) |
| A/B, copy of `codex_battle_hard_02` with its input in `task52_rt\bh`, `task52_final_bh` | 0 `decomp mismatch`, raced 0, interpreter misses none, exit 4 (watchdog), 18,434 presents, 99 IPC. Live: `exec_hit` 24 (12 sound-only, 12 full), thunk 32,039, 0 differences |
| A/B, same build, copy of the `task42_rt` save in `task52_rt\t42`, `task52_final_t42` | 0 mismatch, raced 0, misses none, 21,202 presents, 99 IPC. Live: `exec_hit` 9, thunk 23,766, 0 differences |
| Earlier A/B on the build with the same sources, `task52_ab2` | 0 mismatch, 17,601 presents, 99 IPC, `exec_hit` 18, thunk 30,258 |
| `python tools/game_check.py run --root .` | PASS (1795774488477293900-b9aeaa0f) |

GPU was busy from other work during the final runs (98 to 100 percent before them, run_game warned); presents stayed above 18,000, so they are valid runs.

## Not shown

- The renkei reserve-hit tail (`hit_tail` path) and a pending-exception return never occurred live; the selftest covers both with stand-in callees. The live `exec_hit` calls (24 and 9 in the two final runs) were sound-only hits and full hits.
- Replay compares the port against the lifted body with the recorded callee results. It does not show the callees themselves are right (they are unchanged lifted code) and cannot see a field a callee rewrites between two reads (the port runs after the callees). None of the 33 live calls showed one.
- Floating point is still not ported; the FPSR flags are the open item (see candidate 1).
- Only `exec_hit` uses replay. No `PURE_FN` function other than the selftest chain exists yet.

## Dependencies and ownership

Task-44, task-51. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md` untouched (not owned by this task).

## Lead verification (2026-11-27)
- Reread task52_final_bh and task52_final_t42 logs: 0 `decomp mismatch` lines, `decomp raced total=0`, 99 IPC commands with 0 unknown, interpreter misses none, presents 18,360 and 21,120.
- `check_pure.py` reruns PASS. In `callee_is_pure`, store rows (`rust_store`) and vector stores (vkind 2) are not counted as pure, so a store callee in ab mode exits 10 before it runs.
- Diff scanned for em dashes and personal details: none.
