---
id: task-10
title: Maxwell shader translator (our own) to SPIR-V
status: Verified
---
## Problem before solution
All 416 shader assets (about 82k variants) exist only as compiled Maxwell binaries inside NVN shader blobs. Nothing renders without a translator.
## Outcome and scope
`tools/shaderxlate/` (decoder, IR, SPIR-V emitter, our own code) plus a batch driver that translates every variant found in the owner's romfs into a cache on A:. Non-goal: runtime pipeline creation (task-12).
## Dependencies and ownership
Depends on task-3 (shader inventory). Owns `tools/shaderxlate/`, `tests/shaderxlate/`.
Execution: subchat, Opus high (reason: compiler-grade ISA work, the project's long pole).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every variant in the romfs translates or is listed with a reason | Batch run | Counts | PASS (ours, Phase B, failure burn-down 2026-11-24): 25,754 unique stage programs (43,496 Unity variants found in the romfs; the 82k in Problem is the task-3 estimate, not reconciled here), 25,754 ok, 0 failed; all 43,496 variants have every stage ok (`A:\AI\Recomps\smrpg-work\shaders\cache\v1\report.json`, 478 s at BelowNormal). The 51 former failures (LDG 31, bindless handle trace 6, BRX 4, VMAD 4, VMNMX 3, multisample fetch 3; 4 of them geometry programs) are a unit test (`former_failures`, with `--corpus`). Before the burn-down: 25,703 ok, 51 failed. Phase A (Ryubing, D-009): 25,754 of 25,754 ok (`A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\report.json`). |
| AC2 | All outputs pass spirv-val | Batch run | Report | PASS: ours 25,754 of 25,754 pass spirv-val (target vulkan1.3), `A:\AI\Recomps\smrpg-work\shaders\cache\v1\report.json`; Phase A 25,754 of 25,754 pass. |
| AC3 | Differential check against Ryubing's translator as oracle on a sample of 500 variants (same inputs, equal results in a compute or render harness) | Test | Report | PASS, burn-down rerun 2026-11-24 (worker): 500 of 500 variants compared, 500 equal, 0 differ (697 stage programs: FS 498, VS 199; same seed 1 sample as the lead run) with the oracle-compat switches `half_fold,no_fmz,fminmax` (`A:\AI\Recomps\smrpg-work\shaders\oracle_burn_all\report.json`). With `half_fold` alone: 496 equal, 4 differ (`oracle_burn_half`): the 4 newly compared LDG VS programs, where ours gives 0 and Ryubing NaN; `no_fmz` (Ryubing ignores the `.FMZ` 0 times anything = 0 rule) and `fminmax` (Ryubing uses FMin/FMax, not the NaN-dropping NMin/NMax of FMNMX) remove all 4, so ours keeps the hardware rule in both. Our compat translations of the 693 programs the lead compared are byte-identical to `oracle_lead`. Earlier: PASS, lead accepted the half_fold condition 2026-11-24 (our shipped build keeps the hardware fp16 rounding that Ryubing skips, so it is the more faithful of the two; lead reran into `oracle_lead`: 496 equal, 0 differ, 4 not compared). Hardware-truth check waits on the L4 reference original. 500 variants (seed 1, 697 stage programs) rendered by both translators in vkdiff on lavapipe with identical UBO, texture and vertex inputs. With the oracle-compat switch `half_fold`: 496 variants bit-equal, 0 differ, 4 not compared (VS programs with LDG, which ours does not translate yet); FS 498 of 498 equal, VS 195 of 195 equal (`A:\AI\Recomps\smrpg-work\shaders\oracle_hf5\report.json`). The condition: `half_fold` only mirrors a Ryubing optimizer shortcut (it folds UnpackHalf2x16(PackHalf2x16(x)) into x, so it skips the fp16 rounding hardware does between half ops). The shipped build keeps that rounding, and against it the oracle gives 56 equal, 443 differ, 1 not compared (`A:\AI\Recomps\smrpg-work\shaders\oracle_f2\report.json`); every one of those differences goes away under `half_fold`. Phase A uses the same Ryubing build, so ours matches Phase A to the same extent. Lead recheck 2026-11-25 on a new sample (seed 7, `oracle_lead_s7`, same switches): 499 of 500 variants equal, 692 stage programs (FS 495 equal, VS 196 equal); the 1 differ is bindless d30675c2, max abs 2.97e-3 on rt0 (worse than the 1.6e-4 the worker saw), open as a bug. Lead accepted `no_fmz` and `fminmax` as oracle conditions on the same grounds as `half_fold` (Ryubing breaks a hardware rule, ours keeps it). |

## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Generated output and caches on A: only. Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-008: our own code; Snap (`A:\AI\Recomps\new-pokemon-snap`) and Ryubing (`A:\AI\Recomps\new-pokemon-snap\tools\ref\ryubing-ryujinx`) may be read for lessons and run as test oracles only, never copied.
## Log and handoff
Record failed hypotheses and measured numbers.

2026-11-24 (worker):
- Commands: full cache `python tools/shaderxlate/batch.py --jobs 24` (343.9 s, BelowNormal); oracle
  `python tools/shaderxlate/oracle_diff.py --n 500 --work <dir> [--compat half_fold]` (resumable,
  rerun the same `--work`; faithful pass 72 s of vkdiff once translated, half_fold 184 s); tests
  `python -I tests/shaderxlate/test_shaderxlate.py --report <json>` (20 passed). Phase A:
  `python -I reuse/shaders/ryu_cache.py --jobs 12` (gitignored; 77 s translate, about 6 min rebind
  and spirv-val; rerun to continue).
- Bugs the oracle found in ours, all fixed and covered by unit tests: FFMA RC form had B and C swapped;
  FFMA.FMZ was split into multiply plus add instead of one fused op; HFMA2 RC form operands; MRG_H1
  wrote the lane 0 result into H1; FMUL scale codes 4 and 6 were swapped (x8 and x2), the cause of
  the factor-4 VS diffs; IADD carry chain; batch.py never actually set BelowNormal.
- Oracle behavior that is not ours to copy: Ryubing's optimizer folds UnpackHalf2x16(PackHalf2x16(a))
  into a, so it skips the fp16 rounding hardware does between half ops. Ours rounds (faithful);
  `SXL_COMPAT=half_fold` mirrors the oracle for the test only, and batch.py refuses to run with it set.
- Probe artifact: truncated probe programs (MOV R0, Rd; EXIT patched in) make Ryubing read never-written
  locals. Reports count `ryu_undef_loads`; bisect only on probes where it is 0.
- Failed hypotheses (each measured, no effect on the diff counts): IPA.MUL rounding (`ipa_mul` switch),
  F2F rounding mode, harness descriptor sets, CFG built from sched words.
- Open: LDG through NVN storage descriptors (31 VS programs); residual FS diffs after half_fold
  (179 under 1e-3, 166 under 0.1, 2 under 1), not yet bisected; 1 faithful FS case
  (ceac06d8f2) with max error 3.4.
- D-009 Phase A (2026-11-24): Ryubing via the Snap ShaderHarness generated the cache task-12 loads now,
  `A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\`, documented in docs/SHADER_CACHE.md section 5 and listed in
  docs/REUSE_LEDGER.md. Phase B (this card stays open): ours must match it, then replace it.
- Phase B, 2026-11-24: the half_fold mirror now also folds across blocks when every path brings the same packed value (lift.fold_halves_across_blocks; the emitter keeps cross-block values live and undoes the fold in dispatcher mode). Shipped output unchanged: 300 of 300 sampled programs byte-identical to cache v1. Probe bisect of FS 7dfb9fc921 is blocked: every probe after the first texture fetch gives ryu_undef_loads 2 (the probe artifact).
- Phase B, 2026-11-24 (later): residual FS diffs explained and fixed in the half_fold mirror. (1) The fold now uses an optimistic per-register fixpoint, so it crosses loops that never write the register (FS 7dfb9fc921 was max error 0.188, now bit-equal); (2) fold replacements resolve transitively through MRG chains (FS 93a7a3a584); (3) F2F/F2I F16 sources read lanes through half_lane. New shipped features: I2F with an F16 result; condition-code tests that read only Z and N (EQ, NE, NUM, NAN, EQU, NEU, SFF, SFT) after FSET.CC, enabled only in programs that have both. Shipped output otherwise unchanged (300 of 300 sampled programs byte-identical before the rebuild); cache v1 rebuilt (313.5 s). Tests: 23 passed. Probe lesson: end-of-program register probes read registers that are dead in the real program and can depend on never-written registers; only probes with ryu_undef_loads 0 count (FS 7dfb9fc921 also reads an undefined H1 lane of R55 at 0x1308 in the game shader itself, harmless here because both translators see it the same way). `vkdiff ... --dump DIR` writes raw outputs for pixel checks.
- (Closed by the burn-down below) LDG through NVN storage descriptors (31 VS programs, the 4 not compared in AC3); bindless 6, BRX 4, VMAD 4, VMNMX 3, multisample fetch 3. Phase A stays the cache task-12 loads until the lead decides ours replaces it.
- Failure burn-down, 2026-11-24 (worker, not committed): all 51 cache-v1 failures fixed. LDG through NVN storage descriptors (c[0][0x110+16k], bound at base & ~255, reads unclamped, needs robustBufferAccess); multisample fetch (`2dms`); VMAD/VMNMX/VSETP byte and half selects; BRX jump tables; the bindless handle trace now follows constant-address STL/LDL spills and predicated handle writes (a TEX under P sees a handle written under !P as the value from before), optimistic around loops; geometry stage (ISBERD vertex handle, per-vertex ALD, OUT emit/cut, SPH modes, manifest `geometry`). Oracle per category, all against Ryubing: LDG 31/31 equal (also with in-range patched copies), VMNMX + MS 6/6, BRX 4/4 (arm patching 32/32), geometry 4/4 faithful and compat (new vkdiff mode `gs`: transform-feedback capture of a 21-triangle draw), bindless 4/6 equal.
- Bindless, closed as oracle gaps (2026-11-25, worker): 18a9ec62 is equal only after patching out the @!P0 handle MOV at 0x430 (Ryubing drops a predicated handle). d30675c2 and d87303d3 (lead recheck seed 7: max abs 2.97e-3 on rt0, so input dependent) differ because Ryubing removes 5 depth-compare TEX.B (0x1b68, 0x1b70, 0x1b90, 0x1b98, 0x1c10) and writes 0 to their results: their handle c[2][0x180] | c[2][0x578] is spilled with STL.64 [0x10] at 0x578 and reloaded with LDL.64 at 0xf28, and Ryubing's BindlessElimination only traces a handle to a cbuf or an input/storage load, never through local memory. Ours resolves all 5 to (0x180, 0x578), the texture the program built. Found by alpha-carrier probes (MOV R60, Rk; final alpha from R60): the 4 averaged shadow samples are 0.0 in Ryubing only. Proof (`A:\AI\Recomps\smrpg-work\shaders\proof_spill\work_s7`, seed 7, compat switches): spill-free copies (the handle is also kept in R60 at the depbars 0x350 and 0x538, and the 5 handle copies read R60) are equal, 16384/16384, for both programs, while the originals differ; ours renders the original and the spill-free copy bit-identically.
- Faithful oracle after the burn-down (2026-11-25, worker, seed 1, no compat switches, `A:\AI\Recomps\smrpg-work\shaders\oracle_burn_faithful\report.json`): 500 variants, 56 equal, 444 differ; 697 stage programs: FS 54 equal / 444 differ, VS 190 equal / 9 differ. Every differing program is explained by a compat switch (joined against `oracle_burn_half` and `oracle_burn_all`, same seed): 449 (FS 444, VS 5) are equal with `half_fold` alone, and the other 4 (the LDG VS programs) are equal once `no_fmz,fminmax` are added. None is unexplained. Against `oracle_f2` (lead, before the burn-down: 56 equal, 443 differ, 1 not compared) the one change is the SMAA `_SMAA_PRESET_LOW` variant, whose FS failed to translate then (a former failure) and is now compared: it differs faithfully and is equal with `half_fold`.
- oracle_diff `--budget` now counts from the first vkdiff chunk, so `--budget 1` runs one chunk (prep took over 90 s under machine load and used up the old budget).
- Hand-over notes for the next chat: the shared notes folder, `task-10-shaderxlate-handover.md`.
