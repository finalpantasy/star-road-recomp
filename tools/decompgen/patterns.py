"""Exact trivial-function patterns for the decomp generator (task-45). Our own code.

Each pattern takes the decoded instructions of one whole function (dicts from tools/lifter/dec.py) and returns a
Spec or None. A function matches only when every instruction has exactly the expected shape: no other register
written, no flags, no write-back addressing, no call. Anything else is skipped, never approximated.

Register convention of the generated Rust (same as the hand-written leaves): the symbol takes guest x0, x1, x2
as u64 (a, b, c) and returns the new x0. A function that writes only x0 (or nothing) fits that interface, so the
A/B compare of the whole register context stays valid.
"""
from collections import namedtuple

# kind: LEAF (registers only), GLEAF (load, x[base] + offset), NLEAF (null-safe load), SLEAF (store).
Spec = namedtuple("Spec", "pattern variant kind rust_body base_reg offset nbytes")

ARGS = "abc"                       # Rust parameter names for guest x0..x2
GUEST_LD = {1: "guest_ld8", 2: "guest_ld16", 4: "guest_ld32", 8: "guest_ld64"}
GUEST_ST = {1: "guest_st8", 2: "guest_st16", 4: "guest_st32", 8: "guest_st64"}
UNSIGNED = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}
SIGNED = {1: "i8", 2: "i16", 4: "i32", 8: "i64"}


def _is_ret(d):
    return d["op"] == "ret" and d["rn"] == 30


def _hex(v):
    return "0x%x" % v


def empty(insns, pc):
    """ret: nothing is written, x0 comes back unchanged."""
    if len(insns) == 1 and _is_ret(insns[0]):
        return Spec("empty", "ret", "LEAF", "a", 0, 0, 0)


def _mov_wide(i, state):
    """Apply one movz, movn or movk to x0. Returns (value, sf) or None when it is not a plain write to x0."""
    if i["op"] != "movw" or i["rd"] != 0:
        return None
    sf, opc, imm, shift = i["sf"], i["opc"], i["imm"], i["shift"]
    mask = (1 << (64 if sf else 32)) - 1
    if opc == 2:                       # movz
        return (imm << shift) & mask, sf
    if opc == 0:                       # movn
        return ~(imm << shift) & mask, sf
    if opc == 3 and state is not None and state[1] == sf:   # movk keeps the other 16-bit fields
        return (state[0] & ~(0xFFFF << shift) | (imm << shift)) & mask, sf
    return None


def const_return(insns, pc):
    """mov x0 or w0 to a constant (movz, movn, movk sequence, orr from the zero register), then ret."""
    if len(insns) < 2 or len(insns) > 5 or not _is_ret(insns[-1]):
        return None
    first = insns[0]
    if first["op"] == "log_reg":       # orr x0/w0, xzr, xzr (the `mov x0, xzr` alias): constant 0
        shape = (first["opc"], first["rd"], first["rn"], first["rm"], first["shift"], first["amt"], first["neg"])
        if shape == (1, 0, 31, 31, 0, 0, 0) and len(insns) == 2:
            return Spec("const", "zero_x" if first["sf"] else "zero_w", "LEAF", "0", 0, 0, 0)
        return None
    if first["op"] == "log_imm":       # orr x0/w0, xzr, #bitmask (the `mov x0, #bitmask` alias)
        if (first["opc"], first["rd"], first["rn"]) == (1, 0, 31) and len(insns) == 2:
            return Spec("const", "orr_imm_x" if first["sf"] else "orr_imm_w", "LEAF", _hex(first["imm"]), 0, 0, 0)
        return None
    state = None
    for i in insns[:-1]:
        state = _mov_wide(i, state)
        if state is None:
            return None
    if first["opc"] not in (0, 2):     # a sequence must start with movz or movn
        return None
    return Spec("const", "wide_x" if state[1] else "wide_w", "LEAF", _hex(state[0]), 0, 0, 0)


def _plain_mem(i, op):
    """True for a plain base + unsigned scaled immediate access: no write-back, no register offset, no vector."""
    if i["op"] != op or i.get("v") != 0 or i.get("wb") is not None or i.get("rm") is not None:
        return False
    return i["rn"] <= 2 and 0 <= i["imm"] < (1 << 16) and i["imm"] % i["nbytes"] == 0


