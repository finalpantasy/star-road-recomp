---
id: task-50
title: Decomp generator v2, float and SIMD, field-of-field and boolean getters, main_0169
status: Review
---

## Observed behavior

Task-47 built 12,028 generated functions (8 patterns, 0 differences) and left main_0169 (4 functions) out. The generator refused vector registers and every two-instruction shape that is not a plain getter or setter.

## Shapes counted first (functions of at most 5 instructions, whole main module)

| Shape | Matches | Taken |
|---|---|---|
| `ldr sN or dN, [x0..x2, #imm]; ret` (float getter) | 579 | yes (vget) |
| `str sN or dN, [x0..x2, #imm]; ret` (float setter) | 286 | yes (vset) |
| `fmov` immediate, `fmov s, wzr`, `fmov d, xzr`, `movi v.2d, #0` | 75 | yes (vconst, below 100, asked for) |
| `ldr x8, [x0, #a]; ldr {x,w,b,h,sb,sh,sw}0, [x8, #b]` | 284 | yes (get2) |
| `ldr x8, [x0, #a]; ldr s0 or d0, [x8, #b]` | 114 | yes (vget2) |
| `ldr wT, [x0, #imm]; cmp wT, #k or w1; cset w0, eq or ne` | 254 | yes (get_cmp) |
| `ldr wT; ldrb; ubfx or sxtw` extension getters | under 25 each | no |
| `ldp s0, s1, [x0, #imm]; ret` (Vector2 getter) and `ldp; vmov` shapes | 83 and larger groups | no, not in the asked set and pair loads need two lanes written |

## Semantics from the lifted reference

`Ctx` holds `v[32]` and the A/B `memcmp` covers the whole struct, so vector registers are compared. `OP_ldv` (`runtime/ir_ops.h`, `ir_ldv`) builds `{zero-extended load, 0}`, so `ldr s0` clears bits 32..127 and `ldr d0` clears bits 64..127; `fmov` immediate writes `sa_mk(bits)` = `{bits, 0}`; `movi v.2d, #0` writes zero. At `ret` the lifter flushes v0..v15 and x0..x7, x19..x29 only (`MASK_RET`), so the patterns accept registers v0..v15 and a scratch register x8..x17 (dead at `ret`, the set_bool argument). A float load or constant returns the low 64 bits and the wrapper writes `{bits, 0}`; a store reads the low 32 or 64 bits. The A/B stays the whole-context compare.

## Changes

- `tools/decompgen/patterns.py`: six patterns (vget, vset, vconst, get2, vget2, get_cmp), `Spec` gained `vkind`, `vreg`, `offset2`. `generate.py`: rows `VLEAF(chunk, addr, sym, vkind, vreg, base, off, len, off2)` and `DLEAF(chunk, addr, sym, base, off, len, off2)`, wider Rust signature for vector rows. `test_decompgen.py`: 15 tests, each template pinned on real words of the main module plus refusals, and `vfp_expand_imm` checked against the lifter's for all 512 inputs.
- `runtime/decomp/swap.c`: `Leaf` initializers are designated (13,652 rows and a new struct field would warn on every row), `VecLeaf`, `run_vec`, VLEAF and DLEAF rows, vector store through the shadow compare, selftest fills v0..v31 randomly from its own stream, sets x1 and x2 equal to the loaded field often, builds the pointer slot for two-level rows, and a retry for load rows (below). `wrap_chunk.py`, `check_log.py`, `game.cmake`: row regexes accept `V` and `D`.
- `docs/DECOMP.md`: pattern table, row kinds, semantics, race rule, build numbers.

## Result

Built set: all 13,624 generated rows (352 chunks, main_0169 included) plus 28 hand-written, 13,652 registered. `generate.py --limit 100000 --max-chunks 0` (no exclusions).

| Pattern | Matches | Built | Live (union of 2 A/B runs) | Live calls (2 runs) | Differences |
|---|---|---|---|---|---|
| empty | 3,629 | 3,629 | 432 | 79,502,885 | 0 |
| const | 1,988 | 1,988 | 174 | 8,853,479 | 0 |
| get | 4,892 | 4,892 | 366 | 112,833,492 | 0 |
| get_nullsafe | 21 | 20 | 2 | 34,899 | 0 |
| set | 940 | 938 | 65 | 1,967,751 | 0 |
| set_bool | 438 | 438 | 8 | 94,323 | 0 |
| add_imm | 96 | 94 | 21 | 17,434,107 | 0 |
| mov_reg | 33 | 33 | 0 | 0 | 0 |
| vget (new) | 579 | 579 | 61 | 6,531,100 | 0 |
| vset (new) | 286 | 286 | 8 | 195,930 | 0 |
| vconst (new) | 75 | 75 | 3 | 76,904 | 0 |
| get2 (new) | 284 | 284 | 43 | 5,081,503 | 0 |
| vget2 (new) | 114 | 114 | 1 | 76 | 0 |
| get_cmp (new) | 254 | 254 | 28 | 7,995,328 | 0 |
| generated total | 13,629 | 13,624 | 1,212 | 240,601,777 | 0 |

