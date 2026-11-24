# simd_a: scalar floating point and Advanced SIMD scalar

Files: `tools/lifter/ops_simd_a.py` (21 IR ops), `tools/lifter/lower_simd_a.py` (decoder), `runtime/ir_simd_a.h` (helpers, prefix `sa_` / `SA_`).
Run any tool with `SMRPG_EXT=simd_a` to load only this extension.

## Covered
Every scalar-FP and Advanced SIMD scalar word of main .text lowers (306,635 of 306,635; 0 left). Words by ops:
fp2 (fadd fsub fmul fdiv fnmul fmax fmin fmaxnm fminnm, plus fabd and fmulx kinds), fp1 (fmov fabs fneg fsqrt fcvt s/d/h frintn/p/m/z/a/x/i), fp3 (fmadd fmsub fnmadd fnmsub), fcmp (fcmp/fcmpe, reg and zero),
fccmp (fccmp/fccmpe), fcsel, fmovi, fmov_tx / fmov_fx / fmov_hx (general register moves, including `d[1]` top half), fcvti (fcvt{n,m,p,z,a}{s,u}, fixed-point fcvtzs/u), cvtf (scvtf/ucvtf, integer and fixed-point),
dup_s, cvtf_v (scvtf/ucvtf scalar on a vector register), addp_s, fpair (faddp, fmaxp, fminp, fmaxnmp, fminnmp), felem (fmla fmls fmul fmulx by element),
f3same (fabd fmulx frecps frsqrts fcmeq fcmge fcmgt facge facgt), fest (frecpe frsqrte frecpx), fcmz (compare with zero), fcvti_v (fcvt* result in a vector register).
Main .text uses only: fp1, fp2, fcmp, fccmp, fcsel, fmovi, fmov_tx/fx, fcvti, cvtf, dup_s, cvtf_v, addp_s, fpair (faddp), felem (fmul), f3same (fabd). No fmadd, no half precision, no `d[1]` moves.

## Method
Fast path (FPCR == 0, ordinary finite operands): SSE/FMA result plus exact flags from error-free transformations (TwoSum, double-width products, fma remainders); no MXCSR use. Needs host MXCSR at default (round to nearest, FTZ/DAZ off).
Soft path (everything else, any FPCR): integer implementation of the ARM pseudocode (FPRound, FPProcessNaNs, FPToFixed, estimates with the table algorithm). Handles DN, FZ, RMode, IOC/DZC/OFC/UFC/IXC/IDC.
Tininess is detected before rounding as ARM requires.

## Evidence (oracle, Unicorn 2.1.4)
- Real main words, `--per-op 100 --states 10`, tag `a_final`: 306,420 compared, 0 mismatches, 16 ops (report `A:\AI\Recomps\smrpg-work\build\tests\report_a_final.json`).
- Synthetic encodings (all kinds and ftypes, FP-rich register values; driver was a throwaway script, not committed), tag `as1`: 663,876 compared, 0 mismatches, 21 ops; `as2` with `--fpcr-modes` (RMode, FZ, DN random): 54,636 compared, 0 mismatches.
- Harness sensitivity checked: two deliberate bugs in the header gave 154 mismatches.

## Not verified or not handled
- Half precision (ftype 3) arithmetic, compares, int conversions and fmov: Unicorn raises an undefined-instruction error for them (all skipped), so they follow the ARM pseudocode untested. FCVT half<->single/double IS tested. Half does not occur in main .text.
- FPCR.FZ16 on conversions, FPCR.AHP, FPCR.AH/FIZ/NEP (FEAT_AFP), trapped exceptions (IDE, IXE ...): not handled.
- Not in main and so only covered by the synthetic run: fp3, fmov_hx, felem fmla/fmls/fmulx, f3same except fabd, fest, fcmz, fcvti_v, fpair except faddp.
- Scalar integer SIMD (SQADD, SSHR, ABS, ...) and scalar shifts do not occur in main; not implemented. FRINT32/64, FJCVTZS, FP16 by-element forms not implemented.
- Known Unicorn differences from the ARM pseudocode: none found.
- The three-operand NaN rule, FNMUL NaN sign, FRECPS/FRSQRTS infinity*zero cases all agree with Unicorn.
