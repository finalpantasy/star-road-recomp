---
id: task-62
title: exec_each_targets (the caller of damage_calc) ported
status: Review
---

## Observed behavior

Task-60 and task-61 ported the `damage_calc` driver (0x71021ff580). Its only direct caller in the lifted code is `Battle.SkillExecter.exec_each_targets`, **0x71021fe920** (42 instructions, 168 bytes, chunk main_0378; called from main_0321). Chosen because it is the smallest and only caller; it runs live in the battle route (12 to 15 calls per run).

## What was built

- `decomp/smrpg_decomp/src/battle/targets.rs` (new): `SkillExecter.exec_each_targets` as a `QLEAF` (shim `smrpg_skill_exec_each_targets`, hook `smrpg_skill_exec_each_targets_pre`). Calls `is_hit_calc_succeed`, stores `_tmp_value = 0`, calls `exec_calc_type`, `skill_calc_exception`, `exec_koudou_kouka`, then `damage_calc` (flag +0x44), `heal_calc` (flag +0x45), `got_physics_attack(false)` (flag +0x46 and logger byte +0x46), stores byte +0x74 = 1 when the target's character id is 0x650, and tail-calls `resister_thank_you_message`. All callees are bridged (`Frame::call`, `Regs::invoke`); the register flushes follow the lifted C. Three unit tests (50 total).
- `bridge.rs`: `prestate::snapshot_timeline`. `battle.rs`: `pub mod targets`.
- `runtime/decomp/leaves.def`: the QLEAF plus PLEAFs for 0x71021fee50, 0x71021f8040, 0x71021f6340, 0x7102200060 and 0x71021bdef0.
- `runtime/decomp/swap.c`: nested replay call for `damage_calc` inside a recorded body (`run_nested_replay`, second call buffer, journal save and restore, `smrpg_guest_bl` accepts it); timeline cells (watch 2) with a per-call capture and a store index in the journal; selftest graph for the row in `prepare_caller`; the live by-target counter also counts inside the recorded body. Details in `docs/DECOMP.md` ("exec_each_targets (task-62)").

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo fmt --check`, `cargo check --release`, `cargo test --release --locked --offline` (in `decomp/`) | clean; 50 passed |
| `build_game.py` | built; final build after both mutations were reverted (`cmp` with the saved good copy identical) |
| A/B route `t62_bh2` (fresh copy of the battle route in `task62_rt\bh`, own save, 200 s, max 120, final build) | 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none, `exec_each_targets` 12 calls, 0 differences; `damage_calc` 12 calls, 0 differences, 1 deferred. Earlier runs of the same code, `t62_bh` and `t62_fin`, gave 15 calls each, 0 differences |
| Selftest `t62_fin` (`SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1`, `--seconds 340 -- --max-seconds 150`) | `exec_each_targets` 25,625 calls, 0 differences; `damage_calc` 25,625, 0 differences; no selftest line with differences; 0 `decomp mismatch`; raced 0; IPC 99 / 0 unknown; misses none; live 15 and 15 calls, 0 differences |
| `on` mode `t62_on2` (260 s, max 150) | 0 mismatch, IPC 99 / 0 unknown, misses none, `decomp on addr=0x71021fe920 calls=15`, `addr=0x71021ff580 calls=14` (a call with the +0x44 flag clear skips damage_calc) |
| Mutation 1 (`CHARACTER_WITH_FLAG` 0x650 to 0x651) | 2,837 mismatch lines in the selftest (`call_mismatch=1`) |
| Mutation 2 (`_tmp_value` store 0 to 1) | selftest: 12,634 mismatch lines (store journal: `port stored 0x1 ..., memory has 0`); route run `t62_mut2_live`: 15 mismatch lines from the timeline check (`port stored 0x1 ... before call 1, the cell held 0 there`) |
| `python tools/game_check.py run --root .` | PASS |

## Limits

- The port reads the flag bytes, the target and the logger live after the lifted body ran; a callee that rewrote one after the call that read it would show as a mismatch (none did).
- The 0x650 branch (store +0x74) was not reached by the route; selftest only. The timeline check covers `_tmp_value` live.
- In `ab` mode the nested `damage_calc` comparison runs inside the recording of this row (own buffer and journal); other replay rows reached inside the recorded callees are not compared live, as before.
- Names of the flag fields at +0x44, +0x45, +0x46 and +0x74 are unknown from the metadata; named by what they gate.
- Still lifted: the seven callees of this row (`is_hit_calc_succeed`, `exec_calc_type`, `skill_calc_exception`, `exec_koudou_kouka`, `heal_calc`, `got_physics_attack`, `resister_thank_you_message`) and this row's own caller in main_0321 (not examined).
- Appearance and feel are not approved by this task.

## Lead verification (2026-11-27)
- t62_bh2 and t62_fin: 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none; exec_each_targets 12 and 15 live calls, damage_calc 12 and 15, all 0 differences.
- Selftest exec_each_targets 25,625 calls, 0 differences. Mutations: 2,837 and 12,634 selftest differences; t62_mut2_live 15 mismatch lines.
- t62_on2: exec_each_targets 15 and damage_calc 14 calls in `on` mode.
- `cargo test`: 50 passed. game_check PASS (one earlier attempt hit `sync: process exit 1`, the rerun passed).
