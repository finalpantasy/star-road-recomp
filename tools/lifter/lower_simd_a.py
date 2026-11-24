"""Decoder + lowering for scalar floating point and Advanced SIMD scalar words (extension simd_a, task-5). Our own code.

decode_lower(word, pc) -> (opname, fields) or None.  Decodes raw bits (ARM DDI0487 C4.1.65/C4.1.66 encodings);
never raises.  Vector (non-scalar) Advanced SIMD, structure loads and stores are not handled here.
"""


def _b(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def vfp_expand_imm(ptype, imm8):
    a = (imm8 >> 7) & 1
    bb = (imm8 >> 6) & 1
    cd = (imm8 >> 4) & 3
    efgh = imm8 & 15
    if ptype == 0:
        return (a << 31) | ((bb ^ 1) << 30) | ((0x1F if bb else 0) << 25) | (cd << 23) | (efgh << 19)
    if ptype == 1:
        return (a << 63) | ((bb ^ 1) << 62) | ((0xFF if bb else 0) << 54) | (cd << 52) | (efgh << 48)
    return (a << 15) | ((bb ^ 1) << 14) | ((3 if bb else 0) << 12) | (cd << 10) | (efgh << 6)


_RM = {(0, 0): (0, 0), (0, 1): (0, 1), (1, 0): (1, 0), (1, 1): (1, 1), (2, 0): (2, 0), (2, 1): (2, 1),
       (3, 0): (3, 0), (3, 1): (3, 1)}


def decode_lower(word, pc):
    try:
        return _decode(word & 0xFFFFFFFF)
    except Exception:
        return None


def _decode(w):
    top = _b(w, 28, 24)
    if _b(w, 31, 30) == 1 and _b(w, 28, 24) in (0b11110, 0b11111):
        return _simd_scalar(w)
    if _b(w, 30, 29) != 0:
        return None
    sf = _b(w, 31, 31)
    ptype = _b(w, 23, 22)
    rd, rn, rm = _b(w, 4, 0), _b(w, 9, 5), _b(w, 20, 16)
    if top == 0b11111:
        if sf or ptype == 2:
            return None
        return "fp3", dict(ftype=ptype, kind=(_b(w, 21, 21) << 1) | _b(w, 15, 15), rd=rd, rn=rn, rm=rm, ra=_b(w, 14, 10))
    if top != 0b11110:
        return None
    if not _b(w, 21, 21):
        # fixed-point conversions
        rmode, opcode, scale = _b(w, 20, 19), _b(w, 18, 16), _b(w, 15, 10)
        if ptype == 2 or (not sf and scale < 32):
            return None
        fbits = 64 - scale
        if rmode == 0 and opcode in (2, 3):
            return "cvtf", dict(ftype=ptype, sf=sf, uns=opcode & 1, fbits=fbits, rd=rd, rn=rn)
        if rmode == 3 and opcode in (0, 1):
            return "fcvti", dict(ftype=ptype, sf=sf, rm=3, uns=opcode & 1, fbits=fbits, rd=rd, rn=rn)
        return None
    if _b(w, 15, 10) == 0:
        rmode, opcode = _b(w, 20, 19), _b(w, 18, 16)
        if rmode == 0 and opcode in (6, 7):
            if ptype == 2:
                return None
            if ptype == 0 and sf:
                return None
            if ptype == 1 and not sf:
                return None
            sz = {0: 4, 1: 8, 3: 2}[ptype] if ptype != 3 else 2
            if ptype == 3:
                sz = 2
            if opcode == 6:
                return "fmov_tx", dict(sz=sz, hi=0, rd=rd, rn=rn)
            return "fmov_fx", dict(sz=sz, rd=rd, rn=rn)
        if rmode == 1 and opcode in (6, 7) and ptype == 2 and sf:
            if opcode == 6:
                return "fmov_tx", dict(sz=8, hi=1, rd=rd, rn=rn)
            return "fmov_hx", dict(rd=rd, rn=rn)
        if ptype == 2:
            return None
        if rmode == 0 and opcode in (2, 3):
            return "cvtf", dict(ftype=ptype, sf=sf, uns=opcode & 1, fbits=0, rd=rd, rn=rn)
        rnd = {(0, 0): 0, (0, 1): 0, (1, 0): 1, (1, 1): 1, (2, 0): 2, (2, 1): 2, (3, 0): 3, (3, 1): 3, (0, 4): 4, (0, 5): 4}
        if (rmode, opcode) in rnd:
            return "fcvti", dict(ftype=ptype, sf=sf, rm=rnd[(rmode, opcode)], uns=opcode & 1, fbits=0, rd=rd, rn=rn)
        return None
    if sf or ptype == 2:
        return None
    if _b(w, 14, 10) == 0b10000:
        kind = _b(w, 20, 15)
        if kind in (0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 15):
            return "fp1", dict(ftype=ptype, kind=kind, rd=rd, rn=rn)
        if kind in (4, 5, 7):
            dst = {4: 0, 5: 1, 7: 3}[kind]
            if dst == ptype:
                return None
            return "fp1", dict(ftype=ptype, kind=kind, rd=rd, rn=rn)
        return None
    if _b(w, 13, 10) == 0b1000 and _b(w, 15, 14) == 0:
        opc2 = _b(w, 4, 0)
        if opc2 not in (0, 8, 16, 24):
            return None
        zero = (opc2 >> 3) & 1
        if zero and rm:
            return None
        return "fcmp", dict(ftype=ptype, e=(opc2 >> 4) & 1, zero=zero, rn=rn, rm=rm)
    if _b(w, 12, 10) == 0b100:
        if _b(w, 9, 5) != 0:
            return None
        return "fmovi", dict(rd=rd, imm=vfp_expand_imm(ptype, _b(w, 20, 13)))
    low = _b(w, 11, 10)
    if low == 1:
        return "fccmp", dict(ftype=ptype, e=_b(w, 4, 4), cond=_b(w, 15, 12), nzcv=_b(w, 3, 0), rn=rn, rm=rm)
    if low == 2:
        opc = _b(w, 15, 12)
        if opc > 8:
            return None
        return "fp2", dict(ftype=ptype, kind=opc, rd=rd, rn=rn, rm=rm)
    if low == 3:
        return "fcsel", dict(ftype=ptype, cond=_b(w, 15, 12), rd=rd, rn=rn, rm=rm)
    return None


_CVT_MISC = {  # (U, size[1], opcode) -> ("fcvti_v", rm, uns)
    (0, 0, 0b11010): (0, 0), (0, 0, 0b11011): (2, 0), (0, 0, 0b11100): (4, 0),
    (0, 1, 0b11010): (1, 0), (0, 1, 0b11011): (3, 0),
    (1, 0, 0b11010): (0, 1), (1, 0, 0b11011): (2, 1), (1, 0, 0b11100): (4, 1),
    (1, 1, 0b11010): (1, 1), (1, 1, 0b11011): (3, 1),
}
_CMPZ = {(0, 0b01100): 0, (1, 0b01100): 1, (0, 0b01101): 2, (1, 0b01101): 3, (0, 0b01110): 4}
_PAIR = {(0, 0b01101): 0, (0, 0b01100): 1, (0, 0b01111): 2, (1, 0b01100): 3, (1, 0b01111): 4}   # (size[1], opcode), U must be 1
_SAME = {(0, 0, 0b11011): 1, (0, 0, 0b11100): 4, (0, 0, 0b11111): 2, (0, 1, 0b11111): 3,
         (1, 0, 0b11100): 5, (1, 0, 0b11101): 7, (1, 1, 0b11010): 0, (1, 1, 0b11100): 6, (1, 1, 0b11101): 8}


def _simd_scalar(w):
    u = _b(w, 29, 29)
    size = _b(w, 23, 22)
    rd, rn, rm = _b(w, 4, 0), _b(w, 9, 5), _b(w, 20, 16)
    sz = size & 1
    if _b(w, 28, 24) == 0b11111:
        # by element
        opc = _b(w, 15, 12)
        if _b(w, 10, 10) != 0 or size < 2:
            return None
        h, l = _b(w, 11, 11), _b(w, 21, 21)
        if sz == 0:
            idx = (h << 1) | l
        else:
            if l:
                return None
            idx = h
        kind = {0b0001: 0, 0b0101: 1, 0b1001: 3 if u else 2}.get(opc)
        if kind is None or (u and opc != 0b1001):
            return None
        return "felem", dict(ftype=sz, kind=kind, idx=idx, rd=rd, rn=rn, rm=rm)
    # copy
    if _b(w, 29, 29) == 0 and _b(w, 23, 21) == 0 and _b(w, 15, 15) == 0 and _b(w, 14, 11) == 0 and _b(w, 10, 10) == 1:
        imm5 = _b(w, 20, 16)
        for s, n in ((1, 1), (2, 2), (4, 4), (8, 8)):
            if imm5 & s:
                return "dup_s", dict(sz=n, idx=imm5 >> (s.bit_length()), rd=rd, rn=rn)
        return None
    b21 = _b(w, 21, 21)
    b1110 = _b(w, 11, 10)
    if b21 and b1110 == 2 and _b(w, 20, 17) == 0:
        opc = _b(w, 16, 12)
        s1 = size >> 1
        if opc == 0b11101 and s1 == 0:
            return "cvtf_v", dict(ftype=sz, sf=sz, uns=u, fbits=0, rd=rd, rn=rn)
        k = _CVT_MISC.get((u, s1, opc))
        if k:
            return "fcvti_v", dict(ftype=sz, sf=sz, rm=k[0], uns=k[1], rd=rd, rn=rn)
        if s1 == 1 and opc in (0b01100, 0b01101, 0b01110):
            kk = _CMPZ.get((u, opc))
            if kk is not None:
                return "fcmz", dict(ftype=sz, kind=kk, rd=rd, rn=rn)
        if s1 == 1 and opc == 0b11101:
            return "fest", dict(ftype=sz, kind=1 if u else 0, rd=rd, rn=rn)
        if s1 == 1 and opc == 0b11111 and not u:
            return "fest", dict(ftype=sz, kind=2, rd=rd, rn=rn)
        return None
    if b21 and b1110 == 2 and _b(w, 20, 17) == 0b1000:
        opc = _b(w, 16, 12)
        if u == 0 and size == 3 and opc == 0b11011:
            return "addp_s", dict(rd=rd, rn=rn)
        k = _PAIR.get((size >> 1, opc))
        if u == 1 and k is not None:
            return "fpair", dict(ftype=sz, kind=k, rd=rd, rn=rn)
        return None
    if b21 and _b(w, 10, 10) == 1:
        opc = _b(w, 15, 11)
        k = _SAME.get((u, size >> 1, opc))
        if k is not None:
            return "f3same", dict(ftype=sz, kind=k, rd=rd, rn=rn, rm=rm)
    return None
