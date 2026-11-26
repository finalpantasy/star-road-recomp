---
id: task-41
title: Rust decomp batch 3, five more pure leaves
status: Review
---

## Observed behavior

Task-40 left 13 more small lifted leaves worth swapping, and HashHelpers.Combine had 0 live calls in the opening-save run.

## Outcome and scope

Add up to five register-only leaves with many direct call sites (same wrapper, selftest and A/B). No guest memory, calls, lazy statics or floating point.

| Guest address | Method | Rust symbol | Direct call sites | Live calls (task41_ab) |
|---|---|---|---|---|
| 0x7101df0190 | System.IntPtr.op_Inequality | smrpg_ptr_ne | 235 | 12,958,875 |
| 0x7102021db0 | System.String.GetRawStringData, x0 + 0x14 | smrpg_add_0x14 | 201 | 1,787 |
| 0x7100f8a7f0 | 16.16 fixed-point rounded multiply (unnamed) | smrpg_fixmul_16 | 167 | 0 |
| 0x7101c9d200 | VisualElement.get_computedStyle, x0 + 0x2c8 | smrpg_add_0x2c8 | 88 | 0 |
| 0x7101df0f80 | byte array element address, x0 + sext32(x1) (unnamed) | smrpg_add_sext32 | 60 | 7,247,574 |

Rejected: 0x71002dcc4c (339 sites) and 0x710106c1a4 (73) and 0x71011f0d8c (49) return a constant, 0x71007af0e8 (130) returns 0, so there is no logic to port; 0x710110b160 (101 sites) has a conditional chain of six operations I did not decode; Math.Min and Max byte, sbyte, short forms (16 sites each, repeats of the task-40 reject); anything with a load, store, call or float.

## Dependencies and ownership

Task-29, task-40. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Acceptance and evidence

| Check | Result |
|---|---|
| Rust type-check and fmt | PASS (`cargo check --release --locked --offline`; plain `cargo check` fails on the no_std panic handler, as before this task) |
| Selftest per leaf, 10,000 random plus 15,625 edge combinations | PASS (all 13 leaves, 0 differences). It caught a wrong first draft of the byte array leaf (I read an operand as a shift) |
| Hidden muted 200 s A/B run | PARTIAL: 0 differences everywhere. Live calls 0 for fixmul_16, get_computedStyle and HashHelpers.Combine, so those three are not claimed live. `check_log.py` fails on Combine (and the two new ones) for that reason |
| Locked project check passes | PASS (1795721390940974300-1c6563ed) |

HashHelpers.Combine routes tried (each 200 s, A/B, fresh save copy, task-24 options): codex_mushroom_kingdom_03 (its input.txt, 286 lines) and codex_battle_hard_01 (input.txt, 169 lines). Both 0 calls, 0 differences. The three routes tried (opening, mushroom kingdom, battle) never hash a struct key; no further runs were made. The current exe did not visibly diverge from either route but divergence was not measured.

## Reproduction

See [the decomp guide](../../docs/DECOMP.md).

## Log

- 2026-11-26. Scanned the lifted chunks for functions with at most 10 operations, no memory operations and no calls, ranked by direct call sites. Run `task41_ab` (opening save): 0 mismatches.

### 2026-11-26 Claude lead
Lead checked the diff: all five leaves are pure register arithmetic (no guest memory access). game_check PASS (1795721390940974300-1c6563ed). Live coverage incomplete for 16.16 multiply, get_computedStyle and HashHelpers.Combine (0 calls); they are covered by the selftest only. Needs scenes that call them.
