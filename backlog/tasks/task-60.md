---
id: task-60
title: damage_calc driver ported and compared (four character ids run lifted)
status: Review
---

## Observed behavior

Task-59 left the `Battle.SkillExecter.damage_calc` driver (0x71021ff580, 613 instructions) lifted. This task ports it in `decomp/smrpg_decomp/src/battle/damage.rs` with a register-exact comparison against the lifted body.

## What was built

- `damage.rs`: `SkillExecter.damage_calc` with IL2CPP names. The first-call metadata (26 lazy slots), the debug lines (`NumberLine` and `LiteralLine` tables), the class checks with polls, the formula steps (calc_attack, calc_defence, magic variants, calc_element, belome ratio, renkei ratio, `group_a_b`, action command ratio, guard, encounter 0x819 times 4 for skill 0x2bd, fixed damage 0x50 for character 0x64f with skill 0x2e7, the `id >= 1000` no-damage branch). Already ported callees are called directly; the rest go through the bridge. Shim `smrpg_skill_damage_calc`, hook `smrpg_skill_damage_calc_pre`.
- `leaves.def`: 0x71021ff580 is a `QLEAF` (was lifted only).
- `swap.c`: replay journal-first pre-reads, `smrpg_guest_exc`, `smrpg_guest_lifted`, `smrpg_guest_forget_stores`, re-journaling of an out callee's post bytes, PRE_MAX 96, first-mismatch call listing, selftest graph for the driver and `restore_driver_globals`.
- `bridge.rs`, `frame.rs`: `run_lifted`, `exception_pending`, `Frame::call_port` (nested replay rows share the ctx), `forget_stores`, v2 reload. `belome.rs`: five items `pub(crate)`.
- `tests.rs`: `driver_metadata_returns_step_by_twelve` (47 tests).

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 47 passed |
| `build_game.py` | built (final build after the mutations were reverted) |
| Selftest `t60_fin` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`, battle route copy) | driver 25,625 calls, 0 differences; 0 `decomp mismatch`; raced total 0; IPC 99 / 0 unknown; interpreter misses none; 14,928 presents; live `damage_calc` 15 calls, 0 differences, 5 deferred (lazy init pending) |
| A/B battle route `t60_bh` (copy in `task60_rt`, 200 s, max 120) | 0 mismatch, raced 0, IPC 0 unknown, misses none, 9,581 presents, live 15 calls, 0 differences, 5 deferred |
| A/B task42 save `t60_t42` | 0 mismatch, raced 0, IPC 0 unknown, misses none, 23,829 presents, but 0 `damage_calc` calls: that save does not reach a battle in 120 s, so this run proves only that nothing broke |
| `on` mode battle `t60_on` | 0 mismatch, IPC 0 unknown, misses none, 9,542 presents, `decomp on addr=0x71021ff580 calls=15` |
| Mutation 1 (encounter multiplier `<< 2` to `<< 3`) | 23 `decomp mismatch` lines in a selftest run (first: `call_mismatch=1`) |
| Mutation 2 (`FIXED_DAMAGE` 0x50 to 0x51) | mismatch lines in a selftest run (first at 0.945 s, `call_mismatch=1`) |
| `python tools/game_check.py run --root .` | PASS |

Both mutations were reverted (`cmp` with the saved good copy identical, rebuilt, `t60_fin` ran after).

## Limits

- Characters 0x5dd, 0x5ec, 0x615 and 0x653 run lifted only (the list-clear and BattleManager paths are not ported): entry `run_lifted` in `on` mode, deferred in `ab`.
- The first call per process (any lazy-init flag clear) is deferred, not compared. The hook also defers when renkei could initialize the Math or BattleManager class or the static flag byte, and for skill 0x2c3 on Belome (renkei replaces the logger's list, so the Belome list version cannot be compared).
- Stores in nested ports' dead stack frames are not compared (`forget_stores`).
- Live coverage is 15 route calls; the other paths are covered by the 25,625 selftest cases only. The task42 save contributed no live calls.
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- Logs t60_bh and t60_fin: 0 `decomp mismatch`, raced total=0, 99 IPC commands with 0 unknown, interpreter misses none, damage_calc calls=15 with 5 deferred. t60_t42: clean, 0 damage_calc calls (route never reached a battle). t60_on: damage_calc calls=15.
- Mutations: t60_mut1 and t60_mut2 each logged 23 mismatches at 0x71021ff580 (call_mismatch at different timestamps); the final run used the reverted build.
- `cargo test`: 47 passed. game_check PASS.
- Open: characters 0x5dd, 0x5ec, 0x615 and 0x653 still run lifted; first call per process deferred.
