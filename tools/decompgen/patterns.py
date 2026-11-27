"""Exact trivial-function patterns for the decomp generator (task-45). Our own code.

Each pattern takes the decoded instructions of one whole function (dicts from tools/lifter/dec.py) and returns a
Spec or None. A function matches only when every instruction has exactly the expected shape: no other register
written, no flags, no write-back addressing, no call. Anything else is skipped, never approximated.

Register convention of the generated Rust (same as the hand-written leaves): the symbol takes guest x0, x1, x2
as u64 (a, b, c) and returns the new x0. A function that writes only x0 (or nothing) fits that interface, so the
A/B compare of the whole register context stays valid.
"""
from collections import namedtuple

# kind: LEAF (registers only), GLEAF (load, x[base] + offset), NLEAF (null-safe load), SLEAF (store),
# (the boolean setter's scratch register x8 is not part of the lifted reference's output, see set_bool).
# v2 (task-50): VLEAF (float or SIMD register leaf; vkind 1 load, 2 store, 3 constant, 4 load through a field) and DLEAF
# (integer load through a field). vreg is the vector register, offset2 the second-level load offset.
Spec = namedtuple("Spec", "pattern variant kind rust_body base_reg offset nbytes vkind vreg offset2", defaults=(0, 0, 0))

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


def _plain_mem(i, op, vector=0, rn_ok=lambda n: n <= 2):
    """True for a plain base + unsigned scaled immediate access: no write-back, no register offset. vector=0 is an
    integer access, vector=1 a float or SIMD register access."""
    if i["op"] != op or i.get("v") != vector or i.get("wb") is not None or i.get("rm") is not None:
        return False
    return rn_ok(i["rn"]) and 0 <= i["imm"] < (1 << 16) and i["imm"] % i["nbytes"] == 0


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


def set_bool(insns, pc):
    """and wT, wS, #1; strb wT, [xN, #imm]; ret (S, N = 0..2, T = a scratch register 3..28, usually w8).

    The original also leaves x[T] = x[S] & 1. The lifted reference does not expose it: at `ret` the lifter flushes
    only x0..x7 and x19..x29 to the context (lifter MASK_RET), so x8 is a dead scratch register by the same ABI
    assumption as every other lifted function, and the whole-context A/B compare passes with x8 untouched. The
    port is therefore a plain store of the low bit (SLEAF, store shadow); writing x8 would be a real difference
    (it was tried first and the selftest caught it)."""
    if len(insns) != 3 or not _is_ret(insns[2]):
        return None
    a, s = insns[0], insns[1]
    if a["op"] != "log_imm" or (a["sf"], a["opc"], a["imm"]) != (0, 0, 1) or a["rn"] > 2 or not 3 <= a["rd"] <= 28:
        return None
    if not _plain_mem(s, "str") or s["nbytes"] != 1 or s["rt"] != a["rd"]:
        return None
    body = "unsafe { store_low_bit(%s.wrapping_add(delta).wrapping_add(%s), %s) };\na" % (
        ARGS[s["rn"]], _hex(s["imm"]), ARGS[a["rn"]])
    return Spec("set_bool", "w%d" % a["rd"], "SLEAF", body, s["rn"], s["imm"], 1)


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


SCRATCH = range(8, 18)     # x8..x17: not flushed to the context at ret (lifter MASK_RET covers x0..x7 and x19..x29)
VREGS = range(16)          # v0..v15 are flushed at ret (MASK_RET covers v0..v15); v16..v31 are not
VBYTES = (4, 8)            # s and d registers; q (16 bytes) is not matched


def vfp_expand_imm(dbl, imm8):
    """Same expansion as tools/lifter/lower_simd_a.py vfp_expand_imm (single and double only)."""
    a, bb, cd, efgh = (imm8 >> 7) & 1, (imm8 >> 6) & 1, (imm8 >> 4) & 3, imm8 & 15
    if not dbl:
        return (a << 31) | ((bb ^ 1) << 30) | ((0x1F if bb else 0) << 25) | (cd << 23) | (efgh << 19)
    return (a << 63) | ((bb ^ 1) << 62) | ((0xFF if bb else 0) << 54) | (cd << 52) | (efgh << 48)


