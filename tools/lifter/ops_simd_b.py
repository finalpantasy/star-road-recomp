"""IR ops for the vector (non-scalar) Advanced SIMD class, extension simd_b (task-5). Our own code.

Few ops, each parameterized by constant fields; the bodies call helpers in runtime/ir_simd_b.h whose switches clang
folds in lifted code. Field order follows the spec string. Register lists use up to four register slots (unused slots
repeat the first register, declared RWv/Rv so the lifter's dirty-register dataflow stays correct).
"""
from ops import op


def register_scalar_sat():
    """Append scalar ops after legacy IR IDs, including the control records."""
    op("ns_sat", "rd:Wv rn:Rv rm:Rv size uns shift", """
    vb_u _n = {RV(rn)}, _m = {RV(rm)}, _result = {{0, 0}};
    int _bits = 8 << size;
    s128 _a = vb_getx(&_n, size, 0, uns), _b = vb_getx(&_m, size, 0, uns);
    s128 _value = shift ? vb_shl_lane(c, _a, (s8)_b, _bits, 0, 1, uns) :
                          (uns ? vb_satU(c, _a + _b, _bits) : vb_satS(c, _a + _b, _bits));
    vb_put(&_result, size, 0, (u64)_value & vb_mask(_bits));
    WV(rd, _result.v);
    """)
    op("ns_shn", "rd:Wv rn:Rv size sh kind", """
    vb_u _src = {RV(rn)}, _result = {{0, 0}};
    s128 _value = vb_getx(&_src, size + 1, 0, kind >= 6);
    if (kind & 1) _value = (_value + ((s128)1 << (sh - 1))) >> sh;
    else _value >>= sh;
    int _bits = 8 << size;
    _value = kind >= 6 ? vb_satU(c, _value, _bits) : vb_satS(c, _value, _bits);
    vb_put(&_result, size, 0, (u64)_value & vb_mask(_bits));
    WV(rd, _result.v);
    """)

# ---------------------------------------------------------------- moves, logic, immediates
op("nv_mov", "q rd:Wv rn:Rv", "WV(rd, vb_q(q, RV(rn)));")
op("nv_logic", "q opc rd:RWv rn:Rv rm:Rv", "WV(rd, vb_logic(q, opc, RV(rn), RV(rm), RV(rd)));")
op("nv_movi", "q rd:Wv imm!", "WV(rd, vb_movi(q, imm));")
op("nv_bici", "q bic rd:RWv imm!", "WV(rd, vb_bici(q, bic, imm, RV(rd)));")

# ---------------------------------------------------------------- integer data processing
op("nv_3int", "q size u opc rd:RWv rn:Rv rm:Rv", "WV(rd, vb_3int(c, q, size, u, opc, RV(rn), RV(rm), RV(rd)));")
op("nv_3diff", "q size u opc rd:RWv rn:Rv rm:Rv", "WV(rd, vb_3diff(c, q, size, u, opc, RV(rn), RV(rm), RV(rd)));")
op("nv_2misc", "q size u opc rd:RWv rn:Rv", "WV(rd, vb_2misc_f(c, q, size, u, opc, RV(rn), RV(rd)));")
op("nv_across", "q size u opc rd:Wv rn:Rv", "WV(rd, vb_across_all(c, q, size, u, opc, RV(rn)));")
op("nv_perm", "q size opc rd:Wv rn:Rv rm:Rv", "WV(rd, vb_perm(q, size, opc, RV(rn), RV(rm)));")
op("nv_ext", "q imm4 rd:Wv rn:Rv rm:Rv", "WV(rd, vb_ext(q, imm4, RV(rn), RV(rm)));")
op("nv_shift", "q esz sh opc u rd:RWv rn:Rv", "WV(rd, vb_shift(c, q, esz, sh, opc, u, RV(rn), RV(rd)));")
op("nv_shn", "q esz sh kind rd:RWv rn:Rv", "WV(rd, vb_shn(c, q, esz, sh, kind, RV(rn), RV(rd)));")
op("nv_shll", "q esz sh u rd:Wv rn:Rv", "WV(rd, vb_shll(q, esz, sh, u, RV(rn)));")
op("nv_elem", "q size u opc idx rd:RWv rn:Rv rm:Rv", "WV(rd, vb_elem(c, q, size, u, opc, idx, RV(rn), RV(rm), RV(rd)));")
op("nv_tbl", "q len op rd:RWv rn:Rv rn2:Rv rn3:Rv rn4:Rv rm:Rv",
   "WV(rd, vb_tbl(q, len, op, RV(rd), RV(rn), RV(rn2), RV(rn3), RV(rn4), RV(rm)));")

