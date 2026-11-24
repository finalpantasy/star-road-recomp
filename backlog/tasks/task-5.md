---
id: task-5
title: Lifter v0 and boot to first service call
status: Verified
---
## Problem before solution
No code yet.
## Outcome and scope
Lift all of v1.0.1 `main`, `subsdk0`, `sdk` to C, compile and link with a minimal runtime; the program runs its init and reaches its first SVC and service calls, which are logged. Non-goals: graphics, audio.
## Dependencies and ownership
Depends on task-4 (Verified 2026-11-24). Owner: smrpg-lifter.
Execution: subchat, Sonnet high; escalate unclear failures to the lead (Opus).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every function in the task-2 inventory is lifted or listed with a reason | Lifter report | Counts | PASS (see log) |
| AC2 | Instruction tests: lifted semantics match a reference for the A64 subset used | Test suite | Report | PASS (see log) |
| AC3 | Boot log reaches the first nn service request | Run | Log | PASS (see log) |
## Human gates
None.
## Recovery and return condition
Generated output on A: only.
## Approach and commands
From docs/RECOMPILER.md.
## Log and handoff
2026-11-24, smrpg-lifter. Evidence is on A: under `A:\AI\Recomps\smrpg-work\build\` (logs, tests, bin); code in `tools/lifter/`, `runtime/`, `tests/oracle/`. Nothing committed.

AC1 (lift report `logs/lift_<module>.json`, schema `inventory_rows, functions, lifted, listed_with_reason, reasons, failed`):
| module | inventory rows | unique starts | lifted | listed with reason |
|---|---|---|---|---|
| main | 157,221 | 157,215 | 157,214 | 1 (dp2: CRC32B) |
| sdk | 39,508 | 39,508 | 39,466 | 42 (36 AESIMC/AES simd, 3 system, 2 CRC32, 1 data word) |
| subsdk0 | 13,625 | 13,625 | 13,617 | 8 (7 SHA/AES simd, 1 extr word) |
All 51 listed functions contain ARMv8 crypto-extension or CRC32 words (or a data word) that v0 does not implement. Their dispatch slots hold interpreter stubs, which stop on the same word. The task-2 inventory has 6 duplicate or zero-size start rows; `inventory.load` keeps the longest row per start. Totals: 210,348 functions, 210,297 lifted. Other lift stats: 11.4M instructions, 880k calls, 49.7k tail calls, 2,798 functions with mid-function targets (inventory splits one function into several FDE pieces; they run through the interpreter), 2,676 jump tables, 560 unresolved jump-table candidates.

AC2 (`tests/oracle/oracle.py`, Unicorn 2.1.4 in `A:\AI\Recomps\tools-ext\venv-unicorn`; 2.1.3 has no wheel, pin recorded here): interpreter ops, lifted C and Unicorn compared on real words from main `.text` with register-field mutation and edge-weighted states, FPSR and QC included. Final run `--per-op 100 --states 10 --tag final`: 94 ops, 177,794 words, 3,468,862 cases compared, 0 mismatches, 19,409 skipped (Unicorn faults), report `build/tests/report_final.json`. That run took about 88 min wall (over the 10 minute rule; it was started in the foreground and moved to the background by the harness). The check that runs under game_check is the small form (`tests/test_lifter.py`, about 1 minute: 37,920 cases, 0 mismatches). Worker runs: scalar FP `report_a_final.json` 306,420 cases, 0 mismatches, plus synthetic encodings 663,876 cases and 54,636 with FPCR modes; NEON `report_b_final.json` 653,740 cases and `report_b_stm.json` 27,540 cases, 0 mismatches. Not covered: half-precision arithmetic (Unicorn faults; written from the spec), FPCR AHP/AH/FIZ/NEP/FZ16 on conversions, FRSQRTS last-bit on subnormal inputs, crypto and CRC32 words.

AC3 (`logs/boot_lifted.log`): the loader maps main/sdk/subsdk0 at 0x7100000000/0x7110000000/0x7120000000, relocates them (imports: HLE 19+22+9, wrapped-lifted 508, unresolved 0), starts `nn::init::Start` (sdk+0xfa3c0) on a 16 MB guest stack, and the first service request is `svcConnectToNamedPort("sm:") -> handle 0x10002` at lr=0x71101405c4, 0.001 s of guest time after entry (about 0.07 s after process start), exit code 0, "interpreter misses: none". Differential check: with `SMRPG_NO_LIFTED=1` the whole boot runs in the IR interpreter and gives the same svc and HLE sequence (only guest stack addresses differ, because lifted code keeps registers in C locals), 48 interpreter misses, exit 0 (`logs/boot_interp.log`). The check `tests/test_lifter.py` is wired into `tools/check-config.json`; `python tools/game_check.py run --root .` passed (it failed twice while other sessions edited files).

Build wall times: inventory seconds; lift 14 s (528 chunks of 400 functions); irgen 30 s; compile about 9.5 min with 32 cores while other jobs ran (529 objects, 954 MB of objects, 653 MB of C on A:); link 1 s. An integer-only compile was about 3 to 4 minutes.

Deviations from docs/RECOMPILER.md (all measured or reasoned):
1. Callee-saved registers x19-x29 and v8-v15 are flushed at calls and at ret (dirty only), so unlifted callers and the interpreter see them; the doc left this to the emitter.
2. No loop polls and no landing pads in v0. `c->exc` is checked after each call instead.
3. The interpreter IR is one 32-byte record per text word (`img/ir_<module>.bin`), not per basic block, so mid-function targets and stubs can enter anywhere.
4. Functions the lifter cannot lift get `interp_run` stubs in the dispatch table; mid-function indirect targets miss and fall back to the interpreter.
5. Jump tables come from a symbolic walk with a bound from `cmp #N`, `and #mask` or a cmp in a predecessor block; 560 jump-table candidates stay unresolved and go through `rt_dispatch`.
6. Op semantics have a single source (`ops.py` and the extension `ops_*.py` generate both the C macros and the interpreter switch), which the doc did not specify.
7. Lift is 14 s and compile 3 to 10 min, much better than the doc's estimate.
8. Bug found late: the interpreter built its IR file path wrongly (`img` + `ir_sdk.bin` without a separator), so no interpreter run had ever worked; fixed in `runtime/interp.c`, and the all-interpreter boot is the test for it.
Follow-ups: implement AES/SHA/CRC32 ops (51 functions); merge inventory fragments so fewer targets fall to the interpreter; multi-thread stress; lifted landing pads.
