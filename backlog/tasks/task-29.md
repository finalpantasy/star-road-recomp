---
id: task-29
title: Swap three lifted integer functions for Rust
status: Review
---

## Observed behavior

Lifted direct calls name a C function. Indirect calls look up its guest address.

## Outcome and scope

Build the Rust static library into the game and compare each replacement with its lifted function. Swaps default to off.

## Dependencies and ownership

D-003 and D-007. Owns the Rust workspace, runtime swap glue, game build integration and decomp guide.

## Acceptance and evidence

| Check | Result |
|---|---|
| Game executable links the pinned Rust library | Pending |
| 10,000 randomized comparisons per function | Pending |
| Hidden muted run compares real guest calls with zero differences | Pending |
| Locked project check passes | Pending |

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-25. Confirmed the signed integer overloads of Mathf.Min, Max and Clamp in the private method map and located their definitions in lifted chunk main_0364. The shared lock probe failed because the worker has read-only access to the work drive.
- 2026-11-25. Found zero lifted direct call sites for those overloads. Selected System.Math.Min(int,int), System.Math.Max(int,int) and Common.CommonRoutine.is_in(int,int,int) with 95, 71 and 26 direct call sites respectively. Added the Cargo workspace, CMake hook, wrappers, startup tests and live log check.
- 2026-11-25. Rust 1.96.0 type-check and formatting passed. Clang syntax checks passed for both wrapped chunks and all three runtime translation units. Checked all three generated registration entries against the private sources. The log reader passed one positive and four negative synthetic fixtures. The locked project check failed before launch with access denied on heavy.lock. No executable, randomized test results or live run evidence from this change yet.

### 2026-11-25 Claude lead
Lead built and ran what the sandboxed worker could not. Build links smrpg_decomp. Run `task29_ab` (90 s, hidden, muted, `--continue`, SMRPG_DECOMP=ab, selftest on): selftest 10,125 comparisons per function (10,000 random plus 125 edge), 0 differences for all three; live: Math.Min 291,735 calls and Math.Max 1,438 calls, 0 differences; CommonRoutine.is_in 0 live calls in this window (needs a scene that calls it), so check_log.py reports incomplete live coverage. Runtime boot, sync and rthard pass. Default stays off until the owner accepts.
