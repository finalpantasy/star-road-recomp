---
id: task-57
title: replay recorder grown, out footprints built, calc_belome_barrior_ratio ported; renkei, group_a_b and the damage_calc driver not ported
status: Review
---

## Observed behavior

Task-56 left `damage_calc` (0x71021ff580, 613 instructions) with three blockers: a 12-call recorder, no out-parameter handling and three unported callees. This task removes the first two and ports one callee. It does not finish the driver; the remainder is listed below.

## What was built

- `runtime/decomp/swap.c`: `REC_MAX` removed. The recorder is a per-thread heap buffer (starts at 64 calls, doubles, limit 4096). Past the limit it logs "decomp replay: more than 4096 traced calls in one body" and exits 10 (no silent skip). New `OLEAF(chunk, addr, reg, off, bytes, in)` row kind and `smrpg_guest_bl_out`: in a trial the callee does not run, the pre bytes are compared when `in=1` and the recorded post bytes are returned; in `on` mode the callee runs live. Journal entries and watch marks inside the footprint are dropped. Selftest graph (ten classes) for 0x71021f5c50.
- `decomp/smrpg_decomp/src/bridge.rs`: `Regs::invoke_out`, `prestate::read_u64`, unit test `invoke_out_returns_the_bytes_the_callee_wrote`.
- `decomp/smrpg_decomp/src/battle/frame.rs`: `Frame` helper mirroring the lifted locals and the flush lists at calls and return.
- `decomp/smrpg_decomp/src/battle/belome.rs`: `SkillExecter.calc_belome_barrior_ratio` 0x71021f5c50 (QLEAF with hook), IL2CPP names, float-free; 3 unit tests.
- `leaves.def`: 0x71021f5c50 is QLEAF; new PLEAF rows 0x7101886a00, 0x71000c5060, 0x7101d7c0c0 and OLEAF 0x710015b4c0 (List<int>.Add, 16 byte footprint). `wrap_chunk.py`, `game.cmake`, `check_pure.py`, `check_log.py` accept OLEAF.
- `docs/DECOMP.md`: section "damage_calc (task-57)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean; 41 tests passed |
| `check_pure.py` | 0 failures |
| `tools/rtprep/build_game.py` | first build 15m12s (over the 10 minute limit: 4 new chunks plus external load, GPU 30 to 99 percent busy); final rebuild 1m00s |
| Selftest `task57_self2` (`--seconds 340 -- --max-seconds 150`) | belome 25,625 calls, 0 differences; 0 mismatch, raced 0, IPC 99 distinct 0 unknown, misses none, 22,672 presents |
| A/B battle route `task57_final` (own save, copy of codex_battle_hard_02) | 0 mismatch, raced 0, IPC 102 0 unknown, misses none, 13,158 presents; damage_calc 12 calls, belome 12 calls (1 deferred), 0 differences; List<int>.Add 134,938 and GlobalWork.get_inst 37,647 traced, 0 differences |
| Earlier A/B `task57_bh3` | 18,034 presents, 0 mismatch |
| `on` mode battle `task57_on` | battle reached, 25,666 presents |
| A/B task42_rt save `task57_t42` | 20,761 presents, 0 mismatch, new rows 0 calls |
| Mutation 1 (belome, wrong branch result) | 13 of 14 belome calls mismatched |
| Mutation 2 (belome, wrong constant 0x8203) | 366 of 25,625 selftest differences, exit 10 |
| `python tools/game_check.py run --root .` | PASS |

Both mutations were reverted (file identical to the saved good copy, no MUTATION marker).

## Deviations and discarded runs

- damage_calc ran 12 times per battle run, not about 15. Belome always took the early return (character id is not 0x649), so the Belome-barrier body is covered by the selftest only; live coverage is 0.
- Mutations were placed in `calc_belome_barrior_ratio`, not in the driver, because the driver is not ported.
- Discarded: a selftest with 35 presents, and two runs (bh1, bh2) that had no save or input due to a quoting bug in my run script.

## Not done

1. `calc_renkei_ratio` 0x71021f5ed0 (284 instructions, float math, `Random.Range(0,21)`, List<int>, string format). Needs `OLEAF(.., 1, 0, 4, 1)` and `fp.rs`.
2. `group_a_b` 0x7102201aa0 (390 instructions, not decoded yet).
3. The `damage_calc` driver in `damage.rs`. Footprints it needs: belome x2 4 bytes in; renkei and group_a_b x1 (= x29 - 4) 4 bytes in; `Single.ToString` x0 (= sp + 8) 4 bytes in.

## Lead verification (2026-11-27)
- Reread task57_final (0 `decomp mismatch`, raced 0, 102 IPC with 0 unknown, misses none), task57_t42 (0 mismatch, 99 IPC, 0 unknown) and the mutation logs (13 and 366 mismatch lines).
- Over the rule: the first build took 15 min 12 s (limit 10). Partial: calc_renkei_ratio, group_a_b and the damage_calc driver stay lifted; next task.
- Diff scanned for em dashes and personal details: none.
