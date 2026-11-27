---
id: task-48
title: Hidden default-mode runs that crawl on the same exe and save
status: Review
---

## Observed behavior

Hidden default-mode runs of the same exe on fresh copies of the same save sometimes crawl: `task46_on.log` and `task47_on.log` show 97 distinct IPC commands, no interpreter miss and 925 to 4,200 presents in 120 s. `task46_on2.log` and `task47_on2.log` show 99 commands, one interpreter miss (0x71012a0ed4) and 27,000+ presents. The earlier note called this "a slow machine run".

## What the slow runs are (measured)

- Not a code or save difference. The 97 vs 99 commands are the same two calls: `nn::am::ISelfController` cmd 1 and 2 (`EnterExitRequestHandlingSection` / `LeaveExitRequestHandlingSection`), made only when the game reaches its field and autosaves. The slow runs never get there in 120 s, so they also never reach the function that causes the 0x71012a0ed4 miss. No missing service, retry or timeout shows in the logs.
- The whole game runs at a fraction of speed while the process uses about 20 percent of total CPU. In `task47_on.log` the "frame" in the platform pad line was 87 at 21 s, 133 at 31 s, 183 at 41 s (about 5 presents per second); in a normal run (`task48_r3`) it was 5,877 at 20 s. The 200 per second pad tick stays correct (3,950 to 5,951 samples per 10 s), so Windows timer resolution is not the cause (`plat_core.c` already sets `IGNORE_TIMER_RESOLUTION` and `timeBeginPeriod(1)`).
- The intro movie decodes with `0 decode errors` but at 5 to 10 pictures per second instead of 60: first movie ends after 153 to 507 packets (slow) instead of 935 to 971 (normal). The first Opus decode happens at 46.6 s in `task47_on` and 23.3 s in `task47_on2`, and guest thread 62 never starts in the slow run.

## Cause (probable, strongly correlated, not proven by an isolated test)

Another process was using the same GPU. A sampler (`\GPU Engine(*engtype_3D)\Utilization Percentage` per pid, every 2 s, plus total CPU) ran next to three runs:

| Run | Result | Other 3D GPU users during the run |
|---|---|---|
| `task48_r6` | slow: 1,800 presents, 97 commands, first movie 507 packets | two Blender processes at 25 to 60 percent for 90 s, a python process at 14 to 90 percent, Antigravity at 2 to 50 percent |
| `task48_r7` | normal: 21,000 presents, 99 commands, first movie 971 packets | one python process at 50 to 86 percent for part of the run, Blender 7 percent once |
| `task48_r8` | mostly normal: 15,600 presents, 99 commands, first movie 954 packets (movie phase clean) | Blender 20 to 70 percent from 25 s on (after the movie), python up to 86 percent; fewer frames later (684,803 draws vs 3 million) |

Windows time-slices one 3D engine between processes. A game that submits and presents every few milliseconds and waits on a fence is the first to lose when a renderer holds the engine, and the slow-motion state matches (presents about 200 ms apart, CPU idle, no errors). Other leads (Blender renders in this repo family, Codex NVN replay runs, a python GPU job) all run on the one RTX 4090. The slow runs of task-46 and task-47 happened while the Codex lead was running NVN work (to-codex.md updated 01:55) but the exact other process was not recorded then.

What this rules out (checked): startup path (a run with a 4.5 s stall before window creation, `task48_r4`, was a separate disk and CPU stall while an unrelated `find` of mine was scanning the work tree on A:; its movie then ran at about 20 fps and ended at 81 s), Rust swaps (not needed: runs `task48_r1` to `task48_r3` and `task48_r7` were normal, the slow ones ran the same exe; `SMRPG_DECOMP=off` was not tried because the cause lies outside the swaps), save differences (every run used a fresh copy of the same save).

## Runs (all default mode, 120 s, hidden, muted, fresh copy of `task42_rt\save` in `task48_rt\N\`, logs `task48_rN.log` in `build\logs\`)

| Run | Presents | IPC | Miss | Movie 1 packets | Verdict |
|---|---|---|---|---|---|
| r1 | 27,000 | 99 | 0x71012a0ed4 | 954 | normal |
| r2 | 28,320 | 99 | yes | 935 | normal |
| r3 | 26,880 | 99 | yes | 939 | normal |
| r4 | 12,000 at 81 s | 99 | yes | 952 (ended 81 s) | slow phase, disk and CPU stall from my own find |
| r5 | 840 | 97 | none | 170 | slow, normal startup, GPU not sampled |
| r6 | 1,800 | 97 | none | 507 | slow, Blender on GPU |
| r7 | 21,000 | 99 | yes | 971 | normal |
| r8 | 15,600 | 99 | yes | 954 | normal movie, slower later, Blender on GPU |

Slow state reproduced 3 times in 8 runs (r4 partial, r5, r6) with no code change between them.

## Fix

None. No code or runtime change; the cause is a shared GPU, outside `runtime/nvn/` and not fixable in the game.

## Recommendation

Before trusting a default-mode run for frame count, check that no other process is using the 3D engine (`\GPU Engine(*engtype_3D)\Utilization Percentage` per pid, or `nvidia-smi`), or treat a run with 97 IPC commands and under 5,000 presents as invalid and rerun it. A cheap tooling follow-up (not done here): have `tools/rtprep/run_game.py` print the other GPU users before it launches and flag the result "GPU shared". A controlled test (start a Blender render on purpose beside a clean run) would turn "probable" into "proven"; it was left out because of the 8-run cap and the one-heavy-process rule.

## Log

- 2026-11-27. Compared the four logs, then 8 hidden runs; found the slow state is a global slowdown that correlates with other 3D GPU users. No code changed, no game_check run needed.

### 2026-11-27 Claude lead
Lead added a GPU check to tools/rtprep/run_game.py: it samples nvidia-smi GPU use before and after each run, prints both, and warns when the GPU is already at 30 percent or more. Rule from this task: a 120 s default-mode run with 97 IPC commands and under 5,000 presents is invalid and gets rerun. A controlled proof (a deliberate GPU load beside a clean run) is still open.
