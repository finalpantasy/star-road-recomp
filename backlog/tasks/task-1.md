---
id: task-1
title: Reproducible extraction script from the owner's dump
status: Ready
---
## Problem before solution
Extraction was done by hand on 2026-11-24 (hactool from the New Pokemon Snap tools folder, a cleaned keys copy, manual titlekey). Fact: the owner's prod.keys has malformed 34-hex-digit entries (mariko_* style) that make hactool abort; a filtered copy lives at `A:\AI\Recomps\smrpg-work\keys\prod.keys`. Fact: the patch NCA needs the titlekey from the update's `.tik` (offset 0x180, 16 bytes) and `--basenca` of the base program NCA.
## Outcome and scope
One command turns `F:\Super Mario RPG` dumps plus a prod.keys path into the canonical layout on A: (base/exefs, v101/exefs, v101/romfs) and a hash manifest. Later this becomes the first-run builder step for users. Non-goals: GUI, other titles.
## Dependencies and ownership
None. Owns `tools/extract.py`, `tests/test_extract.py`. Writes only under `A:\AI\Recomps\smrpg-work\canonical-test\`. Must not touch `v101/` or `base/` (other workers read them).
Execution: worker subagent, Sonnet, low effort (mechanical, well specified).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Script reproduces every hash in tools/inputs.json "extracted" and the romfs count/bytes | Run into canonical-test, compare | Command, output, report | NOT_RUN |
| AC2 | Bad keys lines are skipped without printing key values | Grep script output for 32+ hex runs | Output excerpt | NOT_RUN |
| AC3 | Tool paths and keys are arguments, not hardcoded user paths | Code review | Diff | NOT_RUN |
## Human gates
None.
## Recovery and return condition
Delete `canonical-test/` to undo. Return at done or blocked.
## Approach and commands
Python 3, `-I`. Call hactool by path argument. Run hactool at BelowNormal priority. Never print key or titlekey values.
## Log and handoff
Record actual commands, timings, results.