(The matches minus built difference is the 5 rows already hand-written.) Union is from `task50_ab1b` (task-42 save) and `task50_ab2b` (`codex_battle_hard_02`); vget2 was live in one run only (76 calls) and mov_reg never.

main_0169: included in the build and it compiled. The first full build (13,624 rows, 352 chunks, `--jobs 16`, 9 minute `timeout`) ran 444 s end to end including the 17 MB `main_0169_decomp.c.obj` (7.5 MB wrapped C); the object is the long pole so its own compile is at most 444 s with the other 351 compiles sharing the machine, not measured alone. It is no longer excluded.

| Build | Time | Note |
|---|---|---|
| `task50_build1` (all rows, main_0169 included) | 444 s | `smrpg.exe` 315,295,232 B |
| `task50_build2` (swap.c designated initializers: 13,652 warnings gone) | 184 s | swap.c recompile and relink |
| `task50_build3` (swap.c retry for load rows) | 243 s | `smrpg.exe` 315,406,848 B |

## Acceptance and evidence

| Check | Result |
|---|---|
| Hidden muted A/B, task-42 save copy `task50_rt\t42` (`task50_ab1b.log`) | `decomp mode=ab registered=13652`, selftest clean for 13,652 rows (about 94 s), 0 `decomp mismatch`, `decomp raced total=0`, exit code 4 (watchdog), 99 distinct IPC, 0 unknown, interpreter misses none, 22,933 presents, frame 22,928. `check_log.py --summary`: live differences 0 |
| Hidden muted A/B, copy of `codex_battle_hard_02` with its input (`task50_ab2b.log`, captures not modified) | 0 mismatch, raced 0, exit code 4, 99 IPC, 0 unknown, misses none, frame 15,221. `check_log.py --summary` clean |
| Default mode, 120 s, fresh save copy (`task50_on.log`) | `decomp mode=on registered=13652`, exit code 4, 99 IPC, 0 unknown, misses none, 20,729 presents. GPU was 100% busy before the run (other work on the machine), still above the 5,000 present floor |
| `cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline` (5 passed), decompgen unit tests (15) | clean |
| Locked project check `python tools/game_check.py run --root .` | PASS (1795768427093040000-f0583ee8) |

## One difference found and how it was handled

The first A/B on `codex_battle_hard_02` (`task50_ab2.log`, before the retry existed) logged one `decomp mismatch addr=0x7100492560 ctx_byte=0 lifted=64 rust=00` at 205.99 s: `ldr w0, [x0, #0x68]`, a task-45 `get u32` row that has been in every earlier run (77,304 to 79,760 live calls each, 0 differences in task-46, task-47 and the task-50 t42 run). Selftest for it was clean (25,625 calls). Rust and lifted C run one after the other and both read live memory, so a write by another thread in between gives rust 0 and lifted 0x64 with identical code. It is not shown to be a race, only consistent with one: it did not repeat in the next two A/B runs (about 77,000 calls each of that row). I did not exclude the 4,892-row `get` pattern for it, and did not loosen the compare: `compare` in `swap.c` now reruns the same pair once from the saved input, only for load rows, only when the first pair differed. A repeat is still a hard `decomp mismatch`; an agreeing retry is a counted `decomp raced` line that `check_log.py --summary` prints (0 in both final runs) and the selftest (private memory) fails on any race. The owner may prefer to reject that change and exclude load rows from live compare instead.

## Residual risks

Live counts come from two 150 s runs, not three save sets. vget2, mov_reg and most of the 13,624 rows never ran live (selftest only). Appearance and feel are not approved by any of this. The `raced` rule exists because of one unexplained event; if `decomp raced` ever shows up repeatedly on one address, treat it as a real bug and not a race.

## Reproduction

`python -I tools/decompgen/generate.py --limit 100000 --max-chunks 0`, then `python -I tools/rtprep/build_game.py --jobs 16` (about 8 minutes from clean objects). A/B: `python -I tools/rtprep/run_game.py --seconds 260 --log NAME --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SMRPG_SAVE=... --env SMRPG_OPTIONS=... --env SMRPG_INPUT_FILE=... -- --max-seconds 150`. The previous task-47 set is kept in `A:\AI\Recomps\smrpg-work\decompgen_t47\`.

## Log

- 2026-11-27. Counted shapes, added six patterns and the vector and field rows, built everything including main_0169, A/B on two sources, default run, game_check PASS.

### 2026-11-27 Claude lead
Lead reread task50_ab1b, task50_ab2b and task50_on: 0 mismatch lines, `decomp raced total=0` in both A/B runs, 99 IPC commands, 0 unknown, interpreter misses none. Decomp generator unit tests rerun OK. Lead decision: keep the retry for load rows. A wrong Rust load repeats on the retry and stays a hard mismatch; only a pair that agrees on the identical retry is counted as raced, the count is printed, and the selftest fails on any race. The single task50_ab2 mismatch on 0x7100492560 (rust 0, lifted 0x64) did not repeat in 75,988 later calls; cause not proven.
