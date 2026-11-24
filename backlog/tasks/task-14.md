---
id: task-14
title: Runtime hard parts: exceptions, GC safepoints, generic method slots
status: Planned
---
## Problem before solution
RECOMPILER.md 4.4, 4.5 and the 7,212 runtime-filled generic method slots from task-2.
## Outcome and scope
Unwind-by-return exceptions with LSDA landing pads, cooperative GC safepoints with SuspendThread and GetThreadContext HLE, and the generic slot fill path, in the lifter and runtime.
## Dependencies and ownership
Depends on task-5 (Verified). Coordinates with task-13 on `nn::os`. Owns `runtime/eh/`, `runtime/gc/` and lifter changes it lists.
Execution: subchat, Opus high (reason: correctness-critical runtime design).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Throw, catch, finally and rethrow tests pass; 0 uncaught exceptions on boot | Test | Report | NOT_RUN |
| AC2 | GC stress test with live-object checker passes | Test | Report | NOT_RUN |
| AC3 | Boot route shows 0 interpreter fallbacks for generic slots | Run | Log | NOT_RUN |
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
