"""Function lifter: decode -> IR ops -> C (task-5). Our own code.

lift_function() returns the C text of one guest function plus a small info record. Register model per
docs/RECOMPILER.md 3.4 (locals, flush dirty registers at calls, reload the result registers after calls),
with the deviations recorded in backlog/tasks/task-5.md (callee-saved registers are flushed at calls
because the callee's prologue reads them; no loop polls and no landing pads in v0).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dec  # noqa: E402
import lower as lw  # noqa: E402
import ops as OPS  # noqa: E402

import gen  # noqa: E402,F401  (imports the extension op tables so OPS is complete)

ALL_X = list(range(31))
DIRTY_SP = 31
DIRTY_V0 = 32


def bit_x(n):
    return 1 << n


def bit_v(n):
    return 1 << (32 + n)


MASK_ALL = (1 << 64) - 1
# At `ret` flush the result registers and any callee-saved register the epilogue restored, so that c->x[19..29] and
# c->v[8..15] always equal what the caller holds in its locals (conservative GC and non-ABI callees read the context).
MASK_RET = (sum(bit_x(i) for i in range(8)) | sum(bit_x(i) for i in range(19, 30)) |
            sum(bit_v(i) for i in range(16)))


def op_regs(name, f):
    """(reads, writes) as bitmasks over x0..x30, sp, v0..v31, plus (uses_flags_r, uses_flags_w)."""
    r = w = 0
    for fname, kind, role, rtype in OPS.OPS[name]["fields"]:
        if kind != "reg":
            continue
        n = f[fname]
        if rtype == "v":
            bit = bit_v(n)
        elif n == 31:
            if rtype == "x":
                continue
            bit = 1 << DIRTY_SP
        else:
            bit = bit_x(n)
        if "R" in role:
            r |= bit
        if "W" in role:
            w |= bit
    return r, w


def fmt_imm(v):
    if v < 0:
        return "%dll" % v if v > -(1 << 63) else "(s64)0x%xull" % (v & MASK_ALL)
    if v >= 1 << 63:
        return "0x%xull" % v
    return "%d" % v if v < 10 else "0x%x" % v


def emit_op(name, f):
    args = [fmt_imm(f[fn]) if kind == "imm" else str(f[fn]) for fn, kind, *_ in OPS.OPS[name]["fields"]]
    return "OP_%s(%s);" % (name, ", ".join(args))


def fname(addr):
    return "f_%x" % addr


class Unlifted(Exception):
    pass


class FuncCtx:
    def __init__(self, mod, start, end):
        self.mod, self.start, self.end = mod, start, end


def decode_range(words, base, start, end):
    """Decode every word of [start,end): list of (addr, opname, fields) or (addr, None, reason)."""
    out = []
    for a in range(start, end, 4):
        w = words[(a - base) >> 2]
        try:
            nm, f = lw.decode_any(w, a)
            out.append((a, nm, f))
        except dec.Unsupported as e:
            out.append((a, None, str(e)))
        except Exception as e:  # decoder bug: record, do not abort the whole lift
            out.append((a, None, "decoder exception %s" % type(e).__name__))
    return out


# ------------------------------------------------------------------ jump tables
def match_jump_table(insns, idx, base_addr, rd_mem, func_range):
    """Resolve `br xN` at insns[idx] using a small symbolic walk over the preceding straight-line code.
    Returns (index_reg, index_ext_opt, [target addresses]) or None."""
    sym = {}
    lo = max(0, idx - 96)
    for k in range(lo, idx):
        a, nm, f = insns[k]
        if nm is None:
            continue
        if nm == "movi":
            sym[f["rd"]] = ("c", f["imm"])
        elif nm == "addsub_imm" and not f["sub"] and f["rd"] != 31 and f["rn"] != 31:
            s = sym.get(f["rn"])
            sym[f["rd"]] = ("c", (s[1] + f["imm"]) & dec._ones(64)) if s and s[0] == "c" else None
        elif nm == "ld_x":
            s = sym.get(f["rn"])
            if s and s[0] == "c" and f["rm"] != 31:
                sym[f["rt"]] = ("tab", s[1], f["sz"], f["sgn"], f["opt"], f["amt"], f["rm"], f)
            else:
                sym[f["rt"]] = None
        elif nm == "log_reg" and f["opc"] == 1 and f["rn"] == 31 and not f["neg"] and f["amt"] == 0 and f["shift"] == 0 and f["sf"] == 1:
            sym[f["rd"]] = sym.get(f["rm"])      # mov xd, xm keeps the symbolic value (table base parked in a callee-saved reg across a call)
        elif nm in ("addsub_reg", "addsub_ext") and not f["sub"]:
            sa, sb = sym.get(f["rn"]), sym.get(f["rm"])
            sh = f["amt"] if nm == "addsub_reg" else f["amt"]
            if sa and sa[0] == "c" and sb and sb[0] == "tab":
                sym[f["rd"]] = ("sum", sa[1], sb, sh, nm, f)
            elif sa and sa[0] == "tab" and sb and sb[0] == "c":
                sym[f["rd"]] = ("sum", sb[1], sa, sh, nm, f)
            else:
                sym[f["rd"]] = None
        else:
            try:
                r, w = op_regs(nm, f)
            except Exception:
                continue
            for n in range(31):
                if w & bit_x(n):
                    sym[n] = None
    br = insns[idx][2]
    s = sym.get(br["rn"])
    if not s or s[0] != "sum":
        return None
    _, base, tab, sh, nmadd, fadd = s
    _, taddr, sz, sgn, opt, amt, idxreg, fld = tab
    # table bound: look back for  cmp xI, #N ; b.hi/b.hs
    # the index often reaches the table load through a copy (mov w8, w1 after cmp w1, #N): every register it was copied from
    # holds the same value, so a compare on any alias bounds the table
    aliases = {idxreg}
    for k in range(idx - 1, max(-1, idx - 48), -1):
        a, nm, f = insns[k]
        if nm == "log_reg" and f["opc"] == 1 and f["rn"] == 31 and not f["neg"] and f["amt"] == 0 and f["shift"] == 0                 and f["rd"] in aliases and f["rm"] != 31:
            aliases.add(f["rm"])
    bound = None
    # a guard `cmp xI, #N ; b.ls/b.lo <table>` that branches straight to the table setup just above the br (the guard can sit in
    # another block, with unrelated compares of the same register in between)
    br_addr = insns[idx][0]
    # value numbering over the straight-line window: the compare can sit on a register that holds the same value as the index
    # through `and x, y, #0xffffffff` / `and w, w, #0xffff` (masks that a bounded compare makes a no-op) as well as plain copies
    cmpcore = {}
    vn = {}
    lo_w = max(0, idx - 700)

    def vn_of(r):
        return vn.get(r, ("in", r))

    def core(e):
        while e[0] == "and":
            e = e[1]
        return e

    for k in range(lo_w, idx):
        a, nm, f = insns[k]
        if nm is None:
            continue
        if nm == "addsubs_imm" and f["sub"] and f["rd"] == 31:
            cmpcore[k] = core(vn_of(f["rn"]))
            continue
        if nm == "log_reg" and f["opc"] == 1 and f["rn"] == 31 and not f["neg"] and f["amt"] == 0 and f["shift"] == 0 and f["rm"] != 31:
            vn[f["rd"]] = vn_of(f["rm"])
            continue
        if nm == "log_imm" and f["opc"] == 0 and f["imm"] & (f["imm"] + 1) == 0 and f["rd"] != 31 and f["rn"] != 31:
            vn[f["rd"]] = ("and", vn_of(f["rn"]), f["imm"])
            continue
        try:
            _r, wr = op_regs(nm, f)
        except Exception:
            continue
        for n in range(31):
            if wr & bit_x(n):
                vn[n] = ("d", k, n)
    idx_core = core(vn_of(idxreg))

    def rewritten(reg, k):
        for j in range(k + 1, idx):
            _a, _nm, _f = insns[j]
            if _nm is None:
                continue
            try:
                _r, _w = op_regs(_nm, _f)
            except Exception:
                continue
            if _w & bit_x(reg):
                return True
        return False

    def cmp_ok(k, f):
        if cmpcore.get(k) == idx_core:
            return True
        if f["rn"] in aliases:
            # a compare on a stale value of the register (it was reloaded since) says nothing about the index; compares far
            # above the window cannot be told apart and stay accepted (the guard branch must still land at the table setup)
            return k < lo_w or not rewritten(f["rn"], k)
        return False

    def guard_lands(k, f):
        # `cmp xI, #N ; b.ls/b.lo <setup>` jumps straight to the table setup: the compared value is the index as long as nothing
        # rewrites the register between the branch target and the br
        nxt = insns[k + 1] if k + 1 < len(insns) else None
        if not nxt or nxt[1] != "bcond" or not (br_addr - 0x80 <= nxt[2]["target"] <= br_addr):
            return False
        ti = (nxt[2]["target"] - insns[0][0]) >> 2
        return 0 <= ti < idx and not rewritten(f["rn"], ti - 1)

    for k in range(idx - 1, max(-1, idx - 4000), -1):
        a, nm, f = insns[k]
        if nm == "addsubs_imm" and f["sub"] and f["rd"] == 31 and (cmp_ok(k, f) or (f["rn"] in aliases and guard_lands(k, f))) and k + 1 < len(insns):
            nxt = insns[k + 1]
            if nxt[1] == "bcond" and nxt[2]["cond"] in (3, 9) and br_addr - 0x80 <= nxt[2]["target"] <= br_addr and f["imm"] < 4096:
                bound = f["imm"] + 1 if nxt[2]["cond"] == 9 else f["imm"]
                break
    for k in range(idx - 1, max(-1, idx - 80), -1):
        if bound is not None:
            break
        a, nm, f = insns[k]
        if nm == "addsubs_imm" and f["sub"] and f["rd"] == 31 and cmp_ok(k, f):
            bound = f["imm"] + 1
            nxt = insns[k + 1] if k + 1 < len(insns) else None
            if nxt and nxt[1] == "bcond" and nxt[2]["cond"] in (2,):  # hs: >= N goes to default
                bound = f["imm"]
            break
    if bound is None:
        # compare in another basic block: nearest `cmp xI, #N` of the same register anywhere in the function
        order = list(range(idx - 1, -1, -1)) + list(range(idx + 1, len(insns)))
        for k in order:
            a, nm, f = insns[k]
            if nm == "addsubs_imm" and f["sub"] and f["rd"] == 31 and (f["rn"] in aliases or cmpcore.get(k) == idx_core) and f["imm"] < 4096:
                nxt = insns[k + 1] if k + 1 < len(insns) else None
                if nxt and nxt[1] == "bcond" and nxt[2]["cond"] in (2, 8, 9, 12):
                    # hs: >= N exits; hi: > N exits; ls/le style forms enter the table below the bound
                    bound = f["imm"] if nxt[2]["cond"] == 2 else f["imm"] + 1
                    break
    if bound is None:
        # index masked by `and xI, xS, #2^k-1` (also tracks one level of mov/uxtw)
        for k in range(idx - 1, max(-1, idx - 40), -1):
            a, nm, f = insns[k]
            if nm is None:
                continue
            try:
                _, wr = op_regs(nm, f)
            except Exception:
                continue
            if wr & bit_x(idxreg):
                if nm == "log_imm" and f["opc"] == 0 and f["imm"] & (f["imm"] + 1) == 0 and f["imm"] < 4096:
                    bound = f["imm"] + 1
                break
    if bound is None:
        # last resort: a guard on a register that derives from the same source as the index (range reduction through
        # sub/csel/and/mov chains: `w21 = w1 - 15; cmp w21, #4; b.hi default; w24 = csel(lt, ..., w21)`). Tracks, per register,
        # the set of values it derives from over the preceding straight-line window; the nearest guard that shares a source wins.
        dep = {}

        def dep_of(r):
            return dep.get(r) or frozenset([("in", r)])

        guards = []
        for k in range(max(0, idx - 600), idx):
            a, nm, f = insns[k]
            if nm is None:
                continue
            if nm == "addsubs_imm" and f["sub"] and f["rd"] == 31 and f["imm"] < 4096 and k + 1 < len(insns):
                nxt = insns[k + 1]
                if nxt[1] == "bcond" and nxt[2]["cond"] in (2, 8):
                    guards.append((k, f["imm"] if nxt[2]["cond"] == 2 else f["imm"] + 1, dep_of(f["rn"])))
                continue
            try:
                rd_, wr_ = op_regs(nm, f)
            except Exception:
                continue
            if nm in ("log_reg", "log_imm", "addsub_imm", "addsub_reg", "csel") and f.get("rd", 31) != 31:
                srcs = frozenset().union(*[dep_of(f[x]) for x in ("rn", "rm") if x in f and f[x] != 31])
                dep[f["rd"]] = srcs if srcs else frozenset([("d", k)])
            else:
                for n in range(31):
                    if wr_ & bit_x(n):
                        dep[n] = frozenset([("d", k, n)])
        want = dep_of(idxreg)
        for k, bd, dp in reversed(guards):
            if dp & want and all(x[0] != "in" or x[1] != 31 for x in dp & want):
                bound = bd
                break
    if bound is None:
        return None
    targets = []
    for i in range(bound):
        ea = taddr + i * sz
        raw = rd_mem(ea, sz)
        if raw is None:
            return None
        if sgn:
            raw = raw - (1 << (8 * sz)) if raw >> (8 * sz - 1) else raw
        targets.append((base + (raw << sh)) & dec._ones(64))
    # The inventory often splits a switch from its case blocks (each block has its own FDE), so targets outside func_range are
    # expected. They are accepted when aligned and near the br (byte, halfword and word offset tables stay within the window);
    # the emitter turns them into tail calls to the function that owns the case block.
    if any(t & 3 or t < base_addr or abs(t - br_addr) > 0x40000 for t in targets):
        return None
    return idxreg, opt, targets


# ------------------------------------------------------------------ the function lifter
class Lifter:
    def __init__(self, mod):
        """mod: dict(name, base, words (indexable by word), starts (set of function starts), rd_mem(addr,size))"""
        self.mod = mod
        self.starts = mod["starts"]
        self.summ_cache = {}
        self.eh = mod.get("eh")                  # eh_tables.EhInfo or None (task-14)
        self.redirect = mod.get("redirect", {})  # abs addr -> EH ABI name, called through the dispatch table
        self.polls = mod.get("polls", True)      # GC safepoint polls on loop back-edges (task-14)

    def callee_writes(self, addr):
        """Registers written by a small leaf-like callee (outlined helper detection), else None for standard."""
        if addr in self.summ_cache:
            return self.summ_cache[addr]
        res = None
        m = self.mod
        end = m["ends"].get(addr)
        if end and end - addr <= 160:
            insns = decode_range(m["words"], m["base"], addr, end)
            w = 0
            frame = False
            for a, nm, f in insns:
                if nm is None:
                    w = None
                    break
                if nm in dec_control():
                    continue
                if nm in ("stp_wb", "st_wb", "addsub_imm") and f.get("rn") == 31:
                    frame = True
                try:
                    w |= op_regs(nm, f)[1]
                except Exception:
                    pass
            if w is not None and not frame:
                extra = w & (sum(bit_x(i) for i in range(2, 19)) | sum(bit_v(i) for i in range(4, 32)))
                if extra:
                    res = extra | bit_x(0) | bit_x(1)
        self.summ_cache[addr] = res
        return res

    def lift(self, start, end, entry=None):
        """Lift [start, end) as one function entered at `entry` (default start). A synthetic entry (entry > start) is a
        mid-function target: only code reachable from it is emitted, and branches back below it stay local gotos."""
        m = self.mod
        entry = start if entry is None else entry
        insns = decode_range(m["words"], m["base"], start, end)
        info = dict(addr=entry, insns=len(insns), jt=0, jt_unresolved=0, bad=None, tail=0, calls=0, mid=0, mid_t=set())
        pos = {a: i for i, (a, _, _) in enumerate(insns)}
        # ---- exception call sites (task-14): LSDA answer per call, landing pads become leaders and roots
        sites = {}
        if self.eh:
            for a, nm, f in insns:
                if nm in ("bl", "blr"):
                    s = self.eh.site(a + 4)
                    if s[0]:
                        sites[a] = s
        site_pads = {a: s[1] for a, s in sites.items() if s[0] == 1 and start <= s[1] < end}
        pad_roots = set()
        self._sites = sites
        has_pad = any(s[0] == 1 for s in sites.values())
        info["eh_sites"] = len(sites)
        info["eh_pads"] = sum(1 for s in sites.values() if s[0] == 1)
        info["eh_pads_out"] = sum(1 for s in sites.values() if s[0] == 1 and not (start <= s[1] < end))
        info["eh_nosite"] = sum(1 for s in sites.values() if s[0] == 3)
        # ---- reachability and leaders
        leaders = {entry}
        jt = {}
        bad = None
        work = [entry]
        seen = set()
        reach = set()
        while True:
            while work:
                a = work.pop()
                while start <= a < end and a not in seen:
                    seen.add(a)
                    i = pos[a]
                    _, nm, f = insns[i]
                    if nm is None:
                        if bad is None:
                            bad = (a, f)
                        break
                    reach.add(a)
                    nxt = a + 4
                    if nm == "b":
                        t = f["target"]
                        if start <= t < end:
                            leaders.add(t)
                            work.append(t)
                        break
                    if nm in ("bcond", "cbz", "tbz"):
                        t = f["target"]
                        if start <= t < end:
                            leaders.add(t)
                            work.append(t)
                        leaders.add(nxt)
                    elif nm == "br":
                        r = match_jump_table(insns, i, m["base"], m["rd_mem"], (start, end))
                        if r:
                            jt[a] = r
                            for t in r[2]:
                                if start <= t < end:
                                    leaders.add(t)
                                    work.append(t)
                        else:
                            # indirect jump through a loaded pointer (virtual/import tail call) vs an unresolved switch
                            org = None
                            for k in range(i - 1, max(-1, i - 30), -1):
                                nm2, f2 = insns[k][1], insns[k][2]
                                if nm2 is None:
                                    continue
                                try:
                                    if op_regs(nm2, f2)[1] & bit_x(f["rn"]):
                                        org = nm2
                                        break
                                except Exception:
                                    pass
                            if org in ("addsub_reg", "addsub_ext"):
                                info["jt_unresolved"] += 1      # a computed switch we could not bound
                            else:
                                info["br_tail_ptr"] = info.get("br_tail_ptr", 0) + 1
                        break
                    elif nm in ("ret", "brk"):
                        break
                    a = nxt
            new = sorted({p for a, p in site_pads.items() if a in reach and p not in pad_roots})
            if not new:
                break
            pad_roots.update(new)
            leaders.update(new)
            work.extend(new)
        if bad:
            info["bad"] = "%x: %s" % bad
            raise Unlifted(info["bad"])
        # ---- basic-block dirty-register dataflow (forward, union at joins)
        order = [a for a, _, _ in insns if a in reach]
        # dirty-in per leader, iterate to fixpoint over the instruction stream
        din = {a: 0 for a in leaders}
        din[entry] = 0
        used_r = used_w = 0
        flags_used = False
        for a in order:
            _, nm, f = insns[pos[a]]
            if nm in dec_control():
                if nm in ("cbz", "tbz"):
                    key = "rt"
                    if f["rt"] != 31:
                        used_r |= bit_x(f["rt"])
                if nm in ("br", "blr", "ret") and f["rn"] != 31:
                    used_r |= bit_x(f["rn"])
                if nm == "bcond":
                    flags_used = True
                continue
            r, w = op_regs(nm, f)
            used_r |= r
            used_w |= w
            if OPS.OPS[nm]["fR"] or OPS.OPS[nm]["fW"]:
                flags_used = True
        # simple whole-function approximation of dirty state: iterate until stable
        changed = True
        lines_for = {}
        iterations = 0
        while changed and iterations < 8:
            changed = False
            iterations += 1
            dirty = 0
            for a in order:
                if a in leaders:
                    nd = din[a] | dirty if (a - 4) in reach and insns[pos[a - 4]][1] not in ("b", "br", "ret") else din[a]
                    if nd != din[a]:
                        din[a] = nd
                        changed = True
                    dirty = din[a]
                elif (a - 4) not in reach:
                    dirty = din.get(a, 0)
                _, nm, f = insns[pos[a]]
                if nm in dec_control():
                    if nm in ("b", "bcond", "cbz", "tbz"):
                        t = f["target"]
                        if start <= t < end:
                            if t in leaders and (din[t] | dirty) != din[t]:
                                din[t] |= dirty
                                changed = True
                        else:  # leaves via tail call: flushes everything
                            pass
                        if nm == "b":
                            dirty = 0
                    elif nm == "br":
                        if a in jt:
                            for t in jt[a][2]:
                                if t in din and (din[t] | dirty) != din[t]:
                                    din[t] |= dirty
                                    changed = True
                        dirty = 0
                    elif nm in ("bl", "blr", "svc", "brk"):
                        dirty = self.after_flush(dirty, nm)
                    elif nm == "ret":
                        dirty = 0
                    continue
                dirty |= op_regs(nm, f)[1]
        live_on = self.polls and os.environ.get("SMRPG_POLL_LIVE", "1") != "0"
        self._live = self.poll_liveness(order, insns, pos, reach, start, end, jt) if live_on else {}
        # ---- emit
        L = []
        P = L.append
        regs_decl = []
        has_call = any(nm in ("bl", "blr") for _, nm, _f in insns if nm)
        # a function with landing pads holds every callee-saved register in a local: at a pad the throwing callee
        # has not run its epilogue, so the pad rewrites c->x[19..29], sp and v8..v15 from the locals (doc 4.4)
        xs = [n for n in range(31) if (used_r | used_w) & bit_x(n) or (n == 30 and has_call) or (has_pad and 19 <= n <= 29)]
        xs_set = set(xs)
        self._vs = set(n for n in range(32) if (used_r | used_w) & bit_v(n) or (has_pad and 8 <= n <= 15))
        P("void %s(Ctx *restrict c) {" % fname(entry))
        if xs:
            P("  u64 %s;" % ", ".join("x%d" % n for n in xs))
        P("  u64 x31 = 0, sp = c->x[31];")
        vs = sorted(self._vs)
        if vs:
            P("  v128 %s;" % ", ".join("v%d" % n for n in vs))
        if flags_used:
            P("  u8 fl[4] = {0, 0, 0, 0};")
        # every declared register starts from the context: a dirty-set merge at a loop head may flush a register on a
        # path where it was not yet written, and that must write back the original value, not garbage
        for n in xs:
            P("  x%d = c->x[%d];" % (n, n))
        for n in vs:
            P("  v%d = c->v[%d];" % (n, n))
        top = order[0] == entry
        if top:
            P("L_%x:;" % entry)
        else:
            P("goto L_%x;" % entry)
        dirty = 0
        prev_falls = True
        for a in order:
            _, nm, f = insns[pos[a]]
            if a in leaders and not (top and a == entry):
                P("L_%x:;" % a)
                dirty = din[a]
            elif (a - 4) not in reach and not (top and a == entry):
                dirty = din.get(a, dirty)
            if nm in dec_control():
                dirty = self.emit_control(P, insns, pos, a, nm, f, start, end, leaders, jt, dirty, info, xs_set)
                continue
            P("  " + emit_op(nm, f))
            dirty |= op_regs(nm, f)[1]
        P("}")
        info["jt"] = len(jt)
        info["callees"] = info.get("callees", set())
        return "\n".join(L), info

    # ---- helpers for control flow
    def after_flush(self, dirty, nm):
        # after a call everything dirty was flushed; x0..x8 / v0..v7 reloaded (clean)
        return 0

    def flush_lines(self, dirty, mask):
        out = []
        m = dirty & mask
        for n in range(31):
            if m & bit_x(n):
                out.append("c->x[%d] = x%d;" % (n, n))
        if m & (1 << DIRTY_SP):
            out.append("c->x[31] = sp;")
        for n in range(32):
            if m & bit_v(n):
                out.append("c->v[%d] = v%d;" % (n, n))
        return out

    def exc_lines(self, a, start, end, xs_set):
        """What runs right after the call at a when c->exc is set (doc 4.4, task-14)."""
        s = self._sites.get(a)
        if not s or s[0] in (0, 2):
            return ["if (__builtin_expect(c->exc != 0, 0)) return;"]
        if s[0] == 3:
            return ["if (__builtin_expect(c->exc != 0, 0)) { eh_no_site(c, 0x%xull); return; }" % a]
        _k, pad, lsda, action = s
        out = ["if (__builtin_expect(c->exc != 0, 0)) {",
               "  if (!eh_land(c, 0x%xull, 0x%xull)) return;" % (lsda, action)]
        restore = ["c->x[%d] = x%d;" % (n, n) for n in range(19, 30) if n in xs_set] + ["c->x[31] = sp;"]
        restore += ["c->v[%d] = v%d;" % (n, n) for n in range(8, 16) if n in self._vs]
        out.append("  " + " ".join(restore))
        if start <= pad < end:
            ld = ["x%d = c->x[%d];" % (n, n) for n in (0, 1) if n in xs_set]
            if ld:
                out.append("  " + " ".join(ld))
            out.append("  goto L_%x;" % pad)
        else:
            out.append("  c->pc = 0x%xull; __attribute__((musttail)) return rt_dispatch(c);" % pad)
        out.append("}")
        return out

    def poll_liveness(self, order, insns, pos, reach, start, end, jt):
        """Guest registers live before each back-edge branch (task-14). A poll flushes only dirty AND live registers:
        a dead register is not a GC root (nothing reads it again), and flushing every dirty temporary doubled clang
        time (SLP vectorizer and register allocator) on large chunks. Calls, svc, returns and exits read everything."""
        back = []
        for a in order:
            _, nm, f = insns[pos[a]]
            if nm in ("b", "bcond", "cbz", "tbz") and start <= f["target"] <= a:
                back.append(a)
            elif nm == "br" and a in jt and any(start <= t <= a for t in jt[a][2]):
                back.append(a)
        if not back:
            return {}
        ALL = MASK_ALL
        live = {}
        ctl = dec_control()
        changed = True
        while changed:
            changed = False
            for a in reversed(order):
                _, nm, f = insns[pos[a]]
                nxt = live.get(a + 4, 0) if (a + 4) in reach and a + 4 < end else ALL
                if nm in ctl:
                    if nm in ("b", "bcond", "cbz", "tbz"):
                        t = f["target"]
                        lt = live.get(t, 0) if start <= t < end else ALL
                        v = lt if nm == "b" else lt | nxt
                        if nm in ("cbz", "tbz") and f["rt"] != 31:
                            v |= bit_x(f["rt"])
                    elif nm == "br" and a in jt:
                        v = bit_x(jt[a][0]) if jt[a][0] != 31 else 0
                        for t in jt[a][2]:
                            v |= live.get(t, 0) if start <= t < end else ALL
                    else:                       # bl, blr, svc, brk, ret, br out: the callee or caller may read anything
                        v = ALL
                else:
                    r, w = op_regs(nm, f)
                    v = r | (nxt & ~w)
                old = live.get(a, 0)
                if v | old != old:              # least fixpoint: sets only grow from empty
                    live[a] = v | old
                    changed = True
        return {a: live.get(a, ALL) for a in back}

    def poll_lines(self, a, dirty):
        """GC safepoint on a loop back-edge (doc 4.5): flush what is dirty and live, only on the slow path."""
        if not self.polls:
            return []
        live = self._live.get(a, MASK_ALL) | (1 << DIRTY_SP)      # sp bounds the collector's stack scan: always
        fl =" ".join(self.flush_lines(dirty & live, MASK_ALL))
        # dead dirty registers get 0, not their stale context value: a loop that never calls would otherwise show the
        # collector the same stale pointers on every round and keep that garbage alive forever (seen: a mutator
        # spinning in alloc livelocked the fixed test heap). A constant store extends no live range; the context
        # copy of a dirty register is read only by the collector.
        dead = dirty & ~live
        z = ["c->x[%d] = 0;" % n for n in range(31) if dead & bit_x(n)]
        z += ["c->v[%d] = (v128){0, 0};" % n for n in range(32) if dead & bit_v(n)]
        if z:
            fl = (fl + " " if fl else "") + " ".join(z)
        return ["if (__builtin_expect(g_gc_pending != 0, 0)) { %s c->pc = 0x%xull; gc_poll(c); }" % (fl + " " if fl else "", a)]

    def emit_control(self, P, insns, pos, a, nm, f, start, end, leaders, jt, dirty, info, xs_set):
        m = self.mod
        starts = self.starts

        def reload_after(callee_w=None):
            outl = []
            xs = [0, 1] + ([n for n in range(2, 19) if callee_w and callee_w & bit_x(n)] if callee_w else [])
            for n in xs:
                if n in xs_set:
                    outl.append("x%d = c->x[%d];" % (n, n))
            vv = [0, 1, 2, 3] + ([n for n in range(4, 32) if callee_w and callee_w & bit_v(n)] if callee_w else [])
            for n in vv:
                if n in self._vs:
                    outl.append("v%d = c->v[%d];" % (n, n))
            return outl

        def declared_reload(lines):
            # only reload registers the function declares (others are unused)
            return [l for l in lines]

        def flush_all(d):
            for l in self.flush_lines(d, MASK_ALL):
                P("  " + l)

        def target_call(t, tail):
            """Emit a direct call or tail call to guest address t (flushes first)."""
            if t in starts:
                return "%s(c)" % fname(t), True
            info["mid"] += 1; info["mid_t"].add(t)
            return None, False

        if nm == "b" or nm in ("bcond", "cbz", "tbz"):
            t = f["target"]
            if nm == "b":
                cond = None
            elif nm == "bcond":
                cond = "ir_cond(fl, %d)" % f["cond"]
            elif nm == "cbz":
                reg = "x%d" % f["rt"] if f["rt"] != 31 else "x31"
                v = reg if f["sf"] else "(u32)%s" % reg
                cond = "%s %s 0" % (v, "!=" if f["nz"] else "==")
            else:
                reg = "x%d" % f["rt"] if f["rt"] != 31 else "x31"
                cond = "((%s >> %d) & 1) %s 0" % (reg, f["bit"], "!=" if f["nz"] else "==")
            if start <= t < end:
                if t <= a:
                    info["polls"] = info.get("polls", 0) + 1
                    for l in self.poll_lines(a, dirty):
                        P("  " + l)
                if cond is None:
                    P("  goto L_%x;" % t)
                else:
                    P("  if (%s) goto L_%x;" % (cond, t))
                return 0 if cond is None else dirty
            # leaves the function: tail call
            info["tail"] += 1
            body = []
            for l in self.flush_lines(dirty, MASK_ALL):
                body.append(l)
            if t in self.redirect:
                info["redirects"] = info.get("redirects", 0) + 1
                body.append("c->pc = 0x%xull; __attribute__((musttail)) return rt_dispatch(c);" % t)
            elif t in starts:
                body.append("__attribute__((musttail)) return %s(c);" % fname(t))
            else:
                info["mid"] += 1; info["mid_t"].add(t)
                body.append("c->pc = 0x%xull; __attribute__((musttail)) return rt_dispatch(c);" % t)
            if cond is None:
                for l in body:
                    P("  " + l)
                return 0
            P("  if (%s) {" % cond)
            for l in body:
                P("    " + l)
            P("  }")
            return dirty
        if nm == "bl":
            t = f["target"]
            info["calls"] += 1
            P("  x30 = 0x%xull;" % (a + 4))
            d = dirty | bit_x(30)
            flush_all(d)
            cw = self.callee_writes(t) if t in starts and t not in self.redirect else None
            if t in self.redirect:
                info["redirects"] = info.get("redirects", 0) + 1
                P("  rt_icall(c, 0x%xull);" % t)
            elif t in starts:
                P("  %s(c);" % fname(t))
            else:
                info["mid"] += 1; info["mid_t"].add(t)
                P("  c->pc = 0x%xull; rt_dispatch(c);" % t)
            for l in self.exc_lines(a, start, end, xs_set):
                P("  " + l)
            for l in reload_after(cw):
                P("  " + l)
            return 0
        if nm == "blr":
            info["calls"] += 1
            P("  { u64 _t = x%d; x30 = 0x%xull;" % (f["rn"], a + 4))
            d = dirty | bit_x(30)
            for l in self.flush_lines(d, MASK_ALL):
                P("    " + l)
            P("    rt_icall(c, _t); }")
            for l in self.exc_lines(a, start, end, xs_set):
                P("  " + l)
            for l in reload_after(None):
                P("  " + l)
            return 0
        if nm == "ret":
            for l in self.flush_lines(dirty & ~(1 << DIRTY_SP), MASK_RET):
                P("  " + l)
            # sp after a return is the caller's sp. Our callees flushed deeper values into c->x[31] while they ran, and a caller
            # whose sp is clean never re-flushes it; the next interpreter or tail dispatch would then pop its frame from the
            # wrong address (task-13: a restored x19 and the saved registers came from a deeper frame).
            P("  c->x[31] = sp;")
            P("  return;")
            return 0
        if nm == "br":
            if a in jt:
                idxreg, opt, targets = jt[a]
                if any(start <= t <= a for t in targets):
                    info["polls"] = info.get("polls", 0) + 1
                    for l in self.poll_lines(a, dirty):
                        P("  " + l)
                # Switch on the computed target address (the br register), not on the table index register: compilers routinely reuse the
                # index register between the table load and the br (task-13: `mov x8, x0` after `ldrb w11, [x9, x8]` made every lookup
                # take case 0). A target outside the recovered set falls back to the dispatcher instead of aborting.
                P("  switch (x%d) {" % f["rn"])
                seen = set()
                for i, t in enumerate(targets):
                    if t in seen:
                        continue
                    seen.add(t)
                    if start <= t < end:
                        P("  case 0x%xull: goto L_%x;" % (t, t))
                    else:
                        if t not in starts:
                            info["mid"] += 1; info["mid_t"].add(t)
                        P("  case 0x%xull: c->pc = 0x%xull; goto TAIL_PC_%x;" % (t, t, a))
                P("  default: c->pc = x%d; goto TAIL_PC_%x;" % (f["rn"], a))
                P("  }")
                P("  TAIL_PC_%x:;" % a)
                for l in self.flush_lines(dirty, MASK_ALL):
                    P("  " + l)
                P("  __attribute__((musttail)) return rt_dispatch(c);")
                return 0
            for l in self.flush_lines(dirty, MASK_ALL):
                P("  " + l)
            P("  c->pc = x%d; __attribute__((musttail)) return rt_dispatch(c);" % f["rn"])
            return 0
        if nm == "svc":
            for l in self.flush_lines(dirty, MASK_ALL):
                P("  " + l)
            P("  c->pc = 0x%xull; rt_svc(c, %d);" % (a + 4, f["imm"]))
            for n in range(9):
                P("  x%d = c->x[%d];" % (n, n)) if n in xs_set else None
            return 0
        if nm == "brk":
            for l in self.flush_lines(dirty, MASK_ALL):
                P("  " + l)
            P("  c->pc = 0x%xull; rt_brk(c, %d); return;" % (a, f["imm"]))
            return 0
        raise Unlifted("control %s" % nm)


def dec_control():
    return lw.CONTROL
