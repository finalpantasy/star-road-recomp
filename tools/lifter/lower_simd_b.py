"""Advanced SIMD (vector) word -> IR op (simd_b extension, task-5). Our own code.

decode_lower(word, pc) returns (opname, fields) or None. It never raises. Covers the vector (non-scalar)
Advanced SIMD classes; the scalar classes belong to simd_a. Op bodies are in ops_simd_b.py, helpers in
runtime/ir_simd_b.h. Register lists (LDn/STn/TBL) use up to four register fields; unused slots repeat the
first register (the body ignores them).
"""


def _b(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def decode_lower(word, pc):
    try:
        return _decode(word & 0xFFFFFFFF)
    except Exception:
        return None


def _decode(w):
    if w >> 31:
        return None
    if _b(w, 30, 30):
        scalar = _scalar_sat(w)
        if scalar:
            return scalar
    top = _b(w, 28, 24)
    if top in (0x0E, 0x0F):
        return _simd(w, top)
    # structure loads and stores: 0 Q 0011 0xx ... (bits 29:24 = 001100 / 001101 / 001110? see _ldst)
    if _b(w, 29, 24) in (0b001100, 0b001101):
        return _ldst_struct(w)
    return None


def _scalar_sat(w):
    rd, rn, rm = _b(w, 4, 0), _b(w, 9, 5), _b(w, 20, 16)
    unsigned = _b(w, 29, 29)
    size = _b(w, 23, 22)
    # SQADD/UQADD and SQSHL/UQSHL (register).
    mask = w & 0xDF20FC00
    if mask in (0x5E200C00, 0x5E204C00):
        return "ns_sat", dict(rd=rd, rn=rn, rm=rm, size=size, uns=unsigned, shift=int(mask == 0x5E204C00))
    # SQXTN/UQXTN, the zero-shift case of non-rounding saturating narrowing.
    if w & 0xDF3FFC00 == 0x5E214800 and size != 3:
        return "ns_shn", dict(rd=rd, rn=rn, size=size, sh=0, kind=4 + 2 * unsigned)
    # SQSHRN/SQRSHRN and their unsigned forms.
    if w & 0xDF80F400 == 0x5F009400:
        immh = _b(w, 22, 19)
        size = immh.bit_length() - 1
        if 0 <= size < 3:
            sh = (16 << size) - _b(w, 22, 16)
            return "ns_shn", dict(rd=rd, rn=rn, size=size, sh=sh, kind=4 + _b(w, 11, 11) + 2 * unsigned)
    return None


# --------------------------------------------------------------------------------------------- data processing
def _simd(w, top):
    q, u = _b(w, 30, 30), _b(w, 29, 29)
    rd, rn, rm = _b(w, 4, 0), _b(w, 9, 5), _b(w, 20, 16)
    if top == 0x0F:
        if _b(w, 10, 10) == 0:
            return _indexed(w, q, u, rd, rn)
        if _b(w, 22, 19) == 0:
            return _modimm(w, q, u, rd)
        return _shift_imm(w, q, u, rd, rn)
    b21, b10, b11, b15 = _b(w, 21, 21), _b(w, 10, 10), _b(w, 11, 11), _b(w, 15, 15)
    size = _b(w, 23, 22)
    if b21:
        if b10:
            return _three_same(w, q, u, size, rd, rn, rm)
        if b11:
            if _b(w, 20, 17) == 0b0000:
                return _two_misc(w, q, u, size, rd, rn)
            if _b(w, 20, 17) == 0b1000:
                return _across(w, q, u, size, rd, rn)
            return None
        return _three_diff(w, q, u, size, rd, rn, rm)
    if b15:
        return None     # three-same extra (dot product, FCMLA, FCADD, SQRDMLAH): not in ARMv8.0
    if b10:
        return _copy(w, q, u, rd, rn)
    if u == 0 and _b(w, 11, 10) == 0b10:
        return _permute(w, q, size, rd, rn, rm)
    if u == 1 and size == 0:
        imm4 = _b(w, 14, 11)
        if not q and imm4 & 8:
            return None
        return "nv_ext", dict(q=q, imm4=imm4, rd=rd, rn=rn, rm=rm)
    if u == 0 and _b(w, 11, 10) == 0 and size == 0:
        ln = _b(w, 14, 13)
        regs = [(rn + k) % 32 for k in range(ln + 1)]
        regs += [rn] * (4 - len(regs))
        return "nv_tbl", dict(q=q, len=ln, op=_b(w, 12, 12), rd=rd, rn=regs[0], rn2=regs[1], rn3=regs[2], rn4=regs[3], rm=rm)
    return None


def _three_same(w, q, u, size, rd, rn, rm):
    opc = _b(w, 15, 11)
    if opc == 0b00011:
        o = (u << 2) | size
        if o == 2 and rm == rn:
            return "nv_mov", dict(q=q, rd=rd, rn=rn)
        return "nv_logic", dict(q=q, opc=o, rd=rd, rn=rn, rm=rm)
    if opc >= 0b11000:
        a, sz = size >> 1, size & 1
        if sz and not q:
            return None
        ok = {(0, 0x18), (0, 0x19), (0, 0x1A), (0, 0x1B), (0, 0x1C), (0, 0x1E), (0, 0x1F),
              (1, 0x18), (1, 0x1A), (1, 0x1B), (1, 0x1C), (1, 0x1D), (1, 0x1E), (1, 0x1F)}
        if (u, opc) not in ok:
            return None
        if (u, opc) in ((0, 0x1B), (0, 0x1C), (1, 0x1B), (1, 0x1F)) and a:
            return None
        return "nv_3fp", dict(q=q, sz=sz, a=a, u=u, opc=opc, rd=rd, rn=rn, rm=rm)
    if size == 3 and not q:
        return None
    # element sizes allowed (bit mask over size 0..3)
    allow = {0x00: 7, 0x01: 15, 0x02: 7, 0x04: 7, 0x05: 15, 0x06: 15, 0x07: 15, 0x08: 15, 0x09: 15, 0x0A: 15, 0x0B: 15,
             0x0C: 7, 0x0D: 7, 0x0E: 7, 0x0F: 7, 0x10: 15, 0x11: 15, 0x12: 7, 0x13: 7, 0x14: 7, 0x15: 7, 0x16: 6,
             0x17: 15}
    if opc not in allow or not (allow[opc] >> size) & 1:
        return None
    if opc == 0x13 and u and size != 0:      # PMUL
        return None
    if opc == 0x17 and u:
        return None
    if opc == 0x12 and False:
        return None
    return "nv_3int", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn, rm=rm)


