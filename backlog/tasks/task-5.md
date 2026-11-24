---
id: task-5
title: Lifter v0 and boot to first service call
status: Planned
---
## Problem before solution
No code yet.
## Outcome and scope
Lift all of v1.0.1 `main`, `subsdk0`, `sdk` to C, compile and link with a minimal runtime; the program runs its init and reaches its first SVC and service calls, which are logged. Non-goals: graphics, audio.
## Dependencies and ownership
Depends on task-4. Owners set when Ready.
Execution: set at Ready (likely Sonnet high per component, Opus for unclear failures).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every function in the task-2 inventory is lifted or listed with a reason | Lifter report | Counts | NOT_RUN |
| AC2 | Instruction tests: lifted semantics match a reference for the A64 subset used | Test suite | Report | NOT_RUN |
| AC3 | Boot log reaches the first nn service request | Run | Log | NOT_RUN |
## Human gates
None.
## Recovery and return condition
Generated output on A: only.
## Approach and commands
From docs/RECOMPILER.md.
## Log and handoff
Standard.