def _vspec(pattern, variant, vkind, body, vreg, base, off, nbytes, off2=0):
    return Spec(pattern, variant, "VLEAF", body, base, off, nbytes, vkind, vreg, off2)


def vget(insns, pc):
    """ldr sN/dN, [xB, #imm]; ret (N = 0..15). The load zeroes every other lane of the vector register."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if not _plain_mem(i, "ldr", vector=1) or i["nbytes"] not in VBYTES or i["rt"] not in VREGS:
        return None
    address = "%s.wrapping_add(%s)" % (ARGS[i["rn"]], _hex(i["imm"]))
    body = "unsafe { %s }" % _load_expr(i, address)
    return _vspec("vget", "%s%d" % ("s" if i["nbytes"] == 4 else "d", i["rt"]), 1, body, i["rt"], i["rn"], i["imm"], i["nbytes"])


def vset(insns, pc):
    """str sN/dN, [xB, #imm]; ret. Stores the low 32 or 64 bits of the vector register."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if not _plain_mem(i, "str", vector=1) or i["nbytes"] not in VBYTES or i["rt"] not in VREGS:
        return None
    n = i["nbytes"]
    value = "vlo as u32" if n == 4 else "vlo"
    body = "unsafe { %s(%s.wrapping_add(delta).wrapping_add(%s), %s) };\n0" % (
        GUEST_ST[n], ARGS[i["rn"]], _hex(i["imm"]), value)
    return _vspec("vset", "%s%d" % ("s" if n == 4 else "d", i["rt"]), 2, body, i["rt"], i["rn"], i["imm"], n)


def vconst(insns, pc):
    """fmov sN/dN, #imm; or fmov sN, wzr / dN, xzr; or movi (vzero); then ret. Writes {value, 0}."""
    if len(insns) != 2 or not _is_ret(insns[1]):
        return None
    i = insns[0]
    if i.get("rd") not in VREGS:
        return None
    if i["op"] == "fmov_imm":
        value = vfp_expand_imm(i["dbl"], i["imm8"])
        return _vspec("vconst", "%s%d_imm" % ("d" if i["dbl"] else "s", i["rd"]), 3, _hex(value), i["rd"], 0, 0, 0)
    if i["op"] == "fmov_gen" and not i["to_gen"] and i["rn"] == 31:
        return _vspec("vconst", "%s%d_zr" % ("d" if i["sf"] else "s", i["rd"]), 3, "0", i["rd"], 0, 0, 0)
    if i["op"] == "vzero":
        return _vspec("vconst", "%s%d_zero" % ("q" if i["q"] else "d", i["rd"]), 3, "0", i["rd"], 0, 0, 0)
    return None


def _two_level(insns, vector):
    """(first, second, T) for ldr xT, [xB, #a]; ldr ..., [xT, #b]; ret, or None. T is x0 (integer) or a scratch
    register that the lifted reference does not flush at ret (the same argument as set_bool)."""
    if len(insns) != 3 or not _is_ret(insns[2]):
        return None
    f, i = insns[0], insns[1]
    if not _plain_mem(f, "ldr") or f["nbytes"] != 8 or f["signed"]:
        return None
    t = f["rt"]
    if t not in SCRATCH and not (t == 0 and not vector):
        return None
    if not _plain_mem(i, "ldr", vector=vector, rn_ok=lambda n: n == t):
        return None
    return f, i, t


def getter2(insns, pc):
    """ldr xT, [xB, #a]; ldr{,b,h,sb,sh,sw} x0/w0, [xT, #b]; ret: a field of a field, no null check."""
    parts = _two_level(insns, 0)
    if not parts or parts[1]["rt"] != 0:
        return None
    f, i, _t = parts
    first = "%s.wrapping_add(%s)" % (ARGS[f["rn"]], _hex(f["imm"]))
    body = "unsafe {\n    let p = guest_ld64(%s);\n    %s\n}" % (first, _load_expr(i, "p.wrapping_add(%s)" % _hex(i["imm"])))
    return Spec("get2", _load_variant(i), "DLEAF", body, f["rn"], f["imm"], 8, 0, 0, i["imm"])


def vget2(insns, pc):
    """ldr xT, [xB, #a]; ldr sN/dN, [xT, #b]; ret."""
    parts = _two_level(insns, 1)
    if not parts or parts[1]["nbytes"] not in VBYTES or parts[1]["rt"] not in VREGS:
        return None
    f, i, _t = parts
    first = "%s.wrapping_add(%s)" % (ARGS[f["rn"]], _hex(f["imm"]))
    body = "unsafe {\n    let p = guest_ld64(%s);\n    %s\n}" % (first, _load_expr(i, "p.wrapping_add(%s)" % _hex(i["imm"])))
    return _vspec("vget2", "%s%d" % ("s" if i["nbytes"] == 4 else "d", i["rt"]), 4, body, i["rt"], f["rn"], f["imm"], 8, i["imm"])


def get_cmp(insns, pc):
    """ldr{,b,h,w} xT/wT, [xB, #imm]; cmp xT/wT, #k (k <= 15) or cmp xT/wT, xM (M = 1 or 2, not B); cset w0, eq/ne; ret.
    A boolean getter. The compare is equality only, so the flag values themselves never matter."""
    if len(insns) != 4 or not _is_ret(insns[3]):
        return None
    ld, cmp, cs = insns[0], insns[1], insns[2]
    if not _plain_mem(ld, "ldr") or ld["signed"] or ld["nbytes"] not in (1, 2, 4, 8) or ld["rt"] not in SCRATCH:
        return None
    t = ld["rt"]
    if cs["op"] != "csel" or (cs["sf"], cs["kind"], cs["rd"], cs["rn"], cs["rm"]) != (0, 1, 0, 31, 31) or cs["cond"] not in (0, 1):
        return None
    if cmp["op"] == "addsub_imm" and (cmp["sub"], cmp["s"], cmp["rd"], cmp["rn"]) == (1, 1, 31, t) and cmp["imm"] <= 15:
        other, kind = _hex(cmp["imm"]), "imm"
    elif cmp["op"] == "addsub_reg" and (cmp["sub"], cmp["s"], cmp["rd"], cmp["rn"], cmp["shift"], cmp["amt"]) == (1, 1, 31, t, 0, 0) \
            and cmp["rm"] in (1, 2) and cmp["rm"] != ld["rn"]:
        other, kind = ARGS[cmp["rm"]], "x%d" % cmp["rm"]
    else:
        return None
    n = ld["nbytes"]
    loaded = "%s(%s.wrapping_add(%s))" % (GUEST_LD[n], ARGS[ld["rn"]], _hex(ld["imm"]))
    loaded = loaded if n == 8 else "u64::from(%s)" % loaded
    mask = "u64::MAX" if cmp["sf"] else "0xffffffff"
    # csinc w0, wzr, wzr, cond is 1 when cond is false: cond eq -> result is "not equal", cond ne -> "equal"
    op = "!=" if cs["cond"] == 0 else "=="
    body = "unsafe {\n    let v = %s;\n    u64::from((v & %s) %s (%s & %s))\n}" % (loaded, mask, op, other, mask)
    variant = "u%d_%s_%s_%s" % (n * 8, "ne" if cs["cond"] == 0 else "eq", kind, "x" if cmp["sf"] else "w")
    return Spec("get_cmp", variant, "GLEAF", body, ld["rn"], ld["imm"], n)


# The shapes are disjoint (different lengths or instruction kinds), so the order does not matter.
PATTERNS = [empty, const_return, getter, nullsafe_getter, setter, set_bool, add_imm, mov_reg,
            vget, vset, vconst, getter2, vget2, get_cmp]
FAMILIES = ["empty", "const", "get", "get_nullsafe", "set", "set_bool", "add_imm", "mov_reg",
            "vget", "vset", "vconst", "get2", "vget2", "get_cmp"]


def match(insns, pc):
    for p in PATTERNS:
        spec = p(insns, pc)
        if spec:
            return spec
    return None
