---
id: task-43
title: Rust decomp batch 5, first leaves that write guest memory
status: Review
---

## Observed behavior

All 18 Rust leaves so far were register-only or load-only, so the `ab` byte comparison of the register context was enough. A leaf that stores to guest memory needs the memory compared too, and without Rust and lifted C both writing the same live bytes.

## Outcome and scope

Extend `ab` mode so a leaf can store, then add up to five store-only setters. Design (`docs/DECOMP.md`, `runtime/decomp/swap.c` `compare_store`): `leaves.def` declares each footprint with `SLEAF(chunk, address, symbol, base_reg, offset, bytes)`. Store leaves take a fourth Rust argument `delta` added to every store address. In `ab` mode Rust runs on a register copy with its stores redirected into a private shadow copy of the footprint, lifted C runs on live memory, and the registers plus the footprint bytes are compared. Live memory ends exactly as lifted C left it. Rust never touches live memory, so the only residual race is another thread writing the footprint between the shadow copy and the compare, which could give a false difference. The selftest uses a private fake region and requires at least one call to change the footprint.

| Guest address | Method | Rust symbol | Footprint (base, offset, bytes) | Direct call sites | Live calls (task43_ab) | Differences |
|---|---|---|---|---|---|---|
| 0x71002cdf10 | Unity native, zero 12 bytes (unnamed) | smrpg_zero_12 | x0, 0, 12 | 329 | 425,204 | 0 |
| 0x7101451240 | CRI native, x0 twice at x2, w1 at x2+0x10 (unnamed) | smrpg_store_x0_pair_x1_0x10 | x2, 0, 20 | 40 | 285 | 0 |
| 0x7101347ed8 | Unity native setter w1 to x0+4 (unnamed) | smrpg_store_w1_0x4 | x0, 4, 4 | 39 | 0 | not claimed |
| 0x7101e357b0 | Battle.BattleCharacterAnimator.set_idle_id(int, int) | smrpg_set_idle_id | x0, 84, 8 | 25 | 0 | not claimed |
| 0x7101a0aa40 | Flag.off() | smrpg_flag_off | x0, 16, 1 | 24 | 0 | not claimed |

Selftest per leaf: 10,000 random plus 15,625 edge calls, 0 differences. The three 0-call leaves are verified by the selftest only, not live (a second 200 s run with `input_open2.txt`, `task43_ab2`, also had 0 calls for them and 0 differences elsewhere).

Skipped: leaves that set x8 through a `movi` first (0x7100fc3d90, 126 sites, and 0x7100223ca4, 42 sites) because the Rust ABI returns x0 only and the register comparison would flag x8; 0x7100f92970 and others with a load plus compare; anything with calls, write barriers, exclusives or atomics; 0x7101df0ca0 (20 sites) and the next ranks. The scan kept functions of at most 10 operations that are only `OP_st` or `OP_stp` through x0, x1 or x2 with x0, x1, x2 or xzr as values, ranked by direct call sites.

## Dependencies and ownership

Task-29, task-40, task-41, task-42. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Rust type-check, fmt and unit tests | PASS (`cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline`, 3 tests) |
| Selftest per store leaf, 10,000 random plus 15,625 edge, fake region only | PASS (all 23 leaves, 0 differences; each store leaf changed its footprint) |
| Hidden muted 200 s A/B run (`task43_ab`, fresh copy of the task-42 save, task-24 options, `input.txt`) | PASS for 0x71002cdf10 and 0x7101451240 (live calls above 0, 0 differences). 0 mismatches in the log. `check_log.py` fails on the 0-call leaves (the older three plus the new three), as in tasks 41 and 42 |
| Locked project check passes | PASS (1795735659871344000-eb271308) |

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-26. Scanned the lifted chunks for store-only leaves with a return and no tail call. Runs `task43_ab` and `task43_ab2`: 0 mismatches. Not committed.

### 2026-11-26 Claude lead
Lead read the diff. In `on` mode store leaves get delta 0 and write live memory like the lifted body. In `ab` mode Rust writes only a shadow copy of the footprint and lifted C alone touches live memory, so a wrong Rust leaf cannot corrupt the game. `task43_ab.log` has 0 mismatch lines; zero_12 (425,204 calls) and the CRI store (285) are live with 0 differences. set_idle_id, Flag.off and the w1 setter have 0 live calls and are selftest-only. game_check PASS (1795735659871344000-eb271308).
