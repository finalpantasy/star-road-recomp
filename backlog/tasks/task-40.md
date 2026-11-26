---
id: task-40
title: Rust decomp batch 2, five more pure leaves
status: Review
---

## Observed behavior

Task-29 proved the swap pipeline on three 32-bit integer leaves. Many small lifted leaves remain that write only x0.

## Outcome and scope

Add five register-only leaves to the Rust crate with the same wrapper, selftest and A/B check. Leaves touch no guest memory, make no calls and use no floating-point or SIMD state. The swap glue now passes full 64-bit registers, so pointer-sized leaves are covered.

| Guest address | Method | Rust symbol | Direct call sites |
|---|---|---|---|
| 0x71005aca60 | array element address, x0 + sext32(x1) * x2 + 0x20 (unnamed in the method map) | smrpg_array_elem_addr | 723 |
| 0x7101b905b0 | System.Numerics.Hashing.HashHelpers.Combine(int, int) | smrpg_hash_combine | 75 |
| 0x7101d70190 | System.Type.op_Equality | smrpg_ptr_eq | 390 |
| 0x7101d715a0 | System.Type.op_Inequality | smrpg_ptr_ne | 110 |
| 0x7101df0f10 | System.IntPtr.op_Equality | smrpg_ptr_eq | 223 |

Rejected: Saturate_To_Int8, Int16 and the unsigned forms (Burst intrinsics, branchy, no expected live use), Math.Min and Max for byte and sbyte (16 sites each, rare), Char.IsHighSurrogate and IsLowSurrogate (clean but lower value), anything with a load, store, call or float.

## Dependencies and ownership

Task-29. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Rust type-check and fmt | PASS |
| Selftest per leaf, 10,000 random plus 15,625 edge combinations | PASS (all 8 leaves, 0 differences). It caught a wrong first draft of the array leaf (sign extension of the index) |
| Hidden muted 200 s A/B run with real guest calls | PARTIAL (task40_ab: array 1,839,233, Type.op_Equality 122,418, Type.op_Inequality 780,390, IntPtr.op_Equality 12,469,112, 0 differences; Combine 0 live calls, live coverage incomplete) |
| Locked project check passes | PASS (second run; the first run failed rthard with exit 1 and was not reproduced) |

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-26. Scanned the lifted chunks for functions with at most 14 operations, no memory operations and no calls, ranked by direct call sites. Widened the swap glue to u64 arguments and results and the selftest edge set to 25 values per register (64-bit limits plus byte and short limits). check_log.py reads the edge count from the log.
- 2026-11-26. Run `task40_ab` (200 s, hidden, muted, copy of the task-29 opening save, task-24 input): no mismatches. check_log.py stops at HashHelpers.Combine (0 live calls in this window). Its selftest passes, so it stays, with live coverage not claimed. Needs a scene that hashes a struct key.

### 2026-11-26 Claude lead
Lead reran `game_check run`: PASS (1795718705966807300-751785dd); a worker run failed once with rthard exit 1 and passed on rerun, cause not found. HashHelpers.Combine has 0 live calls so its live coverage is incomplete; the other four leaves and the older three show 0 differences live. Default `on` applies to all eight leaves, so run `SMRPG_DECOMP=ab` first if a scene misbehaves.
