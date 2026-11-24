---
id: task-4
title: Recompiler and runtime architecture design
status: Planned
---
## Problem before solution
We need a design that beats existing recomps on correctness and upgradability. Known hard parts (hypotheses to confirm): IL2CPP managed exceptions are C++ exceptions (unwinding across lifted code); Boehm GC scans guest stacks and registers conservatively; heavy virtual and interface dispatch through vtables and generic sharing; NEON and FP exactness; atomics and memory ordering across guest threads; NVN plus Maxwell shaders; frame-rate assumptions in game logic.
## Outcome and scope
`docs/RECOMPILER.md`: lifter design (IR or direct C, function boundaries, indirect branch dispatch, jump tables, tail calls), runtime model (memory map, threads, TLS, exceptions, GC visibility), HLE boundary, graphics strategy (NVN HLE vs Unity GfxDevice-level), upgrade hooks (resolution, frame rate), test strategy (instruction-level oracle tests, differential traces), build-time budgets, and the stage 2 function-swap mechanism. Includes ideas taken from other recomps with credit, and what we do differently and why.
## Dependencies and ownership
Depends on task-2 and task-3. Owns `docs/RECOMPILER.md`.
Execution: Opus, high effort. Reason: system architecture for the whole project.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Each known hard part has a chosen approach, a test, and a fallback | Doc review by lead | Doc | NOT_RUN |
| AC2 | Lifter language and build system chosen with a measured reason | Small spike | Spike numbers | NOT_RUN |
## Human gates
None technical. D-003 stays the owner's.
## Recovery and return condition
Doc only. Return at done.
## Approach and commands
Read task-2/3 outputs, `F:\AI\Recomps\ref\*` for ideas (read only, no code copying).
## Log and handoff
Record rejected designs and why.
