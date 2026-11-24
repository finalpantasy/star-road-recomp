"""IR op table: one C body per op (task-5). Our own code.

Spec grammar (field list):  name = small value (stored in a byte slot)
                            name!  = 64-bit immediate
                            name:Rx / Wx / RWx   general register, xzr form
                            name:Rs / Ws / RWs   general register, sp form
                            name:Rv / Wv / RWv   vector register
Special tokens: +fR (reads NZCV), +fW (writes NZCV), +ex (uses exclusive monitor), +mem.
Bodies use the register macros of mode_lift.h / mode_interp.h: RX WX RS WS RV WV, FL, and Ctx *c.
The same body text is expanded by the C emitter (constants for fields) and by the interpreter (record fields).
"""
import re

OPS = {}      # name -> dict(fields=[(name, kind, role, rtype)], body, fR, fW, mem)
ORDER = []


def op(name, spec, body, ex=False):
    toks = spec.split()
    fields = []
    fR = fW = mem = False
    for t in toks:
        if t == "+fR":
            fR = True
        elif t == "+fW":
            fW = True
        elif t == "+mem":
            mem = True
        elif ":" in t:
            n, k = t.split(":")
            m = re.fullmatch(r"(RW|R|W)([xsv])", k)
            fields.append((n, "reg", m.group(1), m.group(2)))
        elif t.endswith("!"):
            fields.append((t[:-1], "imm", None, None))
        else:
            fields.append((t, "byte", None, None))
    assert name not in OPS, name
    nb = sum(1 for f in fields if f[1] in ("reg", "byte"))
    ni = sum(1 for f in fields if f[1] == "imm")
    assert nb <= 10 and ni <= 2, (name, nb, ni)
    OPS[name] = dict(fields=fields, body=body.strip(), fR=fR, fW=fW, mem=mem, ex=ex)
    ORDER.append(name)


# ---------------------------------------------------------------- moves and arithmetic
op("movi", "rd:Wx imm!", "WX(rd, imm);")
op("movk", "sf rd:RWx imm! shift", "WX(rd, ir_m(sf, (RX(rd) & ~(0xFFFFull << shift)) | ((u64)imm << shift)));")
op("addsub_imm", "sf sub rd:Ws rn:Rs imm!", "WS(rd, ir_addsub(sf, sub, RS(rn), imm));")
op("addsubs_imm", "sf sub rd:Wx rn:Rs imm! +fW", "WX(rd, ir_addsubs(sf, sub, RS(rn), imm, FL));")
op("log_imm", "sf opc rd:Ws rn:Rx imm!", "WS(rd, ir_log(sf, opc, RX(rn), imm));")
op("ands_imm", "sf rd:Wx rn:Rx imm! +fW", "WX(rd, ir_ands(sf, RX(rn), imm, FL));")
op("bfm", "sf opc rd:RWx rn:Rx immr imms wmask! tmask!",
   "WX(rd, ir_bfm(sf, opc, RX(rd), RX(rn), immr, imms, wmask, tmask));")
op("extr", "sf rd:Wx rn:Rx rm:Rx lsb", "WX(rd, ir_extr(sf, RX(rn), RX(rm), lsb));")
op("log_reg", "sf opc shift neg rd:Wx rn:Rx rm:Rx amt",
   "{ u64 _b = ir_shift(sf, shift, RX(rm), amt); if (neg) _b = ir_m(sf, ~_b); WX(rd, ir_log(sf, opc, RX(rn), _b)); }")
op("ands_reg", "sf shift neg rd:Wx rn:Rx rm:Rx amt +fW",
   "{ u64 _b = ir_shift(sf, shift, RX(rm), amt); if (neg) _b = ir_m(sf, ~_b); WX(rd, ir_ands(sf, RX(rn), _b, FL)); }")
op("addsub_reg", "sf sub shift rd:Wx rn:Rx rm:Rx amt",
   "WX(rd, ir_addsub(sf, sub, RX(rn), ir_shift(sf, shift, RX(rm), amt)));")
op("addsubs_reg", "sf sub shift rd:Wx rn:Rx rm:Rx amt +fW",
   "WX(rd, ir_addsubs(sf, sub, RX(rn), ir_shift(sf, shift, RX(rm), amt), FL));")
