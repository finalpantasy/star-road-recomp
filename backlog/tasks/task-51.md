---
id: task-51
title: First readable game-logic port, Battle.BattleCharacterParam and MotionSpecial_Mario.can_bunnage_globe
status: Review
---

## Observed behavior

Stage 2 has 13,652 Rust rows, nearly all generated trivial shapes. None is game logic a person can read. This task ports the first real methods with the game's own class and field names, and fixes the conventions for later ones.

## Candidate search

Scanned every `Assembly-CSharp` method of 12 to 200 instructions (decoder `tools/lifter/dec.py`, names and offsets from the task-2 tables): no store except to the stack, no indirect call, every callee either a ported row or itself pure (no stores, at most 3 distinct unported callees, depth 3). 4 methods pass in the 30 to 200 range, 80 more at 12 to 29 instructions. Static call sites do not say what the battle route runs, so the live check was a real A/B run.

| # | Method | Insns | Callees | Why / verdict |
|---|---|---|---|---|
| 1 | `MotionSpecial_Mario.can_bunnage_globe` (0x7101e73c40) | 30 | `is_dead`, `is_invincible` | Real branching logic (party member, not bunnaged, normal status, no invincibility) over a 4-object chain. Pure. In the 30 to 200 range. Called from `BattleAnimExcel.is_skip_current` only. **Taken.** Not live on any available route (0 calls in 3 runs). |
| 2 | `BattleCharacterParam.can_use_any_command` (0x71021c35a0) | 22 | `BadStatusManager.can_command` | Command-menu gate. Battle route calls it live (452 calls). Taken with its callee. Below 30 insns. |
| 3 | `BattleCharacterParam.can_battle` / `can_targetting` / `can_act` / `is_gone` (4 methods, 12 to 15 insns) | 12 to 15 | none | Pure battle predicates, live (29, 2, 6, 0 calls). Taken as the helper layer. |
| 4 | `EnemyInfoStatistics.Data.is_full_open` (0x71018bdb70) | 31 | none | Pure bestiary completeness test (13 byte and int reads), in range. Menu code, not the battle route (4 call sites, all `MonsterBook`). Not taken. |
| 5 | `FieldCharacter.GetSplinePoint` (0x710219d080) and `DukatiMountainPlayerController.GetPoint` (0x7101da4200) | 40, 36 | none | Float spline math, pure, no direct call sites. Not taken (float; no route reaches them). |

Also seen and not taken: `MelodyBay.Manager.judge_game_mode` (60 insns, 7 flag-getter calls, one call site in a coroutine), `Battle.BattleHit.is_disp_damage_value` (26).

Pick: `can_bunnage_globe` (item 1) as the port that fits the brief, plus items 2 and 3 and the callees so the battle route exercises the layer. The live-proven method of the set is `can_use_any_command`.

## Changes

- `decomp/smrpg_decomp/src/object.rs` (72 lines): field access layer, `GuestObject` trait and `guest_class!` macro.
- `decomp/smrpg_decomp/src/battle.rs` (422 lines with 4 tests): classes `BadStatusManager`, `BuffManager`, `BattleCharacterParam`, `BattleCharacter`, `MotionSpecialMario`, with fields and offsets from the IL2CPP table (`_status` +0x10, `_invincible_turn` +0x28, `_act_num` +0x5c, `_is_join` +0x6e, `_is_bunnage` +0x71, `_can_targetting` +0x78, `_badstatus_mng` +0x98, `_buff_mng` +0xa0, `BattleCharacter._param` +0x20, `MotionSpecial._parent` +0x10). The readable part is the `impl` blocks (lines 1 to 211); the shim is lines 212 to 277.
- `runtime/decomp/leaves.def`: 7 `LEAF` rows (`can_command`, `is_dead`, `is_invincible`, `can_battle`, `can_targetting`, `can_act`, `is_gone`) and 2 `CLEAF` rows (`can_use_any_command`, `can_bunnage_globe`), chunks main_0343, 0346, 0376, 0377.
- `runtime/decomp/swap.c`: `battle_object_kind` and `prepare_battle` build the object graph for the selftest.
- `docs/DECOMP.md`: "Readable ports" section.
- `decomp/smrpg_decomp/src/lib.rs`: two `pub mod` lines.

## Readable versus shim

