"""IR ops for scalar floating point and Advanced SIMD scalar instructions (extension simd_a, task-5). Our own code.

Few ops, parameterized by constant byte fields; the bodies call always_inline helpers of runtime/ir_simd_a.h, so in
lifted code the constant fields fold away. ftype: 0 single, 1 double, 3 half.
"""
from ops import op

# arithmetic: kind 0 fmul 1 fdiv 2 fadd 3 fsub 4 fmax 5 fmin 6 fmaxnm 7 fminnm 8 fnmul 9 fabd 10 fmulx
op("fp2", "ftype kind rd:Wv rn:Rv rm:Rv", "WV(rd, sa_fp2(c, ftype, kind, RV(rn), RV(rm)));")
# kind = ARM opcode field: 0 fmov 1 fabs 2 fneg 3 fsqrt 4 fcvt->S 5 fcvt->D 7 fcvt->H 8..12 frintn/p/m/z/a 14 frintx 15 frinti
op("fp1", "ftype kind rd:Wv rn:Rv", "WV(rd, sa_fp1(c, ftype, kind, RV(rn)));")
# kind 0 fmadd 1 fmsub 2 fnmadd 3 fnmsub
op("fp3", "ftype kind rd:Wv rn:Rv rm:Rv ra:Rv", "WV(rd, sa_fp3(c, ftype, kind, RV(rn), RV(rm), RV(ra)));")
op("fcmp", "ftype e zero rn:Rv rm:Rv +fW", "sa_fcmp(c, ftype, e, RV(rn), RV(rm), zero, FL);")
op("fccmp", "ftype e cond nzcv rn:Rv rm:Rv +fR +fW",
   "{ if (ir_cond(FL, cond)) sa_fcmp(c, ftype, e, RV(rn), RV(rm), 0, FL); "
   "else { FL[0] = (nzcv >> 3) & 1; FL[1] = (nzcv >> 2) & 1; FL[2] = (nzcv >> 1) & 1; FL[3] = nzcv & 1; } }")
op("fcsel", "ftype cond rd:Wv rn:Rv rm:Rv +fR",
   "WV(rd, sa_mk(ir_cond(FL, cond) ? sa_get(ftype, RV(rn)) : sa_get(ftype, RV(rm))));")
op("fmovi", "rd:Wv imm!", "WV(rd, sa_mk(imm));")
# FMOV general <-> vector: sz is the access size in bytes (2, 4, 8); hi = top half of the q register
op("fmov_tx", "sz hi rd:Wx rn:Rv", "WX(rd, sa_vx(sz, hi, RV(rn)));")
op("fmov_fx", "sz rd:Wv rn:Rx", "WV(rd, sa_xv(sz, RX(rn)));")
op("fmov_hx", "rd:RWv rn:Rx", "WV(rd, sa_hi(RV(rd), RX(rn)));")
# float to integer: rm 0 N 1 P 2 M 3 Z 4 A (ties away); fbits for the fixed-point forms
op("fcvti", "ftype sf rm uns fbits rd:Wx rn:Rv", "WX(rd, sa_fcvti(c, ftype, sf, rm, uns, fbits, RV(rn)));")
# integer register to float (SCVTF/UCVTF, with fixed-point fbits)
op("cvtf", "ftype sf uns fbits rd:Wv rn:Rx", "WV(rd, sa_cvtf(c, ftype, sf, uns, fbits, RX(rn)));")
# Advanced SIMD scalar class
op("dup_s", "sz idx rd:Wv rn:Rv", "WV(rd, sa_dup(sz, idx, RV(rn)));")
# SCVTF/UCVTF scalar with the integer in a vector register (sf: 64-bit source)
op("cvtf_v", "ftype sf uns fbits rd:Wv rn:Rv", "WV(rd, sa_cvtf(c, ftype, sf, uns, fbits, (u64)RV(rn)[0]));")
op("addp_s", "rd:Wv rn:Rv", "WV(rd, sa_addp(RV(rn)));")
# kind 0 faddp 1 fmaxnmp 2 fmaxp 3 fminnmp 4 fminp
op("fpair", "ftype kind rd:Wv rn:Rv", "WV(rd, sa_fpair(c, ftype, kind, RV(rn)));")
# kind 0 fmla 1 fmls 2 fmul 3 fmulx (by element); rd is the accumulator
op("felem", "ftype kind idx rd:RWv rn:Rv rm:Rv", "WV(rd, sa_felem(c, ftype, kind, RV(rd), RV(rn), RV(rm), idx));")
# three-same: kind 0 fabd 1 fmulx 2 frecps 3 frsqrts 4 fcmeq 5 fcmge 6 fcmgt 7 facge 8 facgt
op("f3same", "ftype kind rd:Wv rn:Rv rm:Rv", "WV(rd, sa_f3same(c, ftype, kind, RV(rn), RV(rm)));")
# two-reg misc: kind 0 frecpe 1 frsqrte 2 frecpx
op("fest", "ftype kind rd:Wv rn:Rv", "WV(rd, sa_fest(c, ftype, kind, RV(rn)));")
# compare against zero: kind 0 fcmgt 1 fcmge 2 fcmeq 3 fcmle 4 fcmlt
op("fcmz", "ftype kind rd:Wv rn:Rv", "WV(rd, sa_fcmz(c, ftype, kind, RV(rn)));")
# float to integer, result left in a vector register: rm as fcvti; sf selects 64-bit
op("fcvti_v", "ftype sf rm uns rd:Wv rn:Rv", "WV(rd, sa_mk(sa_fcvti(c, ftype, sf, rm, uns, 0, RV(rn))));")
