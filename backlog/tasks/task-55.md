---
id: task-55
title: get_calc_attack and get_calc_magic_attack ported readably with a pre-state mechanism, calc_element ported, damage_calc analysed
status: Review
---

## Observed behavior

Task-54 skipped `get_calc_attack` because the lifted body begins with IL2CPP lazy initialization (a flag byte, two metadata calls, class-initialized words, a static-field flag) and two GC safepoints. A replay port runs after the lifted body, when the flag bytes and class words already changed, so it cannot tell which calls the lifted body made. Re-running a callee on live memory is not allowed. `SkillExecter.damage_calc` (613 instructions) was left for later.

## What was built

- Pre-state mechanism (`runtime/decomp/swap.c`, `decomp/smrpg_decomp/src/bridge.rs`): row kind `QLEAF(chunk, addr, port, hook)`, a pre hook that snapshots cells before the lifted body runs, `prestate::read_u8/read_u32` (snapshot value inside a trial, live value outside), `prestate::store_u8` (a journaled store in a trial, checked against memory afterwards), deferral of the call while a lazy initialization is still pending (counted as `deferred`, not compared), and `Regs::safepoint_n` for safepoints that flush several registers. Design and limits in `docs/DECOMP.md` ("Pre-state and store journal (task-55)").
- Ports, `decomp/smrpg_decomp/src/battle/attack.rs`: `BattleCharacterParam.get_calc_attack` 0x71021c3ea0 and `get_calc_magic_attack` 0x71021bc2e0 (`QLEAF`), with IL2CPP names and offsets (`_attack`, `_magic_attack`, `_buff_mng._add_status`, `_weapon`, `_is_player`, `BattleManager._instance._encounter_id` 2076, candle bonus) and the lifted lazy initialization spelled out.
- Port, `battle/element.rs`: `BattleCharacterParam.calc_element` 0x71021be3c0 (`ELEAF`, 21 instructions).
- Traced rows (`PLEAF`, lifted body unchanged) for the callees and the damage_calc callees: 0x7101797f10, 0x71021c43d0, 0x71000c4150, 0x71000c4330, 0x7101bf7a10, 0x71021f5ed0, 0x71021f7ea0, 0x71021f5c50, 0x7102201aa0, 0x71021c3bb0, 0x71021be280.
- Selftest support: the attack rows read real game globals, so the selftest points three metadata slots and two flag bytes at a private region for the row and restores them.
- Build fix (`runtime/decomp/game.cmake`): the Rust sources were globbed as `src/*.rs`, so an edit under `src/battle/` did not rebuild the archive (found when a mutation build finished in 18 s with no effect). Now `GLOB_RECURSE`.
- Test fix (`battle.rs` tests): the task-51 `Graph` held its array by value while its own pointers pointed into it, so the test `shims_leave_the_register_residue_of_the_lifted_code` failed at HEAD with the current compiler output (checked in a clean worktree: 32 passed, 1 failed). The array is now boxed.

## damage_calc: partly done

