---
id: task-69
title: Repeatable battle routes (SMRPG_DETERMINISTIC)
status: Review
---

## Diagnosis: where the variation comes from

Checked in the runtime (`runtime/svc.c`, `rt_core.c`, `hle/hle_time.c`, `hle/hle_hid.c`) and the IL2CPP metadata (`analysis/il2cpp/functions.tsv`).

| Source | Finding |
|---|---|
| `svcGetInfo` RandomEntropy (id 11), the seed for `nn::os::GenerateRandomBytes` | already fixed constants in `svc.c`: not a source |
| `svcGetSystemTick`, `GetInfo` ThreadTickCount, the `mrs cntvct_el0` path | all call `rt_cntvct()`, which was the host `QueryPerformanceCounter` (machine uptime). `Environment.TickCount`, `Stopwatch.GetTimestamp` and `DateTime.UtcNow/Now` (4-byte icall stubs in the metadata) reach the guest through this tick and `time:u`; Unity seeds its native RNG from the clock. Source |
| time of day: `time:u/s` `posix_now()`, ClockSnapshot, steady clock | host `time(0)` plus host steady seconds. Source |
| hid sample ticks (`hid_ticks`) | host QPC. Source (input timestamps) |
| presented frames per real second | in a hidden run the game is not frame limited (IMMEDIATE present): about 290 presents per second on an idle machine, about 73 under load (task-68 runs: 23311 presents in 320 s). Routes keyed to presents therefore meet asset loading (real-time threads) at a different point every run. This is the big one for the battle start, not the RNG |
| thread scheduling (45 guest threads) | real host threads, not controlled. Effect measured below: small. Asset load completion shifts by up to about 480 presents between runs and animation phases differ by a few thousand pixels, but battle logic counts did not differ in three runs. Not fixed (would need a deterministic scheduler) |
| guest wait timeouts (`GetTickCount64` in svc.c) | real time; kept (they are waits, not values the game reads) |

The metadata shows no direct `UnityEngine.Random.InitState` or `System.Random(seed)` call from game code in the function list (the game code uses its own battle routines and `Unity.Mathematics.Random`); the seeds come from the clock calls above.

## The option

`SMRPG_DETERMINISTIC=1` (env, default off; with it off nothing changes):

- `rt_cntvct()` (`runtime/rt_core.c`) returns a virtual tick: fixed base (100000 s) plus presented frames times 1/60 s plus a per-thread call count (5 us per call, capped at 3/4 of a frame once frames are running; frame 0, the boot, is uncapped so a loop that waits for the clock still ends). Values never go backwards on a thread. The cap matters: without it the clock ran past the next frame, deltaTime fell to 0 and the game froze (first attempt, run `t69_d1`).
- `posix_now()` (`runtime/hle/hle_time.c`): 2026-01-01 UTC plus virtual seconds, so ClockSnapshot, time:u and steady agree.
- `hid_ticks()` (`runtime/hle/hle_hid.c`): the virtual tick.
- `plat_present_tick()` (`runtime/platform/plat_core.c`): counts frames for the clock and holds presents to 60 per real second (the guest's own limiter no longer waits because its clock is virtual; asset loads run in real time, so the pace must be fixed for load versus frame to be comparable).
- Files: `runtime/rt_core.c`, `runtime/rt_internal.h`, `runtime/hle/hle_time.c`, `runtime/hle/hle_hid.c`, `runtime/platform/plat_core.c`. Thread scheduling is unchanged.

## Evidence

Route `bwf` (A:\AI\Recomps\smrpg-work\task68_rt\bwf\): the task-68 `bw` input with the Croco dialog dismissed by an explicit A at 6560 and 6700 and the battle menu lines moved +1000 presents (in this mode the battle menu is up at about 7200). Run: `SMRPG_DETERMINISTIC=1 bash tools/rtprep/run_route.sh <log> bwf ab 340 320 480`.

Live calls / differences per ported row, three runs (`t69_f1`, `t69_f2`, `t69_f3`; `d` = deferred first call):

| Row | f1 | f2 | f3 |
|---|---|---|---|
| `exec_each_targets` | 12/0 | 12/0 | 12/0 |
| `is_hit_calc_succeed` | 12/0+1d | 12/0+1d | 12/0+1d |
| `exec_calc_type` | 12/0+1d | 12/0+1d | 12/0+1d |
| `skill_calc_exception` | 12/0+1d | 12/0+1d | 12/0+1d |
| `resister_thank_you_message` | 12/0+1d | 12/0+1d | 12/0+1d |
| `damage_calc` | 10/0+1d | 10/0+1d | 10/0+1d |
| `got_physics_attack` | 5/0 | 5/0 | 5/0 |
| `heal_calc` | 1/0+1d | 1/0+1d | 1/0+1d |
| `calc_magic_attack` | 40/0+1d | 40/0+1d | 40/0+1d |
| `calc_attack`, `calc_defence`, `calc_element`, `bd6c0` | 1 each | same | same |
| `set_acction_command_ratio` | 2/0 | 2/0 | 2/0 |
| `calc_renkei_ratio`, `calc_belome_barrior_ratio`, `group_a_b` | 1/0+1d | same | same |
| `can_act`, `can_targetting`, `can_battle` | 4, 4, 28 | same | same |
| `is_barriered`, `is_invincible`, `is_dead` | 20, 28, 8 | same | same |
| `can_use_any_command` (per frame poll) | 3010 | 2962 | 2953 |

All rows are the same in three runs except `can_use_any_command`, a per-frame poll that varies by about 2 percent (load-completion frames differ). Each run: 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none, 18600 presents. Compare task-68 (default mode, same input): 10 versus 8 `exec_each_targets`.

Frame captures every 480 presents are not bit-identical between runs at every sampled frame (up to about 100k differing pixels while the field loads, 0 to 1 pixel at present 9120 and 10080 between two runs, a few thousand later): animation phase follows real-time loads. Do not use frame hashes as the repeatability check; use the live row counts.

Default (option off) run `t69_off` (route `bw`, same build): 0 mismatch, raced 0, IPC 99 / 0 unknown, misses none, 90600 presents in 320 s (about 283 per second, not paced). The old frame-keyed `bw` route does not reach the battle at that speed (machine idle now; the task-68 runs were loaded and slow), which is the load-versus-frame effect above. Faithful behavior is unchanged; this run only shows the counters are clean.

Other runs: `t69_d1`, `t69_d2`, `t69_d3`, `t69_e1` (option on, routes `bw`/`bwd`/`bwe`, earlier clock with no per-frame cap and wrong route timing: invalid for the battle, kept as logs).

## Limits

- Presents-keyed input is only repeatable with the option on (fixed 60 per second); the route times differ from the default-mode route.
- Asset-load completion is still real-time threads; a much more loaded machine could shift the battle start past the scripted presses. Rerun if the battle is missed.
- No deterministic thread scheduler.

## Lead verification

- Diff read: 47 lines in rt_core.c, rt_internal.h, hle_time.c, hle_hid.c, plat_core.c. Every new path is behind `rt_deterministic()`; with the variable unset the clock, time of day, hid ticks and present pacing are the old code.
- Logs t69_f1 to f3 and t69_off: 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none. t69_table.txt rows match across f1 to f3 except the per-frame poll `can_use_any_command`.
- No em dashes or personal details in the diff or this card. game_check rerun by the lead before commit.
