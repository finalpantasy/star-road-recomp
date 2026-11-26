---
id: task-42
title: Rust decomp batch 4, first leaves that read guest memory
status: Review
---

## Observed behavior

All 13 Rust leaves so far were register-only. A leaf that only loads from guest memory leaves memory unchanged on both sides, so the `ab` byte comparison stays valid for it.

## Outcome and scope

Add up to five load-only leaves (no stores, calls, atomics, exclusive loads, floating point or lazy static init) with many direct call sites. Rust reads guest memory the way lifted code does: guest addresses are host addresses (`runtime/ir_ops.h` MLD and `ir_ld`, relaxed atomic loads), so `guest_ld64` and `guest_ld32` in `decomp/smrpg_decomp/src/lib.rs` use relaxed atomic loads at the raw address.

| Guest address | Method | Rust symbol | Direct call sites | Live calls (task42_ab) |
|---|---|---|---|---|
| 0x71005b3a20 | Unity native null-safe load, x0 == 0 ? 0 : *(x0 + 0x10) (unnamed) | smrpg_ld64_nullsafe_0x10 | 4,957 | 67,226,568 |
| 0x71011eeeb0 | load global slot 0x7103ded330, plus 0x18 (unnamed) | smrpg_static_7103ded330_add_0x18 | 3,328 | 110,617 |
| 0x71011ef830 | load global slot 0x7103ded330 (unnamed) | smrpg_static_7103ded330 | 729 | 9,725 |
| 0x71005b8b00 | load global slot 0x7103d823f8 (unnamed) | smrpg_static_7103d823f8 | 550 | 550 |
| 0x710200fd30 | System.String.IsNullOrEmpty | smrpg_string_is_null_or_empty | 244 | 120,333 |

Differences in the live run: 0 for all five. Selftest: 10,000 random plus 15,625 edge calls each, 0 differences.

Skipped: 0x71004a2240 (549 sites) and other global slot getters of the same shape (I took the first three by call sites); 0x7102180f20 (216), a plain `ldr x0,[x0,#0x10]` with no null check; 0x710061116c (514), an ldp plus load plus shifted add that I did not port; 0x710183ba20 (106), six operations with two flag tests. 0x710200fd30 was chosen over 0x71004a2240 to cover the compare form. Nothing named by the IL2CPP method map except IsNullOrEmpty; the rest are Unity native code or unnamed globals.

## Dependencies and ownership

Task-29, task-40, task-41. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Rust type-check and fmt | PASS (`cargo check --release --locked --offline`, `cargo fmt --check`) |
| Rust unit tests with a fake guest region | PASS (`cargo test --release --locked --offline`, 2 tests) |
| Selftest per leaf, 10,000 random plus 15,625 edge | PASS (all 18 leaves, 0 differences). Pointer leaves run against a private fake region or null; global slot leaves read the real mapped slot |
| Hidden muted 200 s A/B run (`task42_ab`, fresh copy of the opening save, task-24 options and input) | PASS for the five new leaves: all have live calls, 0 differences. `check_log.py` still fails on the three older leaves with 0 live calls (HashHelpers.Combine, get_computedStyle, fixmul_16), as in task-41 |
| Locked project check passes | PASS (1795732151427847400-087f3d45) |

A concurrent writer between the Rust read and the lifted read could cause a false difference on the global slot leaves. None appeared.

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-26. Scanned the lifted chunks for functions with at most 10 operations, only loads and register arithmetic, no stores or calls, ranked by direct call sites. Run `task42_ab`: 0 mismatches.

### 2026-11-26 Claude lead
Lead read the diff: all five leaves are loads only (relaxed atomic, same as the lifted `ir_ld`), no stores or calls. Lead reread `task42_ab.log`: selftest 25,625 calls and live calls above 0 for all five, 0 differences. `check_log.py` still fails only on the three older 0-call leaves (task-41). game_check PASS (1795732151427847400-087f3d45).
