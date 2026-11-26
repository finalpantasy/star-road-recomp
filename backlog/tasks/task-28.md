---
id: task-28
title: Close lifter instruction and jump-table coverage gaps
status: Verified (AC3 partial)
---
## Problem before solution
Task-5 reported 51 inventory entries emitted as interpreter stubs, plus 45 unresolved jump-table candidates in main, 28 in sdk, and 2 in subsdk0.
## Outcome and scope
Implement A64 AES, SHA-1, SHA-256, PMULL, CRC32 and CRC32C emission. Resolve additional computed jump tables when the index range and targets can be checked.
## Dependencies and ownership
Depends on task-5. Worker owns `tools/lifter/`, lifter tests, and this card.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Crypto and CRC32 instructions | Published vectors and instruction-level cases | Lifter test report | PASS. `tests/test_lifter_crypto.py` runs emitted AES, SHA-1, SHA-256, PMULL and CRC vectors (lead, 2026-11-25). |
| AC2 | Original interpreter stubs | Relift all three modules | Before and after failure counts | PASS. Relift: stubs 51 to 6 (main 1 to 0, sdk 42 to 4, subsdk0 8 to 2). Left: 3 system (cache maintenance), 1 op0 0, 1 simd, 1 extr. |
| AC3 | Computed jump tables | Validate bounded matches and compare all three modules | Before and after counts and matcher tests | Partial. New compact byte/halfword switch matcher (`match_compact_jump_table`, 7 focused tests). jt_unresolved: main 45 to 40, sdk 28 to 24, subsdk0 2 to 2 (total 75 to 66). The rest need CFG dataflow (table base defined outside the guard block) or have no provable bound; groups in A:\AI\Recomps\smrpg-work	ask28_jt\shapes.md. |
| AC4 | Repository check | Run under the shared lock | PASS receipt | PASS. game_check PASS after relift, 537 chunks recompiled, game rebuilt; 80 s hidden run reaches the title screen at frame 4200, 0 unknown IPC, interpreter misses none, movies 0 decode errors. |
## Human gates
None.
## Recovery and return condition
Keep generated images, code, and reports on A:. Leave repository changes for lead integration.
## Approach and commands
- `python -I tests/test_lifter_jump_tables.py`
- `python -I tools/rtprep/heavy_lock.py -- python -I tests/test_lifter_crypto.py`
- `python -I tools/rtprep/heavy_lock.py -- python -I tools/lifter/build.py lift main sdk subsdk0`
- `python -I tools/rtprep/heavy_lock.py -- python tools/game_check.py run --root .`
## Log and handoff
2026-11-25. Started task-28 and inspected the baseline lift reports. Their failure categories include `system`, `extr`, and `op0 0` alongside the crypto and CRC candidates. Each category needs inspection before attributing all 51 stubs to crypto.

2026-11-25. Added portable AES, SHA-1, SHA-256, PMULL and CRC emission and seeded the instruction oracle. The same op definitions generate interpreter cases. Verified existing IR IDs against the prepared header. Unicorn executed all 31 synthetic encodings and matched eight CRC width and polynomial cases. A read-only check found that the decoder now handles the first failing word in 27 crypto or CRC entries and 19 scalar saturating SIMD entries. Of the five remaining first failures, three are cache-maintenance instructions and two faulted in Unicorn. A complete relift is required to find any later unsupported words in those functions.

2026-11-25. Added recovery for a split indexed address calculation plus an immediate load. The matcher rejects bypassed guards, missing table entries, and invalid targets. Six focused tests passed. Historical unresolved counts are main 45, sdk 28, subsdk0 2. The shared lock cannot be created from this sandbox, and the installed Clang path is inaccessible. Full verification requires a session with both available.

2026-11-25 lead. Relifted under heavy.lock (before reports in A:\AI\Recomps\smrpg-work\task28_before\), compiled 537 chunks, rebuilt with build_game.py. Results in the table. Jump tables need a new matcher shape: the 75 remaining candidates were not studied by shape yet. Note: standalone `build.py link` fails on missing HLE and a64_crypto_exec symbols because its runtime list is older than HLE; the game route (build_game.py) links.

2026-11-26. Compact switch matcher (target = adr base + (byte or halfword table entry << 2), table address from adr/adrp+add, index bounded by cmp+b.hi, ccmp+b.hi, AND or UBFX mask in the same straight-line run; rejects bypassed guards, missing entries, targets outside the function or not instructions). Relift, 10 chunks recompiled, build_game, game_check PASS, 80 s hidden run: 0 unknown IPC, interpreter misses none, title screen at frame 4200. Also `build.py link` now writes bin\link.rsp and stops (the legacy stand-alone exe is opt-in via `--exe` and is not the game route).