op("addsub_ext", "sf sub option amt rd:Ws rn:Rs rm:Rx",
   "WS(rd, ir_addsub(sf, sub, RS(rn), ir_m(sf, ir_ext(RX(rm), option, amt))));")
op("addsubs_ext", "sf sub option amt rd:Wx rn:Rs rm:Rx +fW",
   "WX(rd, ir_addsubs(sf, sub, RS(rn), ir_m(sf, ir_ext(RX(rm), option, amt)), FL));")
op("adc", "sf sub rd:Wx rn:Rx rm:Rx +fR", "WX(rd, ir_adc(sf, sub, RX(rn), RX(rm), FL[2]));")
op("adcs", "sf sub rd:Wx rn:Rx rm:Rx +fR +fW", "WX(rd, ir_adcs(sf, sub, RX(rn), RX(rm), FL[2], FL));")
op("ccmp_reg", "sf sub cond nzcv rn:Rx rm:Rx +fR +fW", "ir_ccmp(sf, sub, ir_cond(FL, cond), RX(rn), RX(rm), nzcv, FL);")
op("ccmp_imm", "sf sub cond nzcv rn:Rx imm +fR +fW", "ir_ccmp(sf, sub, ir_cond(FL, cond), RX(rn), imm, nzcv, FL);")
op("csel", "sf kind cond rd:Wx rn:Rx rm:Rx +fR", "WX(rd, ir_csel(sf, kind, ir_cond(FL, cond), RX(rn), RX(rm)));")
op("dp2", "sf kind rd:Wx rn:Rx rm:Rx", "WX(rd, ir_dp2(sf, kind, RX(rn), RX(rm)));")
op("dp1", "sf kind rd:Wx rn:Rx", "WX(rd, ir_dp1(sf, kind, RX(rn)));")
op("dp3", "sf kind rd:Wx rn:Rx rm:Rx ra:Rx", "WX(rd, ir_dp3(sf, kind, RX(rn), RX(rm), RX(ra)));")
op("setf", "size rn:Rx +fW",
   "{ u32 _v = (u32)RX(rn); u32 _m = size ? 0xFFFF : 0xFF; int _b = size ? 15 : 7; "
   "FL[0] = (_v >> _b) & 1; FL[1] = (_v & _m) == 0; FL[3] = ((_v >> (_b + 1)) ^ (_v >> _b)) & 1; }")
op("cfinv", "+fR +fW", "FL[2] = !FL[2];")
op("nop", "", ";")
op("dmb", "crm", "ir_dmb(crm);")
op("clrex", "", "c->excl_valid = 0;")

