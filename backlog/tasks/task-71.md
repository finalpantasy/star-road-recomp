---
id: task-71
title: Route variance under SMRPG_DETERMINISTIC (12 versus 16 exec_each_targets calls)
status: Review
depends: task-69, task-70
---

## Goal

With `SMRPG_DETERMINISTIC=1`, route `bwf` still gives two outcomes (task-70: 12 or 16 `exec_each_targets` calls). Find where two runs first diverge and make the route repeatable, behind the same option (default unchanged).

## Acceptance

- Diagnosis in the card: the first divergence point (present number, thread, cause), with log evidence.
- Fix behind `SMRPG_DETERMINISTIC` (or a route-side fix such as state-keyed input), default off path unchanged.
- Five `ab` runs of `bwf`: identical per-row live counts (except per-frame polls), each 0 `decomp mismatch`, raced 0, 0 unknown IPC, interpreter misses none. One option-off run with clean counters.
- `python tools/game_check.py run --root .` PASS. DECOMP.md note updated.

## Diagnosis

Traced with `SMRPG_DET_TRACE=1` (logging only: first 48 calls of every row with the present frame, per-thread virtual clock calls per frame, the frame in `hid read` lines). Two option-on runs (`t71_tr_a`, `t71_tr_b`), same input:

- Scripted input is identical in both: every button change was read at the same frame (A at 1200, 1350, ..., 6560, 6700, 7500, ...). Input is not the source.
- The battle starts at frame 6704 in both. The first divergence is inside it: `exec_each_targets` call 3 at frame 6813 in a, at 6704 in b; a has a call at 6941 that b lacks, so `damage_calc` 9 versus 10. Later runs `t71_tr_c` and `t71_tr_d` both reached 16 calls but the battle started at frame 6704 versus 6703, so the phase differs even when the count matches.
- Cause: the frame counter (virtual clock, scripted input) advanced in `plat_present_tick` on the render thread T30. The main thread T2 polls `GetNpadStates` once per logic frame (302 polls for 301 presents, traced), and the present lands within about a millisecond of the poll in either order (log lines show present before and after the poll, and two presents between two polls). T2 therefore saw frame k or k+1 at the same point of its logic: deltaTime and the tick it reads (and so Time.time driven animation and anything seeded from it) differ run to run. T2's clock-call count per frame differed in 6569 of 18638 frames between c and d; frame 0 (boot, uncapped) differed by 2 calls on T2 (13259 versus 13257), a smaller second source, not changed here.

## Fix (behind SMRPG_DETERMINISTIC, default off unchanged)

`plat_det_poll()` in `runtime/platform/plat_core.c`, called from the `GetNpadStates` wrapper in `runtime/hle_nn.c`: the frame counter, virtual clock and scripted pad state advance on the main thread's own poll, and the pad sample is published synchronously there. The 5 ms asynchronous publisher is skipped in this mode. `plat_present_tick` keeps only the 60 per second pacing and advances the frame only after 500 ms without a poll (logged; none in the final runs). Diagnosis logging in `runtime/rt_core.c`, `runtime/decomp/swap.c` (`leaf_called`), `runtime/hle_nn.c`, all behind `SMRPG_DET_TRACE`.

## Evidence

Route `bwf`, `SMRPG_DETERMINISTIC=1 bash tools/rtprep/run_route.sh <log> bwf ab 340 320`, logs `t71_fin_ab1..6` in `A:\AI\Recomps\smrpg-workuild\logs`:

| Check | Result |
|---|---|
| Named battle rows, six runs | identical every run: `exec_each_targets` 16, `damage_calc` 14+1d, `exec_calc_type` 16+1d, `exec_koudou_kouka` 16+2d, `is_hit_calc_succeed` 16+1d, `calc_exception` 16+1d, `resister_thank_you_message` 16+1d, `heal_calc` 1, every `param_*` row, `hit_exec_hit` 18 (before the fix 12 or 16) |
| `decomp mismatch`, raced total, IPC, interpreter misses (six runs) | 0, 0, 99 distinct 0 unknown, none |
| Per-frame polls and hot generic helpers (`can_use_any_command`, `badstatus_can_command`, `camera_adjust_camera_transform`, `ptr_eq`, `array_elem_addr`, `zero_12` ...) | ab2 to ab6 equal on the poll rows (3943, 3973, 9364); the generic helpers vary by about 0.1 percent (idle spin loops). ab1, the first run after the rebuild, is an outlier on the poll rows only (`can_use_any_command` 3223, `camera` 8644, 720 frames fewer); its battle-logic rows equal the others. Cause of that one not found (cold cache is a guess) |
| Option off (`t71_off1`, `SMRPG_DECOMP=ab`, no option) | 0 mismatch, raced 0, 99 IPC 0 unknown, interpreter misses none |
| Final binary (weak `plat_*` externs so the mock-game lifter test links; trace line dropped), `t71_fin_ab7` | same rows as ab2 (16, 14+1d, 3943), 0 mismatch, raced 0, 0 unknown IPC, no misses |
| `python tools/game_check.py run --root .` | PASS (`1795864845877091000-06be6eab`, run after the final edit; the first run failed in the lifter mock-game link on the new `plat_*` references, fixed with weak externs) |

## Lead verification

- Diff read: every behavior change is behind `rt_deterministic()`. Default mode changes only the `hid read` log line (frame number added; no tool parses it) and the per-call trace check, which is off unless `SMRPG_DET_TRACE=1`.
- Logs t71_fin_ab1 to ab7 and t71_off1: each 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none.
- Open: the ab1 per-frame poll outlier and the boot-frame residue are recorded above, not fixed. No em dashes or personal details. game_check rerun by the lead before commit.
