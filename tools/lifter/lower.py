"""Decoded instruction dict -> IR op (name, fields). Our own code (task-5)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dec  # noqa: E402
from dec import Unsupported, _ones  # noqa: E402

CONTROL = ("b", "bl", "cbz", "tbz", "bcond", "br", "blr", "ret", "svc", "brk")


def bit_masks(n, imms, immr, datasize):
    """ARM DecodeBitMasks(immediate=FALSE): returns (wmask, tmask)."""
    combined = (n << 6) | (~imms & 0x3F)
    length = combined.bit_length() - 1
    if length < 1:
        raise Unsupported("bitmask")
    levels = _ones(length)
    s, r = imms & levels, immr & levels
    d = ((s - r) & 0x3F) & levels
    esize = 1 << length
    welem = _ones(s + 1)
    welem = ((welem >> r) | (welem << (esize - r))) & _ones(esize) if r else welem
    telem = _ones(d + 1)
    w = t = 0
    for i in range(datasize // esize):
        w |= welem << (i * esize)
        t |= telem << (i * esize)
    return w & _ones(datasize), t & _ones(datasize)


def _m(sf):
    return 0xFFFFFFFFFFFFFFFF if sf else 0xFFFFFFFF


_WB = {None: 0, "post": 1, "pre": 2}


def lower(d):
    o = d["op"]
    if o in CONTROL:
        return o, d
    if o in ("adrp", "adr"):
        return "movi", dict(rd=d["rd"], imm=d["val"])
    if o == "addsub_imm":
        return ("addsubs_imm" if d["s"] else "addsub_imm"), dict(sf=d["sf"], sub=d["sub"], rd=d["rd"], rn=d["rn"], imm=d["imm"])
    if o == "log_imm":
        if d["opc"] == 3:
            return "ands_imm", dict(sf=d["sf"], rd=d["rd"], rn=d["rn"], imm=d["imm"])
        return "log_imm", dict(sf=d["sf"], opc=d["opc"], rd=d["rd"], rn=d["rn"], imm=d["imm"])
    if o == "movw":
        v = d["imm"] << d["shift"]
        if d["opc"] == 0:
            return "movi", dict(rd=d["rd"], imm=(~v) & _m(d["sf"]))
        if d["opc"] == 2:
            return "movi", dict(rd=d["rd"], imm=v)
        return "movk", dict(sf=d["sf"], rd=d["rd"], imm=d["imm"], shift=d["shift"])
    if o == "bfm":
        sf = d["sf"]
        w, t = bit_masks(sf, d["imms"], d["immr"], 64 if sf else 32)
        return "bfm", dict(sf=sf, opc=d["opc"], rd=d["rd"], rn=d["rn"], immr=d["immr"], imms=d["imms"], wmask=w, tmask=t)
    if o == "extr":
        return "extr", dict(sf=d["sf"], rd=d["rd"], rn=d["rn"], rm=d["rm"], lsb=d["lsb"])
    if o == "log_reg":
        f = dict(sf=d["sf"], shift=d["shift"], neg=d["neg"], rd=d["rd"], rn=d["rn"], rm=d["rm"], amt=d["amt"])
        if d["opc"] == 3:
            return "ands_reg", f
        f["opc"] = d["opc"]
        return "log_reg", f
    if o == "addsub_reg":
        return ("addsubs_reg" if d["s"] else "addsub_reg"), dict(sf=d["sf"], sub=d["sub"], shift=d["shift"], rd=d["rd"], rn=d["rn"], rm=d["rm"], amt=d["amt"])
    if o == "addsub_ext":
        return ("addsubs_ext" if d["s"] else "addsub_ext"), dict(sf=d["sf"], sub=d["sub"], option=d["option"], amt=d["amt"], rd=d["rd"], rn=d["rn"], rm=d["rm"])
    if o == "adc":
        return ("adcs" if d["s"] else "adc"), dict(sf=d["sf"], sub=d["sub"], rd=d["rd"], rn=d["rn"], rm=d["rm"])
    if o == "ccmp":
        f = dict(sf=d["sf"], sub=d["sub"], cond=d["cond"], nzcv=d["nzcv"], rn=d["rn"])
        if d["isimm"]:
            f["imm"] = d["rm"]
            return "ccmp_imm", f
        f["rm"] = d["rm"]
        return "ccmp_reg", f
    if o == "csel":
        return "csel", dict(sf=d["sf"], kind=d["kind"], cond=d["cond"], rd=d["rd"], rn=d["rn"], rm=d["rm"])
    if o == "dp2":
        k = {"udiv": 2, "sdiv": 3, "lslv": 8, "lsrv": 9, "asrv": 10, "rorv": 11}[d["kind"]]
        return "dp2", dict(sf=d["sf"], kind=k, rd=d["rd"], rn=d["rn"], rm=d["rm"])
    if o == "dp1":
        k = {"rbit": 0, "rev16": 1, "rev32": 2, "rev": 3, "clz": 4, "cls": 5}[d["kind"]]
        if d["kind"] == "rev" and not d["sf"]:
            k = 2
        return "dp1", dict(sf=d["sf"], kind=k, rd=d["rd"], rn=d["rn"])
    if o == "dp3":
        k = {"madd": 0, "msub": 1, "smaddl": 2, "smsubl": 3, "smulh": 4, "umaddl": 5, "umsubl": 6, "umulh": 7}[d["kind"]]
        return "dp3", dict(sf=d["sf"], kind=k, rd=d["rd"], rn=d["rn"], rm=d["rm"], ra=d["ra"])
    if o in ("nop", "clrex"):
        return o, {}
    if o == "dmb":
        return "dmb", dict(crm=d["crm"])
    if o in ("mrs", "msr"):
        return o, dict(rt=d["rt"], sysreg=d["sysreg"])
    if o in ("ldar", "stlr", "ldxr"):
        return o, dict(sz=1 << d["size"], rt=d["rt"], rn=d["rn"])
    if o == "stxr":
        return "stxr", dict(sz=1 << d["size"], rs=d["rs"], rt=d["rt"], rn=d["rn"])
    if o == "ldxp":
        return "ldxp", dict(sz=d["size"], rt=d["rt"], rt2=d["rt2"], rn=d["rn"])
    if o == "stxp":
        return "stxp", dict(sz=d["size"], rs=d["rs"], rt=d["rt"], rt2=d["rt2"], rn=d["rn"])
    if o == "atomic":
        return "atomic", dict(sz=1 << d["size"], opc=d["opc"], rs=d["rs"], rt=d["rt"], rn=d["rn"])
    if o == "cas":
        return "cas", dict(sz=1 << d["size"], rs=d["rs"], rt=d["rt"], rn=d["rn"])
    if o == "casp":
        return "casp", dict(sz=d["size"], rs=d["rs"], rs2=d["rs"] + 1, rt=d["rt"], rt2=d["rt"] + 1, rn=d["rn"])
    if o == "ldr_lit":
        if d["v"]:
            return "ldlitv", dict(sz=d["nbytes"], rt=d["rt"], addr=d["addr"])
        return "ldlit", dict(sz=d["nbytes"], sgn=int(d["signed"]), d64=int(d["dst64"]), rt=d["rt"], addr=d["addr"])
    if o in ("ldp", "stp"):
        wb = _WB[d["wb"]]
        nm = o + ("v" if d["v"] else "")
        f = dict(sz=d["nbytes"], rt=d["rt"], rt2=d["rt2"], rn=d["rn"], imm=d["imm"])
        if o == "ldp" and not d["v"]:
            f["sgn"] = int(d["signed"])
        if wb:
            f["wb"] = wb
            nm += "_wb"
        return nm, f
    if o in ("ldr", "str"):
        wb = _WB[d["wb"]]
        v = d["v"]
        nm = ("ld" if o == "ldr" else "st") + ("v" if v else "")
        f = dict(sz=d["nbytes"], rt=d["rt"], rn=d["rn"])
        if o == "ldr" and not v:
            f["sgn"] = int(d["signed"])
            f["d64"] = int(d["dst64"])
        if d["rm"] is not None:
            f.update(opt=d["option"], amt=d["amt"], rm=d["rm"])
            nm += "_x"
        else:
            f["imm"] = d["imm"]
            if wb:
                f["wb"] = wb
                nm += "_wb"
        return nm, f
    return None, d


# ---- extension lowerers (FP and Advanced SIMD): modules lower_simd_a.py and lower_simd_b.py, each exposing
# decode_lower(word, pc) -> (opname, fields) or None. Ops live in ops_simd_a.py / ops_simd_b.py.
import importlib  # noqa: E402
import lower_crypto  # noqa: E402

EXT_NAMES = [x for x in os.environ.get("SMRPG_EXT", "simd_a,simd_b").split(",") if x]
EXTS = []
for _n in EXT_NAMES:
    try:
        EXTS.append(importlib.import_module("lower_" + _n))
    except ImportError:
        pass


def decode_any(w, pc):
    """Decode one word to (opname, fields) or raise dec.Unsupported(reason)."""
    reason = "unknown"
    if w == 0xE7FFDEFE or w == 0:
        # A32 permanently-undefined marker the SDK uses as a trap, and the all-zero word (UDF #0): both fault on hardware
        return "brk", dict(imm=0x1DEFE if w else 0x20000)
    try:
        d = dec.decode(w, pc)
        nm, f = lower(d)
        if nm is not None:
            return nm, f
        reason = "base decoded %s without lowering" % d["op"]
    except Unsupported as e:
        reason = str(e)
    r = lower_crypto.decode_lower(w, pc)
    if r:
        return r
    for m in EXTS:
        r = m.decode_lower(w, pc)
        if r:
            return r
    raise Unsupported(reason)