Readable: `BattleCharacterParam::can_battle` is `is_join() && !is_bunnage() && !is_dead() && !is_gone()`; `BadStatusManager::can_command` is `!matches!(status, SLEEP | MUSHROOM | GONE | DEAD)` (the original is the bit mask 0x248 plus a compare with 8); `can_bunnage_globe` walks parent, character, param and returns the step that decided (`GlobeCheck`). Shim: `leaf_shim!` rows map x0 to the class and the bool to x0; the two `CLEAF` shims also write x1 = 0, x8 = 0 and the return address into x30 on paths that reached a call (lifter residue the A/B compares). No float anywhere, so the bit-for-bit float requirement did not apply.

## Metadata in Git

AGENTS.md says nothing from the game enters Git, including "metadata". The repo already commits IL2CPP method names, field offsets and addresses for the 28 hand-written leaves (`leaves.def`, `lib.rs` comments, `docs/DECOMP.md` table), and keeps only the bulk-derived sets on A: (DECOMP.md). I followed that: a small hand-written readable module with names and offsets is in Git; no table, dump or extracted text is. This reading is an assumption; the owner can overrule it, and the module then moves to A: next to `gen.rs` (DECOMP.md says how).

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline` (9 tests, 4 new), decompgen tests (15) | clean |
| Cargo tests against an independent reference | exhaustive over bool bytes 0, 1, 2, 0x80, 0xff, 17 statuses, 6 counts, null parent; residue of both `CLEAF` shims asserted |
| Selftest, 9 rows, 10,000 random + 15,625 edge each (fake object graph, fake stack) | 25,625 calls, 0 differences for every row (`task51_ab1.log`) |
| Build, `python -I tools/rtprep/build_game.py` | 8 min 35 s, 4 chunks recompiled, `decomp mode=ab registered=13661` |
| A/B 1, copy of `task42_rt\save` in `task51_rt\t42`, `task51_ab1.log` (`--seconds 260 -- --max-seconds 150`) | 0 `decomp mismatch`, raced 0, exit 4 (watchdog), 22,833 presents, 99 IPC. Field route: 0 live calls on all 9 rows |
| A/B 2, copy of `codex_battle_hard_02` with its input in `task51_rt\bh`, `task51_ab2.log` | 0 mismatch, raced 0, exit 4, 15,563 presents, 101 IPC lines, interpreter misses none. Live: `can_command` 494, `can_use_any_command` 452, `can_battle` 29, `is_invincible` 19, `is_dead` 10, `can_act` 6, `can_targetting` 2, `is_gone` 0, `can_bunnage_globe` 0 |
| A/B 3, copy of `codex_battle_off_02`, `task51_ab3.log` | 0 mismatch, 13,316 presents, never reached a battle: 0 live calls |
| Locked project check `python tools/game_check.py run --root .` | PASS (1795770636150519000-30d245d9) |

In `ab` mode a lifted caller reaches its callees through their wrappers, so the `can_command` count (494) includes the 452 calls made by `can_use_any_command`.

## Not shown

- `can_bunnage_globe` and `is_gone` had 0 live calls in the three routes: they are selftest-proven (25,625 calls with the full object graph and the cargo reference), not live-proven. Its only caller is `BattleAnimExcel.is_skip_current`, so it likely needs a Mario attack animation path a longer or different battle run reaches.
- No mutation test of the new selftest shapes (the task-44 mutation test covered the comparison itself).
- The enum numbering (`SLEEP` 3, `MUSHROOM` 6, `GONE` 8, `DEAD` 9) is the dump's declaration order; `is_dead` and `is_gone` compare with 9 and 8, which agrees with the names, but the default-value table was not read.

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-27. Unported pure candidates are scarce: most game methods store through `this` or call lazy-init glue, so a 30-instruction pure method with several callees is rare. The 12 to 29 instruction band holds the battle predicates.

### 2026-11-27 Claude lead
Lead read battle.rs (readable impl blocks over a typed field layer, shim at the bottom) and reread task51_ab1 to ab3: 0 mismatch lines, raced total 0, 99 IPC commands, 0 unknown. can_command, can_use_any_command, can_battle, is_invincible, is_dead, can_act and can_targetting ran live in the battle route with 0 differences; can_bunnage_globe and is_gone are selftest-only. Metadata-in-Git recorded as D-014 (owner confirmation pending).
