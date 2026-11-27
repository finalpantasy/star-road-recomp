---
id: task-49
title: The one interpreter miss at 0x71012a0ed4 in every normal run
status: Review
---

## Observed behavior

Every normal default-mode run logged `interpreter misses: 1 distinct targets`, `miss 0x71012a0ed4 xN from 0x71012a09cc` (N grows with run length; `task47_on2.log` x3738). The run only reaches it once the game gets into its field (99 IPC commands).

## Cause (decoded from the prepared image)

0x71012a09cc is the return address after `bl 0x71012a0e60`, so it is the lr, not a branch. The function at 0x71012a0e60 (Unity native, 1,164 bytes) ends a loop body with a compact switch:

```
71012a0ec8  add x27, x27, #0xbfc      ; x27 = 0x7102bbabfc, set once before the loop (adrp+add)
71012a10c4  ldrh w8, [x20, #0x62]
71012a10c8  cmp  w8, #4
71012a10cc  b.hi 0x71012a1098
71012a10d0  adr  x9, 0x71012a0ed4
71012a10d4  ldrb w10, [x27, x8]       ; table {131,113,150,0,2}
71012a10d8  add  x9, x9, x10, lsl #2
71012a10dc  br   x9                   ; targets 0x10e0 0x1098 0x112c 0x0ed4 0x0edc
```

The lifter left this `br` unresolved (jt_unresolved), so jumping to the case at 0x71012a0ed4 had no lifted entry and went to the interpreter. The lifter's `match_compact_dataflow` failed for three general reasons, each fixed by a general rule:

1. Case blocks have no predecessor until the switch is resolved, so they were "unknown entry" blocks that loop back into the dispatch and made `x27` unknown at the join. New: such orphan blocks start from the state at the br and the analysis iterates to a fixpoint (`match_compact_dataflow`, `run(seed, extra)`). All orphans must be targets of this table, else nothing is resolved. The br also feeds its own targets that lie inside the closure (an own target in the closure used to reject the match).
2. A `bl`/`blr` cleared every register. Now x19 to x29 survive (AAPCS64: a returning callee preserves them) and x0 to x18, x30 become unknown (`_cs_apply`).
3. `ldrh w8, ...` then `ldrb w10, [x27, x8]` indexes with the 64-bit register, while the guard compares w8. An unsigned load of 1, 2 or 4 bytes zero-extends, so x8 and w8 now share one bound (`_cs_apply`, new `("z", ("lo", ...))` result).

The later "table target also reachable by another table" check in `lift()` now checks the other tables' targets against the visited and orphan blocks (`df_orph`), since own targets are modeled.

## Result

| Unresolved jump tables | main | sdk | subsdk0 | total |
|---|---|---|---|---|
| before (task-28 end) | 39 | 24 | 2 | 65 |
| after | 35 | 23 | 1 | 59 |

Six tables resolve in total; the one at 0x71012a10dc is among them.

## Evidence

| Check | Result |
|---|---|
| `tests/test_lifter_jump_tables.py` | 24 tests pass (4 new: callee-saved base survives a call and a caller-saved base does not, zero-extended narrow-load index, table target looping back, orphan block that is not a target is rejected) |
| `tests/test_lifter.py` (all lifter tests) | all PASS: crypto vectors, jump tables, boot, instruction oracle 38,814 compared 0 mismatches, lift and compile smoke |
| Relift main, sdk, subsdk0 (27 s) then `build_game.py --jobs 16` | success, all chunks rebuilt with their decomp wrappers, registered=12056 as before (no missing row); decompgen did not need a rerun |
| Default run, 120 s, hidden, muted, fresh save copy `task49_rt\d1`, `task49_d1.log` | `interpreter misses: none`, 99 IPC, 0 unknown, frame 19,641, decomp mode=on registered=12056. GPU was 98 percent busy before the run (someone else) yet the run was not slow |
| `SMRPG_DECOMP=ab` run with selftest, `task49_rt\ab1`, `task49_ab1.log`, `--max-seconds 120` | 0 `decomp mismatch`, 0 selftest differences, misses none, 99 IPC, 0 unknown, frame 19,608 |
| `python tools/game_check.py run --root .` | PASS 1795765442968196500-25ed5902 |

## Residual risk

Rule 2 trusts every callee to keep x19 to x29 (true for compiler output; a hand-written routine that breaks AAPCS64 would make the proof wrong). The 59 remaining tables are unchanged and still unproven; none was guessed. Appearance and feel are not part of this task.

## Changed files

`tools/lifter/lift.py`, `tests/test_lifter_jump_tables.py`, `backlog/tasks/task-49.md`.

### 2026-11-27 Claude lead
Lead reread task49_d1 and task49_ab1: interpreter misses none, 99 IPC commands, 0 unknown, 0 decomp mismatch lines; reran test_lifter_jump_tables.py: OK. Assumption to keep in mind: the new call rule trusts every callee to preserve x19 to x29 (AAPCS64). Unresolved jump tables 65 to 59.
