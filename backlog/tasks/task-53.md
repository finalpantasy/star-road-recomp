---
id: task-53
title: Float-exact readable ports: a Rust ARM64 float layer, and Battle.BattleCamera.adjust_camera_transform through it
status: Review
---

## Observed behavior

Task-52 skipped its best live candidate, `BattleCamera.adjust_camera_transform`, because float code must also reproduce the FPSR cumulative flags (and the FPCR modes) bit for bit, and the A/B compares the whole `Ctx`. This task builds that float layer and ports the method.

## Part A: the float layer

- `decomp/smrpg_decomp/src/fp.rs` (about 1,200 lines with 14 tests): a line-by-line Rust port of the soft path of `runtime/ir_simd_a.h` (`sa_unpack`, `sa_round`, `sa_sum`, `sa_s_addsub`, `sa_s_mul`, `sa_s_div`, `sa_s_fma`, `sa_s_fcmp`, `sa_s_minmax`, `sa_s_cvt`, `sa_s_tofix`, `sa_s_fromfix`) for single and double precision, with no host float operation. `FpEnv { fpcr, fpsr }` reads FZ, DN and RMode and ORs cumulative flags into FPSR. Typed wrappers (`fadd32`, `fmul32`, `fmadd32`, `fcmp32`, `fadd64`, ...), the `Float32Ops` trait for ports, and `Regs::fp_env` / `Regs::set_fpsr` in `bridge.rs` for the guest context (offsets 780 and 784, asserted in `swap.c`).
- Why the soft path only: the C fast paths (host SSE/FMA, FPCR 0, ordinary operands) are meant to agree with it, and the selftest below runs the C helpers as they are, fast paths included, so any disagreement would show.
- `runtime/decomp/fp_selftest.c` (new, in `game.cmake`): at startup under `SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1` it runs the C helpers and `smrpg_fp_selftest_op` on the same operands: fmul, fdiv, fadd, fsub, fmax, fmin, fmaxnm, fminnm, fmadd, fmsub, fnmadd, fnmsub, fcmp, fcmpe, fcvt both ways, float to int (5 rounding modes, fixed-point bits, signed and unsigned, 32 and 64 bit), int to float, in both precisions, every pair of 45 (single) or 37 (double) edge operands in 10 FPCR modes plus 60,000 random pairs per op, random FPSR going in. Result and FPSR out must be equal.
- Nothing found where the lifted C ignores a flag the ARM pseudocode sets; the port follows the C anyway. Half precision, AHP, FEAT_AFP, trapped exceptions, fsqrt, frint, estimates and vector forms are not in the layer (same limits as the C soft path or not used by a port yet).

## Part B: the port