# ---------------------------------------------------------------- loads and stores
def _ld_variants():
    # scalar load/store, immediate offset
    op("ld", "sz sgn d64 rt:Wx rn:Rs imm! +mem", "WX(rt, ir_ld(sz, sgn, d64, RS(rn) + imm));")
    op("ld_wb", "sz sgn d64 wb rt:Wx rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _v = ir_ld(sz, sgn, d64, wb == 1 ? _b : _b + imm); WS(rn, _b + imm); WX(rt, _v); }")
    op("ld_x", "sz sgn d64 opt amt rt:Wx rn:Rs rm:Rx +mem",
       "WX(rt, ir_ld(sz, sgn, d64, RS(rn) + ir_ext(RX(rm), opt, amt)));")
    op("st", "sz rt:Rx rn:Rs imm! +mem", "ir_st(sz, RS(rn) + imm, RX(rt));")
    op("st_wb", "sz wb rt:Rx rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _v = RX(rt); ir_st(sz, wb == 1 ? _b : _b + imm, _v); WS(rn, _b + imm); }")
    op("st_x", "sz opt amt rt:Rx rn:Rs rm:Rx +mem", "ir_st(sz, RS(rn) + ir_ext(RX(rm), opt, amt), RX(rt));")
    op("ldlit", "sz sgn d64 rt:Wx addr! +mem", "WX(rt, ir_ld(sz, sgn, d64, addr));")
    # vector (sz in bytes 1,2,4,8,16)
    op("ldv", "sz rt:Wv rn:Rs imm! +mem", "WV(rt, ir_ldv(sz, RS(rn) + imm));")
    op("ldv_wb", "sz wb rt:Wv rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); v128 _v = ir_ldv(sz, wb == 1 ? _b : _b + imm); WS(rn, _b + imm); WV(rt, _v); }")
    op("ldv_x", "sz opt amt rt:Wv rn:Rs rm:Rx +mem", "WV(rt, ir_ldv(sz, RS(rn) + ir_ext(RX(rm), opt, amt)));")
    op("stv", "sz rt:Rv rn:Rs imm! +mem", "ir_stv(sz, RS(rn) + imm, RV(rt));")
    op("stv_wb", "sz wb rt:Rv rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); v128 _v = RV(rt); ir_stv(sz, wb == 1 ? _b : _b + imm, _v); WS(rn, _b + imm); }")
    op("stv_x", "sz opt amt rt:Rv rn:Rs rm:Rx +mem", "ir_stv(sz, RS(rn) + ir_ext(RX(rm), opt, amt), RV(rt));")
    op("ldlitv", "sz rt:Wv addr! +mem", "WV(rt, ir_ldv(sz, addr));")
    # pairs
    op("ldp", "sz sgn rt:Wx rt2:Wx rn:Rs imm! +mem",
       "{ u64 _a = RS(rn) + imm; int _d = sz == 8 || sgn; u64 _v1 = ir_ld(sz, sgn, _d, _a), _v2 = ir_ld(sz, sgn, _d, _a + sz); WX(rt, _v1); WX(rt2, _v2); }")
    op("ldp_wb", "sz sgn wb rt:Wx rt2:Wx rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _a = wb == 1 ? _b : _b + imm; int _d = sz == 8 || sgn; u64 _v1 = ir_ld(sz, sgn, _d, _a), _v2 = ir_ld(sz, sgn, _d, _a + sz); WS(rn, _b + imm); WX(rt, _v1); WX(rt2, _v2); }")
    op("stp", "sz rt:Rx rt2:Rx rn:Rs imm! +mem",
       "{ u64 _a = RS(rn) + imm; u64 _v1 = RX(rt), _v2 = RX(rt2); ir_st(sz, _a, _v1); ir_st(sz, _a + sz, _v2); }")
    op("stp_wb", "sz wb rt:Rx rt2:Rx rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _a = wb == 1 ? _b : _b + imm; u64 _v1 = RX(rt), _v2 = RX(rt2); ir_st(sz, _a, _v1); ir_st(sz, _a + sz, _v2); WS(rn, _b + imm); }")
    op("ldpv", "sz rt:Wv rt2:Wv rn:Rs imm! +mem",
       "{ u64 _a = RS(rn) + imm; v128 _v1 = ir_ldv(sz, _a), _v2 = ir_ldv(sz, _a + sz); WV(rt, _v1); WV(rt2, _v2); }")
    op("ldpv_wb", "sz wb rt:Wv rt2:Wv rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _a = wb == 1 ? _b : _b + imm; v128 _v1 = ir_ldv(sz, _a), _v2 = ir_ldv(sz, _a + sz); WS(rn, _b + imm); WV(rt, _v1); WV(rt2, _v2); }")
    op("stpv", "sz rt:Rv rt2:Rv rn:Rs imm! +mem",
       "{ u64 _a = RS(rn) + imm; v128 _v1 = RV(rt), _v2 = RV(rt2); ir_stv(sz, _a, _v1); ir_stv(sz, _a + sz, _v2); }")
    op("stpv_wb", "sz wb rt:Rv rt2:Rv rn:RWs imm! +mem",
       "{ u64 _b = RS(rn); u64 _a = wb == 1 ? _b : _b + imm; v128 _v1 = RV(rt), _v2 = RV(rt2); ir_stv(sz, _a, _v1); ir_stv(sz, _a + sz, _v2); WS(rn, _b + imm); }")


_ld_variants()

# ---------------------------------------------------------------- acquire/release, exclusives, LSE atomics
op("ldar", "sz rt:Wx rn:Rs +mem", "WX(rt, ir_ldar(sz, RS(rn)));")
op("stlr", "sz rt:Rx rn:Rs +mem", "ir_stlr(sz, RS(rn), RX(rt));")
op("ldxr", "sz rt:Wx rn:Rs +mem",
   "{ u64 _a = RS(rn); u64 _v = ir_ldar(sz, _a); c->excl_addr = _a; c->excl_val = _v; c->excl_size = sz; c->excl_valid = 1; WX(rt, _v); }")