def _three_diff(w, q, u, size, rd, rn, rm):
    opc = _b(w, 15, 12)
    if opc > 14 or size == 3:
        return None
    if u and opc in (9, 11, 13, 14):
        return None
    if opc in (9, 11, 13) and size == 0:
        return None
    if opc == 14 and size != 0:
        return None
    return "nv_3diff", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn, rm=rm)


_FP_MISC_OK = {
    # (u, a, opc)
    (0, 1, 0x0C), (0, 1, 0x0D), (0, 1, 0x0E), (0, 1, 0x0F), (1, 1, 0x0C), (1, 1, 0x0D), (1, 1, 0x0F),
    (0, 0, 0x16), (1, 0, 0x16), (0, 0, 0x17),
    (0, 0, 0x18), (0, 1, 0x18), (0, 0, 0x19), (0, 1, 0x19), (0, 0, 0x1A), (0, 1, 0x1A), (0, 0, 0x1B), (0, 1, 0x1B),
    (0, 0, 0x1C), (0, 1, 0x1C), (0, 0, 0x1D), (0, 1, 0x1D),
    (1, 0, 0x18), (1, 0, 0x19), (1, 1, 0x19), (1, 0, 0x1A), (1, 1, 0x1A), (1, 0, 0x1B), (1, 1, 0x1B),
    (1, 0, 0x1C), (1, 1, 0x1C), (1, 0, 0x1D), (1, 1, 0x1D), (1, 1, 0x1F),
}


