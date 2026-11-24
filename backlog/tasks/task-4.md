---
id: task-4
title: Recompiler and runtime architecture design
status: Verified
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
| AC1 | Each known hard part has a chosen approach, a test, and a fallback | Doc review by lead | Doc | PASS (lead review 2026-11-24; lead reran decode check, 10,898 insns, 0 mismatches): `docs/RECOMPILER.md` section 12, 20 hard parts each with approach, test and fallback; rejected designs in section 11 |
| AC2 | Lifter language and build system chosen with a measured reason | Small spike | Spike numbers | PASS (2026-11-24): C via clang 23, own Python build driver with 400-function chunks plus CMake for the runtime. 3,857 of 4,000 sampled functions lifted; LLVM IR gives no compile gain (42.2 vs 46.3 CPU s); 400 per file 10 CPU s per 1k functions vs 315 at 1 per file; locals model 25 percent faster than ctx with equal end state on 841 functions; full `main` build estimate 2 to 4.5 min on 16 jobs. Section 13; data in `A:\AI\Recomps\smrpg-work\spike\` |
## Human gates
None technical. D-003 stays the owner's.
## Recovery and return condition
Doc only. Return at done.
## Approach and commands
Read task-2/3 outputs, `F:\AI\Recomps\ref\*` for ideas (read only, no code copying).
## Log and handoff
Record rejected designs and why.
- 2026-11-24 task-4 worker: doc written; rejected designs in `docs/RECOMPILER.md` section 11; spike code in `tools/spike/` (own code, no game bytes; output on A:). Open items for the lead in section 14. Nothing committed.
