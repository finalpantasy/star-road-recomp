---
id: task-68
title: Dev-profile cargo check fix and a second battle coverage route (party, items, magic)
status: Review
---

## Part A: cargo check (dev profile)

Cause: `decomp/Cargo.toml` set `panic = "abort"` only in `[profile.release]`. The crate is `#![no_std]` with its own `panic_handler`; the dev profile defaulted to unwinding, which `core` without `std` cannot do. Fix: `[profile.dev] panic = "abort"`. The release profile is untouched; cargo ignores `panic` for the test profile, so `cargo test` still builds with unwinding.

## Part B: route

Files (A: only, no game data in Git): `A:\AI\Recomps\smrpg-work\task68_rt\bw\` (`save\`, `options.ini`, `input.txt`, `input.final.txt`); exploration dirs `src`, `e1`, `e2`, `kp` (Keep Terrapin, Mario alone, empty bag: no items), `oldbh` (old route). In Git: `tools/rtprep/run_route.sh` (copies the save fresh per run and calls `run_game.py`), `tools/rtprep/live_rows.py` (per-row live table with names from `leaves.def`). Route description and run lines: docs/DECOMP.md, "Battle coverage route 2".

The route: Bandit's Way autosave (Mario 25 HP, Mallow 20 HP, 13 FP, Mushroom and Honey Syrup), Mario uses Mushroom (heals through `heal_calc`), Mallow casts Thunderbolt (magic attack), second Mushroom and Thunderbolt, then in the follow-up fight Mario Defends and Mallow casts Thunderbolt. The route finds its battle by waiting for the field enemies, so it is not repeatable (see below).

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check`, `cargo check --release`, `cargo fmt --check`, `cargo test` (in `decomp/`) | all pass; 101 tests passed |
| Two A/B runs of the new route (`t68_ab1`, `t68_ab2`, same input, 320 s, max 480 presents capture, hidden, muted) | both: 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none, no named row with a difference. Live counts differ between the runs (table below): the battle is not repeatable |
| A/B run of the old route (`t68_old`, `oldbh`) | 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none; `exec_each_targets` 6 calls (it was 15 in t67_fin_ab: that route varies too) |
| `python tools/game_check.py run --root .` | PASS after the last edit (see the lead line at the end if different) |

Live calls / differences per ported battle row (`d` = deferred first call, not compared):

| Row | ab1 | ab2 | old route |
|---|---|---|---|
| `exec_each_targets` | 10/0 | 8/0 | 6/0 |
| `is_hit_calc_succeed` | 10/0+1d | 8/0+1d | 6/0+1d |
| `exec_calc_type` | 10/0+2d | 8/0+2d | 6/0+1d |
| `skill_calc_exception` | 10/0+1d | 8/0+1d | 6/0+1d |
| `resister_thank_you_message` | 10/0+1d | 8/0+1d | 6/0+1d |
| `damage_calc` | 8/0+1d | 7/0+1d | 6/0+1d |
| `got_physics_attack` | 4/0 | 3/0 | 4/0 |
| `heal_calc` | 1/0+1d | 1/0+1d | 0 |
| `calc_magic_attack` | 40/0+1d | 30/0+1d | 21/0+1d |
| `calc_attack`, `calc_defence`, `calc_element` | 1/0 each (+1d for attack) | same | same |
| `calc_renkei_ratio`, `calc_belome_barrior_ratio`, `group_a_b` | 1/0+1d | 1/0+1d | 1/0+1d |
| `set_acction_command_ratio` | 2/0 | 2/0 | 1/0 |
| `is_barriered` | 15/0 | 13/0 | 11/0 |
| `is_invincible`, `is_dead` | 21, 8 | 19, 6 | see log |
| `can_use_any_command` | 1247/0 | 1599/0 | see log |
| `calc_magic_defence` | 0 | 0 | 0 |
| `use_damage_guard` | 0 | 0 | 0 |
| nine `exec_koudou_kouka` rows (0x71021be0f0, ba6d0, ba6a0, be1f0, bd6c0, be2f0, c3c00, c68c0, bf040) | all 0 | all 0 | all 0 |

Earlier runs of the same input (`t68_bw2`, `t68_bw3`, 10 to 12 `exec_each_targets` calls, 0 mismatch) reached `heal_calc` 1 and 2 and `bd6c0` (0x71021bd6c0) 1 and 2 live calls with 0 differences: that row is reached by a Mushroom or Defend that happens to land. The 0x650 branch of `exec_each_targets` was not reached (the damage_calc target tally showed `other` only).

## Rows still at 0 and what reaches them

- `heal_calc`: reached (1 live per run, plus the deferred first call). More live calls need more Mushrooms used.
- `calc_magic_defence`, `use_damage_guard`, `calc_defence` for magic: `damage_calc` calls them Rust to Rust; the counter sees them only on the deferred first `damage_calc` of the process. They get a live count if the very first damage of the run is a magic hit (calc_magic_defence) or hits a barriered target (use_damage_guard: a Barrier or Safety type buff). A route whose first action is Mallow's Thunderbolt while no earlier skill runs damage_calc (Mario's first action must be a non-skill, which does not exist: Defend and Item also go through `exec_each_targets`), or an enemy magic skill first, is needed. A cleaner fix is a debug counter inside the Rust path (not done here: no port or swap.c change).
- the `exec_koudou_kouka` leaves (except `bd6c0` seen once): need skills that reach the buff arms: a buff or status skill (Mallow or Geno support, Safety or Shield style items, a status spell); Defend alone did not reach them.
- `skill_calc_exception` 0x67 and 0x25d arms: skill ids 0x67 and 0x25d, not identified here; 0x25d also needs an acter other than character 0x657. The exception arm for character 0x5ed needs that enemy (a battle-state value 2 in `BattleManager`).
- 0x650 branch of `exec_each_targets`: a battle with character 0x650 as a target (an enemy encounter that has it).

## Notes

- Keep Terrapin route (`kp`): Mario alone, Item menu shows "Nothing in inventory", 3 `exec_each_targets` calls; not used for the table.
- Nothing from the game, no commit.

## Lead verification (2026-11-28)
- t68_ab1, t68_ab2, t68_old: 0 `decomp mismatch`, raced total=0, 0 unknown IPC, interpreter misses none, no live row with differences. heal_calc 1 live call (0 differences) in each new-route run; 0 on the old route.
- decomp/Cargo.toml `[profile.dev] panic = "abort"`: dev `cargo check` now passes.
- game_check PASS (slow, about 12 minutes, while other projects loaded the machine).
- Open: the route is not repeatable (random encounters and turn order); a seeded-RNG option would make it so.