Not ported. Reasons, the call-site breakdown and the order of what comes next are in `docs/DECOMP.md` ("damage_calc (task-55)"). Coherent finished part: the two attack stat methods, `calc_element` (the multiplier it applies) and the machinery; its callees have live counts below. Remaining: the driver itself (about 90 call sites, 613 instructions), then `set_acction_command_ratio`, `calc_belome_barrior_ratio`, `calc_renkei_ratio`, `group_a_b`.

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test` | clean (plain `cargo check` without `--release` fails the same way at HEAD: no_std with unwinding panics); 35 tests (2 new: attack against a label-state model of the lifted control flow over more than 10,000 combinations, element); no new warnings |
| `python -I runtime/decomp/check_pure.py` | 0 failures |
| `python -I tools/rtprep/build_game.py` | build 1 (new rows, 2 new chunks) 8 min 32 s. The Vulkan SDK folder `A:\AI\Recomps\new-pokemon-snap\tools\vulkan_sdk` (named in the build cache for the NVN layer) was emptied by something else at 09:47; the mutation builds pointed the cache at a stand-in under `A:\AI\Recomps\smrpg-work\vulkan_sdk_local` (junctions to the MSYS Vulkan and SPIR-V headers, MSYS `glslc.exe`). That forced one full NVN rebuild, 10 min 6 s (6 s over the limit, not decomp work). Later rebuilds 27 s to 2 min 7 s |
| Selftest run, copy of `codex_battle_hard_02` in `task55_rt\bh`, `task55_selftest` (`--seconds 340 -- --max-seconds 150`) | selftest rows: attack, magic attack and element each 10,000 random + 15,625 edge, 25,625 calls, 0 differences; 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 12,965 presents; `check_log.py --summary`: "selftest clean for 13669 rows; live differences: 0; raced loads (retry agreed): 0" |
| A/B on the same copy, `task55_bh5` and the final build `task55_final_bh` | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none; 12,523 and 10,149 presents |
| A/B, copy of the `task42_rt` save in `task55_rt\t42`, `task55_t42` | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 25,978 presents; battle rows 0 live calls (field route), 0x71000c4150 12,577 calls, 0x71000c4330 291 calls, 0 differences |
| `python tools/game_check.py run --root .` | PASS (1795793436365290400-2d809897) |

Live counts on the battle route (`task55_bh5`, 150 s of game; `task55_final_bh` gave the same counts except `Random.Range` 305 and the others identical): `get_calc_attack` 15 calls, 0 differences, 1 deferred; `get_calc_magic_attack` 48 calls, 0 differences, 1 deferred; `calc_element` 15; `Random.Range` 375; `magic_attack_up_ratio` 16; `initialize_runtime_metadata` 16,016; class-init call 0x71000c4330 319; `calc_renkei_ratio` 15; `set_acction_command_ratio` 15; `calc_belome_barrior_ratio` 15; `group_a_b` 15; `is_barriered` 15; `damage_calc` 15; `get_on_candle_num` 0; `use_damage_guard` 0. All with 0 differences.

Runs that were discarded or did not reach battle: `task55_bh` (4,623 presents, invalid, rerun); `task55_bh2`, `_bh3`, `_bh4` had 18,000 to 20,700 presents but never reached the battle (0 live attack calls), because another project's compile (34 `clang++` processes, 100 percent CPU for about 40 minutes) slowed the game's start so the recorded inputs, which are frame-indexed, no longer lined up; `task55_mut6` had 3,214 presents. The counts quoted above come from runs after the machine was free.

## Mutation tests (all reverted)

| Mutation | Result |
|---|---|
| `calc_element`: default multiplier 1.0 changed to 0.5 | Live A/B, `task55_mut3`: 13 of 13 calls mismatched ("replay: recorded=0 used=0 call_mismatch=0"), attack rows 0 differences |
| Attack stat code shared by both attack methods: `sum = base + added + 1` | Live A/B, `task55_mut8`: `get_calc_attack` 7 of 8 compared calls (1 deferred) and `get_calc_magic_attack` 29 of 30 mismatched; 36 `decomp mismatch` lines. Earlier runs `task55_mut4` (magic 2 of 3) agree |
| Attack stat code: `_add_status` index 0 changed to 1 and 2 to 3 | Not caught live (`task55_mut2`, 0 differences: no buffs on the route, every `_add_status` entry is 0). Caught by the selftest (`task55_mut_self`: `get_calc_magic_attack` 14,388 differences of 25,625; the run stops at the first failing row) |

The first mutation build (`task55_mut`, `task55_mut2`) built nothing because of the glob bug above, so those runs show 0 differences with the unmutated code; the findings come from the builds after the fix. `attack.rs` and `element.rs` were restored from saved copies (`cmp` identical) and the final build and `task55_final_bh` came after the reverts.

## Not shown

- The candle path (encounter 2076, `get_on_candle_num`: 0 live calls) is covered by the selftest and `cargo test` only. Whether the live route exercises the player-weapon path (`Random.Range` inside `get_calc_attack`) was not separated from the other `Random.Range` callers; the selftest and `cargo test` cover it.
- The first call of each attack method in a process is deferred, not compared, by design. The port's assumption that a class initializer sets its word on the first call is checked by `cargo test` and the selftest, not by a live first call.
- `damage_calc` and its out-parameter callees are not ported; their traced rows show only that the lifted code is called as recorded.
- Replay shows the port matches the lifted body given the callees' recorded results; it does not show Unity natives or the buff ratio methods are right (unchanged lifted code).
- No visual or timing change: nothing is changed in `on` mode behavior beyond the ports themselves; `on` mode itself was not run in this task.

## Dependencies and ownership

Task-44, task-51, task-52, task-53, task-54. Owns `decomp/smrpg_decomp`, `runtime/decomp/`, `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.

## Lead verification (2026-11-27)
- Reread task55_final_bh: 0 `decomp mismatch` lines, `decomp raced total=0`, 99 IPC commands with 0 unknown, interpreter misses none. Mutation log task55_mut7 shows 36 mismatch lines.
- The Vulkan SDK folder was emptied at 09:47 outside this project; the smrpg texture cleanup ended at 09:11 and touched only hd-texture-test subfolders. The stand-in path lives only in the local build cache on A:, not in Git.
- Diff scanned for em dashes and personal details: none.
