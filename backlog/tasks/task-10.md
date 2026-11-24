---
id: task-10
title: Maxwell shader translator (our own) to SPIR-V
status: Ready
---
## Problem before solution
All 416 shader assets (about 82k variants) exist only as compiled Maxwell binaries inside NVN shader blobs. Nothing renders without a translator.
## Outcome and scope
`tools/shaderxlate/` (decoder, IR, SPIR-V emitter, our own code) plus a batch driver that translates every variant found in the owner's romfs into a cache on A:. Non-goal: runtime pipeline creation (task-12).
## Dependencies and ownership
Depends on task-3 (shader inventory). Owns `tools/shaderxlate/`, `tests/shaderxlate/`.
Execution: subchat, Opus high (reason: compiler-grade ISA work, the project's long pole).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every variant in the romfs translates or is listed with a reason | Batch run | Counts | NOT_RUN |
| AC2 | All outputs pass spirv-val | Batch run | Report | NOT_RUN |
| AC3 | Differential check against Ryubing's translator as oracle on a sample of 500 variants (same inputs, equal results in a compute or render harness) | Test | Report | NOT_RUN |
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