def _load_expr(i, address):
    n = i["nbytes"]
    raw = "%s(%s)" % (GUEST_LD[n], address)
    if not i["signed"]:
        return raw if n == 8 else "u64::from(%s)" % raw
    if i["dst64"]:
        return "%s as %s as i64 as u64" % (raw, SIGNED[n])
    return "u64::from(%s as %s as i32 as u32)" % (raw, SIGNED[n])


def _load_variant(i):
    return "%s%d%s" % ("s" if i["signed"] else "u", i["nbytes"] * 8, ("x" if i["dst64"] else "w") if i["signed"] else "")


def getter(insns, pc):
    """ldr{,b,h,sb,sh,sw} x0/w0, [xN, #imm]; ret (N = 0..2)."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if not _plain_mem(i, "ldr") or i["rt"] != 0:
        return None
    address = "%s.wrapping_add(%s)" % (ARGS[i["rn"]], _hex(i["imm"]))
    return Spec("get", _load_variant(i), "GLEAF", "unsafe { %s }" % _load_expr(i, address), i["rn"], i["imm"], i["nbytes"])


def nullsafe_getter(insns, pc):
    """cbz x0, +8; ldr x0/w0, [x0, #imm]; ret: 0 stays 0, otherwise the load (the task-42 shape)."""
    if len(insns) != 3 or not _is_ret(insns[2]):
        return None
    z, i = insns[0], insns[1]
    if z["op"] != "cbz" or z["sf"] != 1 or z["nz"] != 0 or z["rt"] != 0 or z["target"] != pc + 8:
        return None
    if not _plain_mem(i, "ldr") or i["rt"] != 0 or i["rn"] != 0:
        return None
    address = "a.wrapping_add(%s)" % _hex(i["imm"])
    body = "if a == 0 {\n    a\n} else {\n    unsafe { %s }\n}" % _load_expr(i, address)
    return Spec("get_nullsafe", _load_variant(i), "NLEAF", body, 0, i["imm"], i["nbytes"])


def setter(insns, pc):
    """str{,b,h} xT/wT/wzr, [xN, #imm]; ret (N, T = 0..2 or the zero register). No barrier, no call."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if not _plain_mem(i, "str") or not (i["rt"] <= 2 or i["rt"] == 31):
        return None
    n = i["nbytes"]
    value = "0" if i["rt"] == 31 else ARGS[i["rt"]]
    if n != 8 and i["rt"] != 31:
        value = "%s as %s" % (value, UNSIGNED[n])
    body = "unsafe { %s(%s.wrapping_add(delta).wrapping_add(%s), %s) };\na" % (
        GUEST_ST[n], ARGS[i["rn"]], _hex(i["imm"]), value)
    variant = "u%d_%s" % (n * 8, "zr" if i["rt"] == 31 else "x%d" % i["rt"])
    return Spec("set", variant, "SLEAF", body, i["rn"], i["imm"], n)


def add_imm(insns, pc):
    """add or sub x0/w0, xN/wN, #imm (no flags); ret. A field address."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if i["op"] != "addsub_imm" or i["s"] != 0 or i["rd"] != 0 or i["rn"] > 2:
        return None
    method = "wrapping_sub" if i["sub"] else "wrapping_add"
    src = ARGS[i["rn"]]
    if i["sf"]:
        body = "%s.%s(%s)" % (src, method, _hex(i["imm"]))
    else:
        body = "u64::from((%s as u32).%s(%s))" % (src, method, _hex(i["imm"]))
    return Spec("add_imm", "%s_%s" % ("sub" if i["sub"] else "add", "x" if i["sf"] else "w"), "LEAF", body, 0, 0, 0)


def mov_reg(insns, pc):
    """orr x0/w0, xzr, xM/wM (the `mov x0, x1` alias); ret."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if i["op"] != "log_reg" or (i["opc"], i["rd"], i["rn"], i["shift"], i["amt"], i["neg"]) != (1, 0, 31, 0, 0, 0):
        return None
    if i["rm"] not in (1, 2):
        return None
    src = ARGS[i["rm"]]
    body = src if i["sf"] else "u64::from(%s as u32)" % src
    return Spec("mov_reg", "x" if i["sf"] else "w", "LEAF", body, 0, 0, 0)


# The shapes are disjoint (different lengths or instruction kinds), so the order does not matter.
PATTERNS = [empty, const_return, getter, nullsafe_getter, setter, add_imm, mov_reg]
FAMILIES = ["empty", "const", "get", "get_nullsafe", "set", "add_imm", "mov_reg"]


def match(insns, pc):
    for p in PATTERNS:
        spec = p(insns, pc)
        if spec:
            return spec
    return None