def _two_misc(w, q, u, size, rd, rn):
    opc = _b(w, 16, 12)
    if opc >= 0x0C and (opc in (0x0C, 0x0D, 0x0E, 0x0F) or opc >= 0x16):
        a, sz = size >> 1, size & 1
        if (u, a, opc) not in _FP_MISC_OK:
            return None
        if opc == 0x16 and (u == 1) and not sz:
            return None
        if opc == 0x1C and a and sz:
            return None
        if opc in (0x16, 0x17):
            if opc == 0x17 and sz and False:
                return None
            # 0x16: FCVTN (u=0), FCVTXN (u=1): sz=0 -> half (u=0) ; 0x17 FCVTL: sz=0 half->single, sz=1 single->double
            return "nv_2fp", dict(q=q, sz=sz, a=a, u=u, opc=opc, rd=rd, rn=rn)
        if sz and not q:
            return None
        return "nv_2fp", dict(q=q, sz=sz, a=a, u=u, opc=opc, rd=rd, rn=rn)
    # integer
    ok0 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 18, 20}
    ok1 = {0, 2, 3, 4, 5, 6, 7, 8, 9, 11, 18, 19, 20}
    if opc not in (ok1 if u else ok0):
        return None
    # element size constraints
    if opc in (0, ):
        if size == 3 or (u == 0 and False):
            return None
        if u == 0:
            pass
    if (u, opc) == (0, 0) and size == 3:
        return None
    if (u, opc) == (1, 0) and size > 1:
        return None
    if (u, opc) == (0, 1) and size != 0:
        return None
    if opc in (2, 6) and size == 3:
        return None
    if opc in (4,) and size == 3:
        return None
    if (u, opc) == (0, 5) and size != 0:
        return None
    if (u, opc) == (1, 5) and size > 1:
        return None
    if opc in (18, 19, 20) and size == 3:
        return None
    if (u, opc) == (1, 19) and False:
        return None
    if opc in (3, 7, 8, 9, 10, 11) and size == 3 and not q:
        return None
    if size == 3 and not q and opc not in (18, 19, 20):
        return None
    return "nv_2misc", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn)


def _across(w, q, u, size, rd, rn):
    opc = _b(w, 16, 12)
    if opc in (0x03, 0x0A, 0x1A):
        if size == 3 or (size == 2 and not q):
            return None
        return "nv_across", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn)
    if opc == 0x1B and not u:
        if size == 3 or (size == 2 and not q):
            return None
        return "nv_across", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn)
    if u and opc in (0x0C, 0x0F):
        if (size & 1) or not q:
            return None
        return "nv_across", dict(q=q, size=size, u=u, opc=opc, rd=rd, rn=rn)
    return None


def _permute(w, q, size, rd, rn, rm):
    opc = _b(w, 14, 12)
    if opc in (0, 4):
        return None
    if size == 3 and not q:
        return None
    return "nv_perm", dict(q=q, size=size, opc=opc, rd=rd, rn=rn, rm=rm)


def _copy(w, q, op, rd, rn):
    imm5, imm4 = _b(w, 20, 16), _b(w, 14, 11)
    if _b(w, 23, 21) != 0:
        return None
    if imm5 & 15 == 0:
        return None
    esz = (imm5 & -imm5).bit_length() - 1
    idx = imm5 >> (esz + 1)
    if op:
        if not q:
            return None
        idx2 = imm4 >> esz
        return "nv_inse", dict(esz=esz, idx=idx, idx2=idx2, rd=rd, rn=rn)
    if imm4 == 0:
        if esz == 3 and not q:
            return None
        return "nv_dupe", dict(q=q, esz=esz, idx=idx, rd=rd, rn=rn)
    if imm4 == 1:
        if esz == 3 and not q:
            return None
        return "nv_dupg", dict(q=q, esz=esz, rd=rd, rn=rn)
    if imm4 == 3:
        if not q:
            return None
        return "nv_insg", dict(esz=esz, idx=idx, rd=rd, rn=rn)
    if imm4 == 5:      # SMOV
        if esz == 3 or (esz == 2 and not q):
            return None
        return "nv_umov", dict(q=q, esz=esz, sgn=1, idx=idx, rd=rd, rn=rn)
    if imm4 == 7:
        if (esz == 3) != bool(q) or esz > 3:
            return None
        return "nv_umov", dict(q=q, esz=esz, sgn=0, idx=idx, rd=rd, rn=rn)
    return None


