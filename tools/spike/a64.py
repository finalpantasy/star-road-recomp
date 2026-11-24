"""Minimal AArch64 (A64) decoder for the task-4 lifter spike.

Our own code. Covers the integer, branch, load/store, exclusive, system and
scalar FP subset that the spike lifts; anything else raises Unsupported, and
the spike reports coverage. This is a spike, not the task-5 lifter.
"""


class Unsupported(Exception):
    pass


def b(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def _ones(n):
    return (1 << n) - 1


def decode_bit_masks(n, imms, immr, datasize):
    """ARM DecodeBitMasks for logical immediates. Returns wmask."""
    combined = (n << 6) | (~imms & 0x3F)
    length = combined.bit_length() - 1
    if length < 1:
        raise Unsupported("bitmask")
    levels = _ones(length)
    s = imms & levels
    r = immr & levels
    if s == levels:
        raise Unsupported("bitmask reserved")
    esize = 1 << length
    welem = _ones(s + 1)
    # rotate right by r within esize
    welem = ((welem >> r) | (welem << (esize - r))) & _ones(esize) if r else welem
    out = 0
    for i in range(datasize // esize):
        out |= welem << (i * esize)
    return out & _ones(datasize)


def decode(w, pc):
    """Decode one instruction word at guest address pc. Returns a dict."""
    op0 = b(w, 28, 25)
    if op0 in (0b1000, 0b1001):
        return _dp_imm(w, pc)
    if op0 in (0b1010, 0b1011):
        return _branch_sys(w, pc)
    if op0 & 0b0101 == 0b0100:
        return _ldst(w, pc)
    if op0 & 0b0111 == 0b0101:
        return _dp_reg(w, pc)
    if op0 & 0b0111 == 0b0111:
        return _fp(w, pc)
    raise Unsupported("op0 %x" % op0)


def _dp_imm(w, pc):
    sf = b(w, 31, 31)
    rd = b(w, 4, 0)
    rn = b(w, 9, 5)
    k = b(w, 28, 23)
    if b(w, 28, 24) == 0b10000:
        imm = sext((b(w, 23, 5) << 2) | b(w, 30, 29), 21)
        if b(w, 31, 31):
            return dict(op="adrp", rd=rd, val=((pc & ~0xFFF) + (imm << 12)) & _ones(64))
        return dict(op="adr", rd=rd, val=(pc + imm) & _ones(64))
    if k == 0b100010:
        imm = b(w, 21, 10) << (12 if b(w, 22, 22) else 0)
        return dict(op="addsub_imm", sf=sf, sub=b(w, 30, 30), s=b(w, 29, 29), rd=rd, rn=rn, imm=imm)
    if k == 0b100100:
        ds = 64 if sf else 32
        if not sf and b(w, 22, 22):
            raise Unsupported("logimm N")
        imm = decode_bit_masks(b(w, 22, 22), b(w, 15, 10), b(w, 21, 16), ds)
        return dict(op="log_imm", sf=sf, opc=b(w, 30, 29), rd=rd, rn=rn, imm=imm)
    if k == 0b100101:
        opc = b(w, 30, 29)
        if opc == 1:
            raise Unsupported("movw opc")
        hw = b(w, 22, 21)
        if not sf and hw > 1:
            raise Unsupported("movw hw")
        return dict(op="movw", sf=sf, opc=opc, rd=rd, imm=b(w, 20, 5), shift=hw * 16)
    if k == 0b100110:
        opc = b(w, 30, 29)
        if opc == 3:
            raise Unsupported("bfm opc")
        return dict(op="bfm", sf=sf, opc=opc, rd=rd, rn=rn, immr=b(w, 21, 16), imms=b(w, 15, 10))
    if k == 0b100111:
        if b(w, 30, 29) != 0 or b(w, 21, 21):
            raise Unsupported("extr")
        return dict(op="extr", sf=sf, rd=rd, rn=rn, rm=b(w, 20, 16), lsb=b(w, 15, 10))
    raise Unsupported("dpimm")


def _branch_sys(w, pc):
    if b(w, 30, 26) == 0b00101:
        tgt = (pc + (sext(b(w, 25, 0), 26) << 2)) & _ones(64)
        return dict(op="bl" if b(w, 31, 31) else "b", target=tgt)
    if b(w, 30, 25) == 0b011010:
        tgt = (pc + (sext(b(w, 23, 5), 19) << 2)) & _ones(64)
        return dict(op="cbz", sf=b(w, 31, 31), nz=b(w, 24, 24), rt=b(w, 4, 0), target=tgt)
    if b(w, 30, 25) == 0b011011:
        tgt = (pc + (sext(b(w, 18, 5), 14) << 2)) & _ones(64)
        bit = (b(w, 31, 31) << 5) | b(w, 23, 19)
        return dict(op="tbz", nz=b(w, 24, 24), rt=b(w, 4, 0), bit=bit, target=tgt)
    if b(w, 31, 24) == 0b01010100 and not b(w, 4, 4):
        tgt = (pc + (sext(b(w, 23, 5), 19) << 2)) & _ones(64)
        return dict(op="bcond", cond=b(w, 3, 0), target=tgt)
    if b(w, 31, 21) == 0b11010100000 and b(w, 4, 0) == 1:
        return dict(op="svc", imm=b(w, 20, 5))
    if b(w, 31, 21) == 0b11010100001 and b(w, 4, 0) == 0:
        return dict(op="brk", imm=b(w, 20, 5))
    if b(w, 31, 25) == 0b1101011:
        opc = b(w, 24, 21)
        if b(w, 20, 16) != 0x1F or b(w, 15, 10) != 0 or b(w, 4, 0) != 0:
            raise Unsupported("brreg")
        rn = b(w, 9, 5)
        if opc == 0:
            return dict(op="br", rn=rn)
        if opc == 1:
            return dict(op="blr", rn=rn)
        if opc == 2:
            return dict(op="ret", rn=rn)
        raise Unsupported("brreg opc")
    if b(w, 31, 22) == 0b1101010100:
        if w & 0xFFFFF01F == 0xD503201F:
            return dict(op="nop")
        if w & 0xFFFFF0FF == 0xD50330BF:
            return dict(op="dmb", crm=b(w, 11, 8))
        if w & 0xFFFFF0FF == 0xD503309F:
            return dict(op="dmb", crm=b(w, 11, 8))  # DSB: same fence for user code
        if w & 0xFFFFF0FF == 0xD50330DF:
            return dict(op="nop")  # ISB
        if w & 0xFFFFF0FF == 0xD503305F:
            return dict(op="clrex")
        l = b(w, 21, 21)
        if b(w, 20, 20) == 1:
            sysreg = b(w, 20, 5)
            return dict(op="mrs" if l else "msr", sysreg=sysreg, rt=b(w, 4, 0))
        raise Unsupported("system")
    raise Unsupported("branch/sys")


def _ldst(w, pc):
    rt = b(w, 4, 0)
    rn = b(w, 9, 5)
    size = b(w, 31, 30)
    v = b(w, 26, 26)
    if b(w, 29, 24) == 0b001000:
        o2, l, o1, o0 = b(w, 23, 23), b(w, 22, 22), b(w, 21, 21), b(w, 15, 15)
        if o1:
            raise Unsupported("excl pair")
        if o2 == 0:
            return dict(op="ldxr" if l else "stxr", size=size, rt=rt, rn=rn, rs=b(w, 20, 16), ordered=o0)
        if o0 == 0:
            raise Unsupported("ldlar")
        return dict(op="ldar" if l else "stlr", size=size, rt=rt, rn=rn)
    if b(w, 29, 27) == 0b011 and b(w, 25, 24) == 0:
        opc = size
        addr = (pc + (sext(b(w, 23, 5), 19) << 2)) & _ones(64)
        if v:
            if opc == 3:
                raise Unsupported("ldr lit v")
            return dict(op="ldr_lit", v=1, nbytes=(4, 8, 16)[opc], signed=0, rt=rt, addr=addr)
        if opc == 3:
            return dict(op="nop")  # PRFM literal
        return dict(op="ldr_lit", v=0, nbytes=(4, 8, 4)[opc], signed=opc == 2, dst64=opc != 0, rt=rt, addr=addr)
    if b(w, 29, 27) == 0b101:
        mode = b(w, 25, 23)
        opc = size
        l = b(w, 22, 22)
        if v:
            if opc == 3:
                raise Unsupported("ldp v opc")
            nbytes, signed = (4, 8, 16)[opc], 0
        else:
            if opc == 3 or (opc == 1 and not l):
                raise Unsupported("ldp opc")
            nbytes = 8 if opc == 2 else 4
            signed = opc == 1
        imm = sext(b(w, 21, 15), 7) * nbytes
        wb = {0: None, 1: "post", 2: None, 3: "pre"}[mode]
        return dict(op="ldp" if l else "stp", v=v, nbytes=nbytes, signed=signed, rt=rt, rt2=b(w, 14, 10),
                    rn=rn, imm=imm, wb=wb)
    if b(w, 29, 27) == 0b111:
        opc = b(w, 23, 22)
        if v:
            nbytes = 16 if (size == 0 and opc & 2) else (1 << size)
            if opc & 2 and size != 0:
                raise Unsupported("ldr v opc")
            load = opc & 1
            kind = dict(v=1, nbytes=nbytes, signed=0, dst64=1)
        else:
            nbytes = 1 << size
            if opc == 0:
                load, kind = 0, dict(v=0, nbytes=nbytes, signed=0, dst64=1)
            elif opc == 1:
                load, kind = 1, dict(v=0, nbytes=nbytes, signed=0, dst64=1)
            elif opc == 2:
                if size == 3:
                    return dict(op="nop")  # PRFM
                load, kind = 1, dict(v=0, nbytes=nbytes, signed=1, dst64=1)
            else:
                if size >= 2:
                    raise Unsupported("ldrs opc")
                load, kind = 1, dict(v=0, nbytes=nbytes, signed=1, dst64=0)
        base = dict(op="ldr" if load else "str", rt=rt, rn=rn, **kind)
        if b(w, 25, 24) == 0b01:
            base.update(imm=b(w, 21, 10) * nbytes, wb=None, rm=None)
            return base
        if b(w, 25, 24) == 0b00:
            if b(w, 21, 21) == 0:
                m = b(w, 11, 10)
                base.update(imm=sext(b(w, 20, 12), 9), rm=None, wb={0: None, 1: "post", 2: None, 3: "pre"}[m])
                return base
            if b(w, 11, 10) == 0b10:
                option = b(w, 15, 13)
                if option & 2 == 0:
                    raise Unsupported("ldr regoff opt")
                amt = (nbytes.bit_length() - 1) if b(w, 12, 12) else 0
                base.update(rm=b(w, 20, 16), option=option, amt=amt, imm=0, wb=None)
                return base
            raise Unsupported("lse atomic")
        raise Unsupported("ldst 111")
    raise Unsupported("ldst")


def _dp_reg(w, pc):
    sf = b(w, 31, 31)
    rd, rn, rm = b(w, 4, 0), b(w, 9, 5), b(w, 20, 16)
    if b(w, 28, 24) == 0b01010:
        return dict(op="log_reg", sf=sf, opc=b(w, 30, 29), shift=b(w, 23, 22), neg=b(w, 21, 21), rd=rd, rn=rn,
                    rm=rm, amt=b(w, 15, 10))
    if b(w, 28, 24) == 0b01011:
        if b(w, 21, 21) == 0:
            if b(w, 23, 22) == 3:
                raise Unsupported("addsub shift")
            return dict(op="addsub_reg", sf=sf, sub=b(w, 30, 30), s=b(w, 29, 29), shift=b(w, 23, 22), rd=rd,
                        rn=rn, rm=rm, amt=b(w, 15, 10))
        if b(w, 23, 22) != 0 or b(w, 12, 10) > 4:
            raise Unsupported("addsub ext")
        return dict(op="addsub_ext", sf=sf, sub=b(w, 30, 30), s=b(w, 29, 29), option=b(w, 15, 13),
                    amt=b(w, 12, 10), rd=rd, rn=rn, rm=rm)
    k = b(w, 28, 21)
    if k == 0b11010000 and b(w, 15, 10) == 0:
        return dict(op="adc", sf=sf, sub=b(w, 30, 30), s=b(w, 29, 29), rd=rd, rn=rn, rm=rm)
    if k == 0b11010010:
        if not b(w, 29, 29) or b(w, 10, 10) or b(w, 4, 4):
            raise Unsupported("ccmp")
        return dict(op="ccmp", sf=sf, sub=b(w, 30, 30), isimm=b(w, 11, 11), rm=rm, cond=b(w, 15, 12), rn=rn,
                    nzcv=b(w, 3, 0))
    if k == 0b11010100:
        if b(w, 29, 29) or b(w, 11, 11):
            raise Unsupported("csel")
        return dict(op="csel", sf=sf, kind=(b(w, 30, 30) << 1) | b(w, 10, 10), rd=rd, rn=rn, rm=rm,
                    cond=b(w, 15, 12))
    if k == 0b11010110:
        opcode = b(w, 15, 10)
        if b(w, 30, 30) == 0:
            names = {2: "udiv", 3: "sdiv", 8: "lslv", 9: "lsrv", 10: "asrv", 11: "rorv"}
            if opcode not in names or b(w, 29, 29):
                raise Unsupported("dp2")
            return dict(op="dp2", sf=sf, kind=names[opcode], rd=rd, rn=rn, rm=rm)
        if rm != 0:
            raise Unsupported("dp1")
        names = {0: "rbit", 1: "rev16", 2: "rev32", 3: "rev64", 4: "clz", 5: "cls"}
        if opcode not in names:
            raise Unsupported("dp1 op")
        kind = names[opcode]
        if not sf and kind == "rev32":
            kind = "rev"
        if not sf and kind == "rev64":
            raise Unsupported("rev64 w")
        if sf and kind == "rev64":
            kind = "rev"
        return dict(op="dp1", sf=sf, kind=kind, rd=rd, rn=rn)
    if b(w, 28, 24) == 0b11011:
        if b(w, 30, 29):
            raise Unsupported("dp3")
        op31, o0 = b(w, 23, 21), b(w, 15, 15)
        kinds = {(0, 0): "madd", (0, 1): "msub", (1, 0): "smaddl", (1, 1): "smsubl", (2, 0): "smulh",
                 (5, 0): "umaddl", (5, 1): "umsubl", (6, 0): "umulh"}
        if (op31, o0) not in kinds or (op31 and not sf):
            raise Unsupported("dp3 op")
        return dict(op="dp3", sf=sf, kind=kinds[(op31, o0)], rd=rd, rn=rn, rm=rm, ra=b(w, 14, 10))
    raise Unsupported("dpreg")


def _fp(w, pc):
    rd, rn, rm = b(w, 4, 0), b(w, 9, 5), b(w, 20, 16)
    # Vector ORR used as MOV Vd, Vn and MOVI #0: the common SIMD idioms in prologue/zeroing code.
    if w & 0xBFE0FC00 == 0x0EA01C00 and rm == rn:
        return dict(op="vmov", q=b(w, 30, 30), rd=rd, rn=rn)
    if w & 0xBFFFFFE0 == 0x2F00E400:
        return dict(op="vzero", q=b(w, 30, 30), rd=rd)
    if b(w, 30, 30) or b(w, 29, 29):
        raise Unsupported("simd")
    ptype = b(w, 23, 22)
    if b(w, 28, 24) == 0b11111:
        if ptype > 1:
            raise Unsupported("fp3 type")
        kind = ("fmadd", "fmsub", "fnmadd", "fnmsub")[(b(w, 21, 21) << 1) | b(w, 15, 15)]
        return dict(op="fp3", dbl=ptype, kind=kind, rd=rd, rn=rn, rm=rm, ra=b(w, 14, 10))
    if b(w, 28, 24) != 0b11110:
        raise Unsupported("simd")
    sf = b(w, 31, 31)
    if b(w, 21, 21) and b(w, 15, 10) == 0:
        rmode, opcode = b(w, 20, 19), b(w, 18, 16)
        if ptype > 1:
            raise Unsupported("fpint type")
        if rmode == 0 and opcode in (6, 7):
            if (sf and not ptype) or (not sf and ptype):
                raise Unsupported("fmov size")
            return dict(op="fmov_gen", to_gen=opcode == 6, sf=sf, rd=rd, rn=rn)
        if rmode == 0 and opcode in (2, 3):
            return dict(op="cvtf", sf=sf, dbl=ptype, unsigned=opcode == 3, rd=rd, rn=rn)
        rnd = {(3, 0): "z", (3, 1): "z", (0, 0): "n", (0, 1): "n", (1, 0): "p", (1, 1): "p", (2, 0): "m",
               (2, 1): "m", (0, 4): "a", (0, 5): "a"}
        if (rmode, opcode) in rnd:
            return dict(op="fcvti", sf=sf, dbl=ptype, unsigned=opcode & 1, rnd=rnd[(rmode, opcode)], rd=rd, rn=rn)
        raise Unsupported("fpint")
    if ptype > 1:
        raise Unsupported("fp half")
    if b(w, 21, 21) and b(w, 14, 10) == 0b10000:
        opcode = b(w, 20, 15)
        names = {0: "fmov", 1: "fabs", 2: "fneg", 3: "fsqrt", 4: "fcvt_s", 5: "fcvt_d", 8: "frintn", 9: "frintp",
                 10: "frintm", 11: "frintz", 12: "frinta", 14: "frintx", 15: "frinti"}
        if opcode not in names:
            raise Unsupported("fp1")
        return dict(op="fp1", dbl=ptype, kind=names[opcode], rd=rd, rn=rn)
    if b(w, 21, 21) and b(w, 13, 10) == 0b1000 and b(w, 15, 14) == 0:
        opc2 = b(w, 4, 0)
        if opc2 not in (0, 8, 16, 24):
            raise Unsupported("fcmp")
        return dict(op="fcmp", dbl=ptype, rn=rn, rm=rm, zero=bool(opc2 & 8))
    if b(w, 21, 21) and b(w, 12, 10) == 0b100 and b(w, 9, 5) == 0:
        return dict(op="fmov_imm", dbl=ptype, rd=rd, imm8=b(w, 20, 13))
    if b(w, 21, 21) and b(w, 11, 10) == 0b10:
        names = {0: "fmul", 1: "fdiv", 2: "fadd", 3: "fsub", 4: "fmax", 5: "fmin", 6: "fmaxnm", 7: "fminnm",
                 8: "fnmul"}
        opcode = b(w, 15, 12)
        if opcode not in names:
            raise Unsupported("fp2")
        return dict(op="fp2", dbl=ptype, kind=names[opcode], rd=rd, rn=rn, rm=rm)
    if b(w, 21, 21) and b(w, 11, 10) == 0b11:
        return dict(op="fcsel", dbl=ptype, rd=rd, rn=rn, rm=rm, cond=b(w, 15, 12))
    raise Unsupported("fp")
