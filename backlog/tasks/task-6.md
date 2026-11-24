---
id: task-6
title: Decomp language spike (C vs Rust vs Odin) for D-003
status: Planned
---
## Problem before solution
D-003 is OPEN. The owner wants a lightweight, very fast result; the lead leans C for in-place function swapping.
## Outcome and scope
Decompile the same 5 representative game methods (one battle timing method, one menu, one field movement, one data table reader, one coroutine) in C and Rust (Odin if cheap), each swapped into the stage 1 build and checked by a differential test. Report readability, swap friction, build time, and performance. The owner then decides D-003.
## Dependencies and ownership
Depends on task-5 (needs a running stage 1 build) and task-2 (names). Owners set when Ready.
Execution: Sonnet high; lead reviews.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | All versions pass the same differential test | Recorded inputs | Reports | NOT_RUN |
| AC2 | Plain-language comparison the owner can decide from | Report | Doc | NOT_RUN |
## Human gates
D-003 decision by the owner.
## Recovery and return condition
Spike branch or folder only.
## Approach and commands
TBD at Ready.
## Log and handoff
Standard.
