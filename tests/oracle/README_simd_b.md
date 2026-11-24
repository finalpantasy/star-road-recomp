# simd_b: vector Advanced SIMD (NEON) extension

Files: `tools/lifter/ops_simd_b.py` (IR ops), `tools/lifter/lower_simd_b.py` (word to op), `runtime/ir_simd_b.h` (C semantics).
Select it with `SMRPG_EXT=simd_b` (alone) or the default `simd_a,simd_b`.

## What is covered (all vector, non-scalar Advanced SIMD seen in main .text)

| Op | Instructions |
| --- | --- |
| `nv_mov`, `nv_logic` | ORR Vd,Vn,Vn (MOV), AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF |
| `nv_movi`, `nv_bici` | MOVI, MVNI, FMOV (vector immediate), ORR/BIC (vector immediate), MOVI Dd |
| `nv_3int` | integer three-same: ADD SUB MUL MLA MLS PMUL (8-bit) CMEQ CMTST CMGT CMGE CMHI CMHS (reg) SMAX SMIN SABD SABA (and U forms) SHADD SRHADD SHSUB SQADD SQSUB SSHL SQSHL SRSHL SQRSHL SQDMULH SQRDMULH ADDP SMAXP SMINP |
| `nv_3diff` | long, wide, narrowing: SADDL SADDW SSUBL SSUBW ADDHN RADDHN SUBHN RSUBHN SABAL SABDL SMLAL SMLSL SMULL SQDMLAL SQDMLSL SQDMULL PMULL (8-bit), "2" forms |
| `nv_2misc` | REV16/32/64 SADDLP SADALP SUQADD CLS CLZ CNT NOT RBIT SQABS SQNEG CMxx #0 ABS NEG XTN SQXTN SQXTUN UQXTN SHLL |
| `nv_3fp`, `nv_2fp`, `nv_elemf` | FP three-same, FP two-reg misc, FP by element (single and double) |
| `nv_across` | ADDV SADDLV UADDLV SMAXV UMAXV SMINV UMINV FMAXV FMINV FMAXNMV FMINNMV |
| `nv_perm`, `nv_ext`, `nv_tbl` | ZIP UZP TRN, EXT, TBL/TBX with 1 to 4 table registers |
| `nv_dupe`, `nv_dupg`, `nv_insg`, `nv_inse`, `nv_umov` | DUP (element, general), INS (general, element), UMOV, SMOV |
| `nv_shift`, `nv_shn`, `nv_shll`, `nv_fixcvt` | shift by immediate (SSHR SSRA SRSHR SRSRA SRI SHL SLI SQSHLU SQSHL UQSHL), narrowing shifts, SSHLL/USHLL, fixed-point SCVTF/UCVTF/FCVTZS/FCVTZU |
| `nv_elem` | integer by element: MUL MLA MLS SQDMULH SQRDMULH SMLAL SMLSL SMULL SQDMLAL SQDMLSL SQDMULL (+U) |
| `nv_ldm`, `nv_stm`, `nv_ldl`, `nv_stl`, `nv_ldr` | LD1..LD4 / ST1..ST4 multiple, single lane, LD1R..LD4R, with no, immediate or register post-index writeback |

FP: IEEE semantics follow the Arm ARM pseudocode: FPCR.FZ (input and output flush, IDC/UFC), FPCR.DN, FPCR.RMode, NaN
propagation order, default NaN, cumulative flags IOC DZC OFC UFC IXC IDC, FMA fused, FRECPE/FRSQRTE exact estimate
tables, FRECPS/FRSQRTS fused, FMAX/FMIN/FMAXNM/FMINNM NaN rules, saturating FCVT*, rounding FRINT*, half-precision
conversion in FCVTN/FCVTL. Integer saturation sets FPSR.QC.

Implementation: single-precision vector ops with FPCR.FZ/DN/RMode = 0 and no NaN inputs run as SSE/FMA with the host
MXCSR flags mapped to FPSR; every other case runs lane by lane through the scalar engine in `ir_simd_b.h`. The host
MXCSR is assumed to hold its default value (0x1F80) on entry and is left at that value.

## Not covered

* Crypto extension (AES, SHA, PMULL 64x64) and CRC32: no such word in main .text (checked with coverage.py).
* Three-same-extra class (SDOT, FCMLA, FCADD, SQRDMLAH) and half-precision arithmetic: ARMv8.0 target, none present.
* FCVTN/FCVTL honor FPCR.RMode, FZ and DN but not FPCR.FZ16 or AHP.
* FRSQRTS is computed as `fma(-a, b, 3) * 0.5` (double based for single precision), so results for extreme
  magnitudes (|a*b| near FLT_MAX) can differ from a true single rounding in the last bit; double precision overflow
  edge cases are not exact.
* The tiny-result test for FPCR.FZ output flush uses the rounded host result plus the host underflow flag, so a value
  that is below the smallest normal before rounding but exactly equal to it after rounding in the same step is
  handled through the flag only.

## Known Unicorn differences

No semantic disagreement with Unicorn was found. The only difference is a harness limit: runner.c NDIFF is 64, so
64-byte stores (ST4 or ST1 x4 with Q=1) show as memory mismatches in the stock oracle. With NDIFF raised to 128 they
compare clean. Some structure load/store cases are skipped by Unicorn (invalid addresses).

## Oracle results (SMRPG_EXT=simd_b, 100 words per op, 10 states)

* `report_b_final.json`: 28 ops (all but nv_stm), compared 653740, mismatches 0, skipped 4080.
* `report_b_stm.json`: nv_stm with NDIFF=128, compared 27540, mismatches 0.
* Extra synthetic-word runs per encoding class, including `--fpcr-modes` for nv_3fp and nv_2fp: 0 mismatches.
* Coverage on main .text: 8496049 lowered before, 8905123 after; the 306651 remaining words are scalar FP and
  Advanced SIMD scalar (simd_a).