def _modimm(w, q, op, rd):
    cmode = _b(w, 15, 12)
    if _b(w, 11, 10) != 0b01:
        return None
    imm8 = (_b(w, 18, 16) << 5) | _b(w, 9, 5)
    rep = lambda v, n, bits: sum((v & ((1 << bits) - 1)) << (bits * i) for i in range(n))
    c31 = cmode >> 1
    if c31 < 4:
        imm = rep(imm8 << (8 * c31), 2, 32)
    elif c31 < 6:
        imm = rep(imm8 << (8 * (c31 - 4)), 4, 16)
    elif c31 == 6:
        imm = rep((imm8 << 8) | 0xFF if cmode & 1 == 0 else (imm8 << 16) | 0xFFFF, 2, 32)
    else:
        if cmode & 1 == 0:
            if op == 0:
                imm = rep(imm8, 8, 8)
            else:
                imm = sum(0xFF << (8 * i) for i in range(8) if (imm8 >> i) & 1)
        else:
            a, bb, cdefgh = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
            if op == 0:
                f32 = (a << 31) | ((bb ^ 1) << 30) | ((0x1F if bb else 0) << 25) | (cdefgh << 19)
                imm = rep(f32, 2, 32)
            else:
                if not q:
                    return None
                imm = (a << 63) | ((bb ^ 1) << 62) | ((0xFF if bb else 0) << 54) | (cdefgh << 48)
            return "nv_movi", dict(q=q, rd=rd, imm=imm)
        return "nv_movi", dict(q=q, rd=rd, imm=imm)
    # cmode<3:1> in 0..6: the MOVI/MVNI/ORR/BIC split
    if cmode & 1 and c31 < 6:
        return "nv_bici", dict(q=q, bic=op, rd=rd, imm=imm)
    if op:
        imm = (~imm) & 0xFFFFFFFFFFFFFFFF
    return "nv_movi", dict(q=q, rd=rd, imm=imm)


def _shift_imm(w, q, u, rd, rn):
    immh, immb, opc = _b(w, 22, 19), _b(w, 18, 16), _b(w, 15, 11)
    hb = immh.bit_length() - 1       # element size log2 (0..3)
    immhb = (immh << 3) | immb
    esize = 8 << hb
    if hb == 3 and not q and opc not in (0x10, 0x11, 0x12, 0x13, 0x14):
        return None
    right = 2 * esize - immhb
    left = immhb - esize
    if opc in (0x00, 0x02, 0x04, 0x06):
        return "nv_shift", dict(q=q, esz=hb, sh=right, opc=opc >> 1, u=u, rd=rd, rn=rn)
    if opc == 0x08 and u:
        return "nv_shift", dict(q=q, esz=hb, sh=right, opc=4, u=u, rd=rd, rn=rn)      # SRI
    if opc == 0x0A:
        return "nv_shift", dict(q=q, esz=hb, sh=left, opc=5, u=u, rd=rd, rn=rn)       # SHL / SLI
    if opc == 0x0C and u:
        return "nv_shift", dict(q=q, esz=hb, sh=left, opc=6, u=u, rd=rd, rn=rn)       # SQSHLU
    if opc == 0x0E:
        return "nv_shift", dict(q=q, esz=hb, sh=left, opc=7, u=u, rd=rd, rn=rn)       # SQSHL/UQSHL imm
    if opc in (0x10, 0x11, 0x12, 0x13):
        if hb == 3:
            return None
        if opc in (0x10, 0x11):
            kind = (opc & 1) | (u << 1)       # 0 SHRN 1 RSHRN 2 SQSHRUN 3 SQRSHRUN
        else:
            kind = 4 + (opc & 1) + 2 * u      # 4 SQSHRN 5 SQRSHRN 6 UQSHRN 7 UQRSHRN
        return "nv_shn", dict(q=q, esz=hb, sh=right, kind=kind, rd=rd, rn=rn)
    if opc == 0x14:
        if hb == 3:
            return None
        return "nv_shll", dict(q=q, esz=hb, sh=left, u=u, rd=rd, rn=rn)
    if opc in (0x1C, 0x1F):
        if hb < 2 or (hb == 3 and not q):
            return None
        if opc == 0x1F and False:
            return None
        return "nv_fixcvt", dict(q=q, sz=hb - 2, to_int=int(opc == 0x1F), u=u, fbits=right, rd=rd, rn=rn)
    return None


