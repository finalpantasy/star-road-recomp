---
id: task-13
title: Horizon HLE services: sm, fs, os, time, settings, applet, hid, audio
status: Planned
---
## Problem before solution
After task-5 the boot stops at the first service request. Unity needs these services to reach level0.
## Outcome and scope
`runtime/hle/` (our own): service manager, fs (RomFS from the owner's extraction, save data), os sync and threads, time, settings, applet and account stubs, hid, audout. Each service logs unknown commands.
## Dependencies and ownership
Depends on task-5 (Verified). Owns `runtime/hle/`.
Execution: subchat, Sonnet high; split per service if it helps.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Boot passes every service call up to level0 load with 0 unknown commands on the path | Run | Log | NOT_RUN |
| AC2 | Memory-ordering and condvar tests pass before the first threaded boot | Test | Report | NOT_RUN |
## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Generated output and caches on A: only. Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-008: our own code; Snap (A:\AI\Recomps
ew-pokemon-snap) and Ryubing (A:\AI\Recomps
ew-pokemon-snap	oolsefyubing-ryujinx) may be read for lessons and run as test oracles only, never copied.
## Log and handoff
Record failed hypotheses and measured numbers.