- `Battle.BattleCamera.adjust_camera_transform` (0x7101e2cfd0, 69 instructions, 10,287 live calls in task52_ab1; 3,659 to 4,100 in this task's 150 s runs). Readable body in `battle.rs`: `BattleCamera` (`_camera` 0x10, `_look_at_position` 0x3c, `_euler_angle` 0x48, `_distance` 0x54, `_is_position_change_locked` 0x74, `_camera_shake` 0x90), `CameraShake.offset` (0x34), trait `CameraCalls` (the six Unity callees by name: `Component.get_transform`, `Transform.set_eulerAngles`, `Quaternion.Internal_FromEulerRad`, `Quaternion.op_Multiply`, `Transform.set_position`, `get_position`; arithmetic through `Float32Ops`), `unity.rs` (`Vector3`, `Quaternion`, `Transform`, `Camera`, `DEG2RAD` = 0x3c8efa35), `f32_field` in `object.rs`.
- Shim `LiftedCamera` + `smrpg_camera_adjust_camera_transform`: stages at each of the 9 call sites exactly the registers the lifted body flushes (read from the lifted C), restores the epilogue registers, d8 to d10 as `{low64, 0}`.
- Row `ELEAF(main_0343, 7101e2cfd0, ...)` and traced `PLEAF` rows for the six callees (main_0273, main_0274 x3, main_0364 x2), same `ab` rules as task-52 (replay, no callee runs twice). `swap.c`: `smrpg_guest_safepoint` (the lifted loop back-edge poll after the follow step, `Regs::safepoint`), the selftest fake graph in `prepare_caller` (edge floats in every field, flag byte 0 to 0xff, shake null or present, 9 FPCR picks, random FPSR), `_fltused` (the first float code in the Rust archive references it; the link failed without it).
- Docs: `docs/DECOMP.md` section "Float layer (task-53)".

## Acceptance and evidence

| Check | Result |
|---|---|
| `cargo check --release --locked --offline`, `cargo fmt --check`, `cargo test --release --locked --offline` | clean, 31 tests (16 new: 14 in `fp`, 2 camera). The one older warning in `lib.rs` (unused assignment in a test) is not from this task |
| `fp` unit tests | hand-computed: tie to even, 4 rounding modes, overflow to infinity vs largest finite, divide by zero, NaN propagation and default NaN, denormal inputs and tiny results under FZ (IDC, UFC), signed zero in every mode, fused vs split fmadd, fcmp flags, min and max with zeros and NaNs, conversions, double precision |
| Camera `cargo test` | call order and values for every branch against a recorder; shim against a transcription of the lifted C with scrambling fake callees: 1,680 graphs (3 flag values x shake or none x 7 FPCR modes x 40 edge-float picks), whole register file, call order and safepoints equal |
| `python -I runtime/decomp/check_pure.py` | PASS for both entries (unchanged) |
| Build `python -I tools/rtprep/build_game.py` | build 1: 9 min 1 s, link failed (`_fltused`); relink after the fix 2 min 43 s; later rebuilds 35 s each |
| Float selftest (startup, `task53_final_selftest`) | 2,732,380 cases, 0 differences |
| Row selftest, camera, 10,000 random + 15,625 edge | 25,625 calls, 0 differences (stand-in callees, fake graph) |
| Selftest run, copy of `codex_battle_hard_02` in `task53_rt\bh`, `--seconds 260 -- --max-seconds 150`, `task53_final_selftest` | 0 `decomp mismatch`, raced 0, "IPC: 99 distinct commands used, 0 unknown", "interpreter misses: none", 9,039 presents, camera live 3,659 calls 0 differences; `check_log.py --summary`: selftest clean for 13,664 rows, live differences 0. First run of the same build, `task53_selftest`: 0 mismatch, 10,888 presents, camera 3,798 calls |
| A/B, same bh copy, no selftest, `task53_final_bh` (first build `task53_ab_bh`) | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 9,375 presents (9,267), camera live 4,100 calls (3,865), 0 differences; callee rows 0 differences |
| A/B, copy of `task42_rt\save` in `task53_rt\t42`, `task53_final_t42` (first build `task53_ab_t42`) | 0 mismatch, raced 0, 99 IPC 0 unknown, misses none, 22,808 presents (20,710). Field route: 0 live camera calls |
| `python tools/game_check.py run --root .` | PASS (1795778277473533800-30fd18e9) |

GPU was 72 to 100 percent busy from other work before most runs (run_game warned); presents stayed above 9,000, so the runs are valid (rule: a 120 s run under 5,000 presents is invalid).

## Mutation tests (all reverted, sources byte-identical to the saved copies, rebuilt)

| Mutation | Result |
|---|---|
| Flag: `round` sets IXC only for positive results | Startup selftest: 185,516 float differences, exit 10 at 0.6 s (`task53_mut_selftest`, first lines "fmul ... rust=0x80000000/0xc lifted=0x80000000/0x1c"). The live A/B did not see it: 1,559 and then 1,832 camera calls, 0 differences (`task53_mut_ab`, `task53_mut_ab2` with the opposite sign), because FPSR flags are sticky and IXC is already set by the game's first inexact operation. |
| Operand order: `behind - look_at` instead of `look_at - behind` | Live A/B: 1,785 `decomp mismatch addr=0x7101e2cfd0 replay: recorded=6 used=6 call_mismatch=1`, "call 6 to 0x71017a7910, v0 differs" (`task53_mut_ab3`, first at 57 s) |
| Rounding: round to nearest even replaced by truncation | Live A/B: 1,808 mismatches, "call 4 to 0x7102057610, v1 differs" (`task53_mut_ab4`) |

## Not shown

- Live, the camera runs the follow step without a shake in both runs that mismatched under mutation (every mismatch had 6 recorded calls). The shake branches (tail `set_position` with the offset added, the locked path, the safepoint with a pending collection) are covered by the selftest and `cargo test`, not by the game route. `g_gc_pending` was never seen set during a camera call.
- A sticky-flag consequence: the live compare cannot see a missing IXC (or any flag the game has already set); only the startup selftest, which feeds a random FPSR in, can.
- Replay shows the port matches the lifted body given the callees' recorded results; it does not show the Unity natives are right (unchanged lifted code) and cannot see a field a callee rewrites between two reads. None seen.
- The fp layer is used by one port; fsqrt, frint and the vector forms are not ported.

## Dependencies and ownership

Task-44, task-51, task-52. Owns `decomp/smrpg_decomp`, `runtime/decomp/` and `docs/DECOMP.md`.

## Log

- 2026-11-27. Not committed. `STATE.md`, `runtime/nvn`, `tests/nvn`, task-12, task-36 to task-39 and `HANDOFF.md` untouched.

## Lead verification (2026-11-27)
- Reread task53_final_bh, task53_final_t42 and task53_final_selftest logs: 0 `decomp mismatch` lines each, `decomp raced total=0`, 99 IPC commands with 0 unknown, interpreter misses none, presents 9,360, 22,800 and 9,000.
- Selftest log: `decomp fp selftest cases=2732380 differences=0`.
- Mutation log task53_mut_ab4: 1,808 mismatch lines, so the live A/B catches a rounding change. The final runs came after the mutations were reverted.
- Diff scanned for em dashes and personal details: none.