# ---------------------------------------------------------------- copy
op("nv_dupe", "q esz idx rd:Wv rn:Rv", "WV(rd, vb_dupe(q, esz, idx, RV(rn)));")
op("nv_dupg", "q esz rd:Wv rn:Rx", "WV(rd, vb_dup(q, esz, RX(rn)));")
op("nv_insg", "esz idx rd:RWv rn:Rx", "WV(rd, vb_ins(esz, idx, RV(rd), RX(rn)));")
op("nv_inse", "esz idx idx2 rd:RWv rn:Rv", "WV(rd, vb_inse(esz, idx, idx2, RV(rd), RV(rn)));")
op("nv_umov", "q esz sgn idx rd:Wx rn:Rv", "WX(rd, vb_umov(q, esz, sgn, idx, RV(rn)));")

# ---------------------------------------------------------------- floating point, vector
op("nv_3fp", "q sz a u opc rd:RWv rn:Rv rm:Rv", "WV(rd, vb_3fp(c, q, sz, a, u, opc, RV(rn), RV(rm), RV(rd)));")
op("nv_2fp", "q sz a u opc rd:RWv rn:Rv", "WV(rd, vb_2fp(c, q, sz, a, u, opc, RV(rn), RV(rd)));")
op("nv_elemf", "q sz u opc idx rd:RWv rn:Rv rm:Rv", "WV(rd, vb_elemf(c, q, sz, u, opc, idx, RV(rn), RV(rm), RV(rd)));")
op("nv_fixcvt", "q sz to_int u fbits rd:Wv rn:Rv", "WV(rd, vb_fixcvt(c, q, sz, to_int, u, fbits, RV(rn)));")

# ---------------------------------------------------------------- structure loads and stores
op("nv_ldm", "q esz opc wb rt:RWv rt2:RWv rt3:RWv rt4:RWv rn:RWs rm:Rx imm! +mem",
   "{ u64 _b = RS(rn); v128 _t[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; vb_ldm(_t, _b, q, esz, opc); "
   "WV(rt, _t[0]); if (vb_nregs(opc) > 1) WV(rt2, _t[1]); if (vb_nregs(opc) > 2) WV(rt3, _t[2]); "
   "if (vb_nregs(opc) > 3) WV(rt4, _t[3]); if (wb) WS(rn, _b + (wb == 1 ? (u64)imm : RX(rm))); }")
op("nv_stm", "q esz opc wb rt:Rv rt2:Rv rt3:Rv rt4:Rv rn:RWs rm:Rx imm! +mem",
   "{ u64 _b = RS(rn); v128 _t[4] = {RV(rt), RV(rt2), RV(rt3), RV(rt4)}; vb_stm(_t, _b, q, esz, opc); "
   "if (wb) WS(rn, _b + (wb == 1 ? (u64)imm : RX(rm))); }")
op("nv_ldl", "esz sel idx wb rt:RWv rt2:RWv rt3:RWv rt4:RWv rn:RWs rm:Rx imm! +mem",
   "{ u64 _b = RS(rn); v128 _t[4] = {RV(rt), RV(rt2), RV(rt3), RV(rt4)}; vb_ldl(_t, _b, esz, sel, idx); "
   "WV(rt, _t[0]); if (sel > 1) WV(rt2, _t[1]); if (sel > 2) WV(rt3, _t[2]); if (sel > 3) WV(rt4, _t[3]); "
   "if (wb) WS(rn, _b + (wb == 1 ? (u64)imm : RX(rm))); }")
op("nv_stl", "esz sel idx wb rt:Rv rt2:Rv rt3:Rv rt4:Rv rn:RWs rm:Rx imm! +mem",
   "{ u64 _b = RS(rn); v128 _t[4] = {RV(rt), RV(rt2), RV(rt3), RV(rt4)}; vb_stl(_t, _b, esz, sel, idx); "
   "if (wb) WS(rn, _b + (wb == 1 ? (u64)imm : RX(rm))); }")
op("nv_ldr", "q esz sel wb rt:RWv rt2:RWv rt3:RWv rt4:RWv rn:RWs rm:Rx imm! +mem",
   "{ u64 _b = RS(rn); v128 _t[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; vb_ldr(_t, _b, q, esz, sel); "
   "WV(rt, _t[0]); if (sel > 1) WV(rt2, _t[1]); if (sel > 2) WV(rt3, _t[2]); if (sel > 3) WV(rt4, _t[3]); "
   "if (wb) WS(rn, _b + (wb == 1 ? (u64)imm : RX(rm))); }")