def _indexed(w, q, u, rd, rn):
    size, L, M, H = _b(w, 23, 22), _b(w, 21, 21), _b(w, 20, 20), _b(w, 11, 11)
    rmlo, opc = _b(w, 19, 16), _b(w, 15, 12)
    fpops = {(0, 1), (0, 5), (0, 9), (1, 9)}
    if (u, opc) in fpops and size >= 2:
        sz = size & 1
        if sz == 0:
            idx, rm = (H << 1) | L, (M << 4) | rmlo
        else:
            if L:
                return None
            idx, rm = H, (M << 4) | rmlo
            if not q:
                return None
        return "nv_elemf", dict(q=q, sz=sz, u=u, opc=opc, idx=idx, rd=rd, rn=rn, rm=rm)
    if (u, opc) in fpops or size == 0 or size == 3:
        return None
    if size == 1:
        idx, rm = (H << 2) | (L << 1) | M, rmlo
    else:
        idx, rm = (H << 1) | L, (M << 4) | rmlo
    okop = {(0, 2), (0, 3), (0, 6), (0, 7), (0, 8), (0, 10), (0, 11), (0, 12), (0, 13),
            (1, 2), (1, 6), (1, 10), (1, 0), (1, 4), (1, 8)}
    # U=1: MLA(0) MLS(4) UMLAL(2) UMLSL(6) UMULL(10); U=0: MUL(8) SMLAL(2) SQDMLAL(3) SMLSL(6) SQDMLSL(7) SMULL(10) SQDMULL(11)
    #      SQDMULH(12) SQRDMULH(13); U=1 also has 8? (MUL is U=0 opc 8)
    okop.discard((1, 8))
    if (u, opc) not in okop:
        return None
    if opc == 0 or opc == 4:
        pass
    return "nv_elem", dict(q=q, size=size, u=u, opc=opc, idx=idx, rd=rd, rn=rn, rm=rm)


# --------------------------------------------------------------------------------------------- structure ld/st
def _ldst_struct(w):
    q, L = _b(w, 30, 30), _b(w, 22, 22)
    rt, rn = _b(w, 4, 0), _b(w, 9, 5)
    post = _b(w, 23, 23)
    single = _b(w, 24, 24)
    rm = _b(w, 20, 16)
    if not post and rm != 0:
        return None
    wb = 0
    if post:
        wb = 1 if rm == 31 else 2
    rmf = rm if wb == 2 else 31
    if not single:
        if _b(w, 21, 21):
            return None
        opc, size = _b(w, 15, 12), _b(w, 11, 10)
        nregs = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2}.get(opc)
        if nregs is None:
            return None
        selem = {0: 4, 2: 1, 4: 3, 6: 1, 7: 1, 8: 2, 10: 1}[opc]
        if selem > 1 and size == 3 and not q:
            return None
        regs = [(rt + k) % 32 for k in range(nregs)]
        regs += [rt] * (4 - len(regs))
        tb = nregs * (8 << q)
        return ("nv_ldm" if L else "nv_stm"), dict(q=q, esz=size, opc=opc, wb=wb, rt=regs[0], rt2=regs[1], rt3=regs[2],
                                                  rt4=regs[3], rn=rn, rm=rmf, imm=tb)
    # single structure
    R, opc3, S, size = _b(w, 21, 21), _b(w, 15, 13), _b(w, 12, 12), _b(w, 11, 10)
    selem = (((opc3 & 1) << 1) | R) + 1
    scale = opc3 >> 1
    regs = [(rt + k) % 32 for k in range(selem)]
    regs += [rt] * (4 - len(regs))
    if scale == 3:
        if not L or S:
            return None
        return "nv_ldr", dict(q=q, esz=size, sel=selem, wb=wb, rt=regs[0], rt2=regs[1], rt3=regs[2], rt4=regs[3], rn=rn,
                              rm=rmf, imm=selem << size)
    if scale == 0:
        esz, idx = 0, (q << 3) | (S << 2) | size
    elif scale == 1:
        if size & 1:
            return None
        esz, idx = 1, (q << 2) | (S << 1) | (size >> 1)
    else:
        if size == 0:
            esz, idx = 2, (q << 1) | S
        elif size == 1 and not S:
            esz, idx = 3, q
        else:
            return None
    return ("nv_ldl" if L else "nv_stl"), dict(esz=esz, sel=selem, idx=idx, wb=wb, rt=regs[0], rt2=regs[1], rt3=regs[2],
                                              rt4=regs[3], rn=rn, rm=rmf, imm=selem << esz)
