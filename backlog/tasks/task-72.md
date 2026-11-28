---
id: task-72
title: exec_koudou_kouka table 1, first arms (those route bwf reaches, then one skill family)
status: Review
depends: task-70, task-71
---

## Goal

Split plan step 2 (docs/DECOMP.md): port the table 1 arms (`switch (x12)`, lifted lines about 1028 to 1988) that route `bwf` reaches, then one more skill family if time allows, into the task-70 head port. The pre-state hook keeps deferring only the arms that are still lifted.

## Acceptance

- Card lists every table 1 arm (label, skill type range, line count, ported or not) and which arms `bwf` reaches.
- Unit tests; `cargo check`, `cargo check --release`, `cargo fmt --check`, `cargo test` pass.
- Selftest row 0 differences, with the graph covering the ported arms; at least one caught mutation per ported arm, reverted and rebuilt.
- Three `ab` runs of `bwf` with `SMRPG_DETERMINISTIC=1`: identical row counts, fewer deferred `exec_koudou_kouka` calls than task-71 (16+2d), 0 `decomp mismatch`, raced 0, 0 unknown IPC, misses none.
- `python tools/game_check.py run --root .` PASS. DECOMP.md note.

## Arm inventory

The full table of the 31 table 1 targets (types, lifted line counts, callees, state) is in docs/DECOMP.md, "exec_koudou_kouka table 1, first arms (task-72)". Route `bwf` reaches table 1 once per run, with type 0xe, target 0x71021f6800 (found with a temporary log in the pre-state hook, since removed; the second deferred call of task-71 was the first call of the process, lazy initialization). Ported: the epilogue target (52 types), 0x71021f7618 (0x12), 0x71021f6800 (0xb to 0x10), 0x71021f6840 (0x15 0x19 0x46), 0x71021f6928 (0x4a), 0x71021f68e8 (0x50), 0x71021f68f4 (8), 0x71021f6aa0 (7 9), 0x71021f6ad0 (5), 0x71021f6b28 (6). The "one more skill family" is types 5 to 9 plus 0x4a and 0x50: the small arms that call only `ba6a0`, `ba6d0` (already rows) and two 13 line getters. Not ported: 21 targets. Two of them (0x71021f6884, 0x71021f6ca0) use only small callees that are already rows and are the next step; the rest call the large callees 0x71021f76a0, 0x71021f7b40, 0x7101cd4790, the enumerator or the debug string code.

## Evidence

| Check | Result |
|---|---|
| `cargo check`, `cargo check --release`, `cargo fmt --check`, `cargo test` (120 tests, 6 new arm tests and the arm table test) | pass |
| Startup selftest (`t72_self_fin`, `--seconds 340 -- --max-seconds 150`) | `addr=0x71021f6340 random=10000 edge=15625 calls=25625 differences=0`; no selftest line with differences, 0 `decomp mismatch` |
| Mutation: ClearFlag logger +0x40 store 1 to 2 (`t72_m1_clearflag`) | caught (store journal, 4,842+ lines) |
| Mutation: StoreOne `w9` 1 to 2 (`t72_m2_storeone`) | caught, differences=1655 |
| Mutation: dispatch `w9 = 9` to 8 (`t72_m3_storenine`) | caught, differences=6010 (also the guard arms' flushed x9) |
| Mutation: StoreNine arm only, `w9 = 8` (`t72_m9_storenine`) | caught, differences=555 |
| Mutation: result byte +0x24 store 1 to 2 (`t72_m4_resultbyte`) | caught, differences=1119 |
| Mutation: Guard11 `or 8` to 9 (`t72_m5_guard11`) | caught, differences=782 |
| Mutation: Guard10 `or 4` to 5 (`t72_m6_guard10`) | caught, differences=1102 |
| Mutation: type 5 halving `>> 1` to `>> 2` (`t72_m7_half`) | caught, differences=210 |
| Mutation: type 6 mode 3 to 4 (`t72_m8_plain`) | caught, differences=1073 |
| Mutation: `_flag_44` store 0 to 1 (`t72_m10_flag44`) | caught, differences=3874 |
| Mutation: epilogue arm uses the direct store (`t72_m11_epilogue`) | caught, differences=1685 |
| Mutation: dispatch `x11` table address +2 (`t72_m13b_x11`) | caught, differences=6001 (x11 is flushed at the guard, getter calls) |
| Mutation: no pre-state snapshot of `this +0x44` (`t72_m14b_nosnap44`) | caught, differences=142 (the barrier branch reads the cell the arm stores) |
| Mutation: omit the +0x44 store (`t72_m16_omit44`), omit the +0x38 store (`t72_m17_omit38`) | caught: "the lifted body changed ... and the port did not store it" (watch marks work) |
| Mutations not observable: `snapshot(..., watch=false)` on `this +0x44` and on result +0x38 (`t72_m14_snap44`, `t72_m15_snap38`) | The watch flag only adds the check "the lifted body changed the cell and the port stored nothing"; the other check (port stored a value memory does not hold) stays, and m16 and m17 show the watch check fires when a store is missing. Reading the post-state of `+0x38` instead of the snapshot gives the same `or` result (idempotent), so no behavior differs. |
| Mutation: dispatch `x13` or `x10` `x12` registers in the epilogue arms | Not observable by construction for the epilogue targets: `x10` to `x13` are written at the dispatch but only flushed at calls, and those arms make none; the arms that call flush them and are covered by m13b. |
| Three final `ab` runs of `bwf` (`t72_fin_ab1..3`, `SMRPG_DETERMINISTIC=1`, `ab 320 200`) | identical counts: `exec_koudou_kouka` 16/0+1d (task-71: 16/0+2d), `exec_each_targets` 16/0, `damage_calc` 14/0+1d; 0 `decomp mismatch`, raced 0, 99 IPC commands 0 unknown, interpreter misses none |
| `python tools/game_check.py run --root .` | PASS (`1795869383896054300-d891c09b`, run after the last edit to code and docs; log `t72_game_check.log`) |

Notes. The one deferred call left on the route is the first call of the process (lazy initialization); the type 0xe call that task-71 deferred now runs in the port and is compared live (16 live calls). The ab runs ran with the arm reached live only through 0x71021f6800; the other nine targets are proved by the selftest graph and mutations (the route does not reach them). Two cells the arms store are read before the arm stores them: `this +0x44` (the barrier branch tests it) is now read with `prestate::read_u8`, snapshotted in the hook. The selftest graph gained the result object (`this +0x18`), the getter object (target +0x28), skill object +0x14 and the status manager's +0x10.

Files: decomp/smrpg_decomp/src/battle/exec_koudou_head.rs, decomp/smrpg_decomp/src/battle/exec_koudou_head/tests.rs, runtime/decomp/swap.c (selftest graph only), docs/DECOMP.md, backlog/tasks/task-72.md.

## Lead verification

- Files: only the task-72 set (head port, its tests, swap.c selftest graph, DECOMP.md, this card). Logs t72_fin_ab1 to ab3: each 0 `decomp mismatch`, raced total 0, IPC 99 / 0 unknown, interpreter misses none.
- The four non-plain mutation results are accepted with the reasons above: the watch flag only adds a missing-store check (m16, m17 prove it fires), and x10 to x13 are never flushed in arms that make no call (m13b covers the arms that do).
- No em dashes or personal details. game_check rerun by the lead before commit.
