---
id: task-44
title: Rust decomp batch 6, first functions that call ported leaves, and live coverage for the 0-call leaves
status: Review
---

## Observed behavior

All 23 Rust functions so far were leaves. Six of them (HashHelpers.Combine, get_computedStyle, the 16.16 fixmul, the w1 setter, set_idle_id, Flag.off) had selftest evidence only, with 0 live calls.

## Outcome and scope

Part A. `CLEAF(chunk, address, symbol)` in `runtime/decomp/leaves.def` declares a function whose only callees are already Rust leaves. The Rust symbol takes a pointer to the guest register file and edits it in place (so x1 to x3 and the stale x30 are reproduced, not only x0), calling the Rust callee directly. `ab` runs it on a copy of the whole `Ctx` and compares every byte with lifted C (`compare` in `runtime/decomp/swap.c`, `docs/DECOMP.md`). Guest stack: the lifted push of x29 and x30 lands below the final sp and is dead after the return, so those bytes are not compared; sp and every register are. Updated `game.cmake`, `wrap_chunk.py` and `check_log.py` for the new row kind.

| Guest address | Method | Rust symbol | Callees | Footprint | Direct call sites | Live calls (runs 1, 2, 3) | Differences |
|---|---|---|---|---|---|---|---|
| 0x71005b3d70 | Unity native, x0 = *(u64 *)element_address(x0, x1, 8), stack frame (unnamed) | smrpg_call_array_load_8 | 0x71005aca60 | none (registers, one load) | 3 | 0, 0, 0 | not claimed |
| 0x71005b3d50 | Unity native, x2 = 8, tail call to element address (unnamed) | smrpg_call_array_elem_addr_8 | 0x71005aca60 | none | 2 | 29,669; 29,671; 29,663 | 0 |
| 0x71005ae5a0 | Unity native, object to field to u32 at +8, stack frame (unnamed) | smrpg_call_nested_u32_8 | 0x71005b3a20 | none (two loads) | 1 | 55,801; 12,841; 11,345 | 0 |
| 0x7101cccaf0 | Unity native, w1 == 0x81d ? 1 : is_in(w1, 0x7f8, 0x7fb) (unnamed) | smrpg_call_in_range_7f8 | 0x71018ba2c0 | none | 0 | 0, 0, 0 | not claimed |
| 0x71001919f0 | Unity native, x0 = (*(u64 *)(x0 + 0x10) == x1) (unnamed) | smrpg_call_field_ptr_eq | 0x7101df0f10 | none (one load) | 0 | 0, 0, 0 | not claimed |

Selftest per function: 10,000 random plus 15,625 edge calls, 0 differences, on a private fake region and fake stack. A mutation test (one return address changed in the Rust body) failed the selftest on x30, so the comparison does see the stale x30 and registers. Live calls of these callers also count the callees they reach, so the callee live counts in the logs include them.

Scan: lifted chunks, functions of at most 20 operations whose every `bl` or tail call is in `leaves.def`, no `rt_icall`, no vector registers. 12 matched. Rejected: 0x7101e72320 and 0x7101e734b0 (they call the store leaf set_idle_id, which has 0 live calls, and need a 68-byte footprint plus their own byte store), 0x7100ddce60 (stores through x1 and calls three times), 0x7101932720 (0 direct sites, one ptr_eq call plus compare and csel, not taken to stay at five), 0x7101a90c20 and 0x710192e2d0 (identical one-op ptr_eq tail calls with 0 sites), 0x710192e400 (same as 0x71001919f0 with offset 0), all 0 direct sites. Only three callers had direct call sites; ranking beyond them was by shape and variety (tail call, stack frame, branch). Bigger callers (the 77 to 109 operation functions around 0x71011eeeb0) are out of scope.

Part B. Hidden muted 200 s `SMRPG_DECOMP=ab` runs with selftest, copies of the Codex captures' saves, options and `input.txt` in `A:\AI\Recomps\smrpg-work\task44_rt\` (captures not modified). Run 1: `codex_battle_hard_02` (reaches the Terrapin battle, frame 12,872). Run 2: `codex_bandits_route_22` (frame 7,762). Run 3: `codex_battle_off_02` (frame 17,750).

| Leaf | Run with live calls | Live calls | Differences |
|---|---|---|---|
| 0x7101b905b0 HashHelpers.Combine | none | 0, 0, 0 | selftest only |
| 0x7101c9d200 get_computedStyle | none | 0, 0, 0 | selftest only |
| 0x7100f8a7f0 16.16 fixmul | none | 0, 0, 0 | selftest only |
| 0x7101347ed8 w1 setter | none | 0, 0, 0 | selftest only |
| 0x7101e357b0 set_idle_id | none | 0, 0, 0 | selftest only (the battle menu was reached in run 1 (frame 5,400 in the capture), so it is not called on the first attack turn) |
| 0x7101a0aa40 Flag.off | run 1 (15 calls), run 2 (5 calls); run 3 had 0 | 15, 5, 0 | 0 |

## Dependencies and ownership

Task-29, task-40, task-41, task-42, task-43. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Rust type-check, fmt and unit tests | PASS (`cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline`, 4 tests) |
| Selftest per function, 10,000 random plus 15,625 edge, fake region and fake stack | PASS (all 28 functions, 0 differences) |
| Hidden muted 200 s A/B runs `task44_ab1`, `task44_ab2`, `task44_ab3` | 0 mismatch lines in all three. 0x71005b3d50 and 0x71005ae5a0 have live calls with 0 differences. `check_log.py` fails on the leaves and callers with 0 live calls (HashHelpers.Combine first), as in tasks 41 to 43 |
| Locked project check passes | PASS (1795755281846924800-0a1a76f3) |

Not live: 0x71005b3d70, 0x7101cccaf0 and 0x71001919f0 (callers), plus the five older leaves above except Flag.off. They are selftest only.

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-26. Selftest first crashed because the lifted caller pushes to a random sp and returned early on a random `exc`; fixed in the selftest setup (fake stack, `c.exc = 0`), not in the comparison. Runs `task44_ab1` to `task44_ab3`: 0 mismatches. Not committed.

### 2026-11-26 Claude lead
Lead read the diff: each CLEAF row calls only Rust versions of already verified leaves, so no callee runs twice on live memory in ab mode; the whole register context including sp and x30 is compared. All three logs (task44_ab1 to ab3) have 0 mismatch lines. Live: array_elem_addr_8 and nested_u32_8; Flag.off now has live calls (15 and 5). game_check PASS (1795755281846924800-0a1a76f3).