op("stxr", "sz rs:Wx rt:Rx rn:Rs +mem",
   "{ u64 _a = RS(rn); u64 _ok = 0; if (c->excl_valid && c->excl_addr == _a && c->excl_size == (u32)sz) _ok = ir_cas(sz, _a, c->excl_val, RX(rt)); c->excl_valid = 0; WX(rs, _ok ? 0 : 1); }")
op("ldxp", "sz rt:Wx rt2:Wx rn:Rs +mem",
   "{ u64 _a = RS(rn); u64 _v1 = ir_ldar(sz, _a), _v2 = ir_ldar(sz, _a + sz); c->excl_addr = _a; c->excl_val = _v1; c->scratch[0] = _v2; c->excl_size = sz | 0x100; c->excl_valid = 1; WX(rt, _v1); WX(rt2, _v2); }")
op("stxp", "sz rs:Wx rt:Rx rt2:Rx rn:Rs +mem",
   "{ u64 _a = RS(rn); u64 _ok = 0; if (c->excl_valid && c->excl_addr == _a && c->excl_size == ((u32)sz | 0x100)) _ok = ir_casp(sz, _a, c->excl_val, c->scratch[0], RX(rt), RX(rt2)); c->excl_valid = 0; WX(rs, _ok ? 0 : 1); }")
op("atomic", "sz opc rs:Rx rt:Wx rn:Rs +mem", "WX(rt, ir_atomic(sz, opc, RS(rn), RX(rs)));")
op("atomic_st", "sz opc rs:Rx rn:Rs +mem", "(void)ir_atomic(sz, opc, RS(rn), RX(rs));")
op("cas", "sz rs:RWx rt:Rx rn:Rs +mem", "{ u64 _e = RX(rs); u64 _o = ir_cas_old(sz, RS(rn), _e, RX(rt)); WX(rs, _o); }")
op("casp", "sz rs:RWx rs2:RWx rt:Rx rt2:Rx rn:Rs +mem",
   "{ u64 _o1, _o2; ir_casp_old(sz, RS(rn), RX(rs), RX(rs2), RX(rt), RX(rt2), &_o1, &_o2); WX(rs, _o1); WX(rs2, _o2); }")

# ---------------------------------------------------------------- system
op("mrs", "rt:Wx sysreg! +fR",
   "{ u64 _r = 0; switch (sysreg) { "
   "case SYS_NZCV: _r = ((u64)FL[0] << 31) | ((u64)FL[1] << 30) | ((u64)FL[2] << 29) | ((u64)FL[3] << 28); break; "
   "case SYS_FPCR: _r = c->fpcr; break; case SYS_FPSR: _r = c->fpsr; break; "
   "case SYS_TPIDR_EL0: _r = c->tpidr_el0; break; case SYS_TPIDRRO_EL0: _r = c->tpidrro_el0; break; "
   "case SYS_CNTFRQ: _r = 19200000; break; case SYS_CNTVCT: _r = rt_cntvct(); break; "
   "case SYS_DCZID: _r = 4; break; case SYS_CTR: _r = 0x8444C004ull; break; default: rt_fatal(c, \"mrs\", sysreg); } WX(rt, _r); }")
op("msr", "rt:Rx sysreg! +fR +fW",
   "{ u64 _v = RX(rt); switch (sysreg) { "
   "case SYS_NZCV: FL[0] = (_v >> 31) & 1; FL[1] = (_v >> 30) & 1; FL[2] = (_v >> 29) & 1; FL[3] = (_v >> 28) & 1; break; "
   "case SYS_FPCR: c->fpcr = (u32)_v; break; case SYS_FPSR: c->fpsr = (u32)_v; break; "
   "case SYS_TPIDR_EL0: c->tpidr_el0 = _v; break; default: rt_fatal(c, \"msr\", sysreg); } }")


def gen_header():
    """C macros OP_<name>(fields...) for ops_gen.h (the field order is the spec order)."""
    out = ["/* generated by tools/lifter/ops.py; do not edit */", "#pragma once"]
    for n in ORDER:
        o = OPS[n]
        params = ", ".join(f[0] for f in o["fields"])
        body = o["body"].replace("\n", " \\\n    ")
        out.append("#define OP_%s(%s) do { %s } while (0)" % (n, params, body))
    out.append("enum { " + ", ".join("IROP_%s" % n for n in ORDER) + ", IROP_COUNT };")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    print(len(ORDER), "ops")
