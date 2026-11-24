"""Lower decoded A64 instructions to C for the task-4 spike. Our own code.

Two register models:
  ctx   - every guest register lives in the Ctx struct (memory), as a naive lifter does.
  local - guest registers are C locals loaded on entry; written registers are flushed to Ctx
          before calls, svc and returns, and only caller-saved registers are reloaded after a
          call (AAPCS64 trust). The same rule gives correct callee-saved values at C++ landing pads.
Options: exc  - test c->exc after every call (unwind-by-return exceptions)
         poll - safepoint poll on loop back-edges (cooperative GC suspend)
"""

import struct

from a64 import Unsupported

COND = ["fz", "!fz", "fc", "!fc", "fn", "!fn", "fv", "!fv", "(fc&&!fz)", "!(fc&&!fz)", "(fn==fv)", "(fn!=fv)",
        "(!fz&&fn==fv)", "!(!fz&&fn==fv)", "1", "1"]

SYSREG = {0xDE82: "tpidr", 0xDE83: "tpidrro", 0xDA20: "fpcr", 0xDA21: "fpsr"}
CALLER_SAVED_X = set(range(0, 19))


def vfp_expand(imm8, dbl):
    s, b6, mid, frac = imm8 >> 7, (imm8 >> 6) & 1, (imm8 >> 4) & 3, imm8 & 0xF
    if dbl:
        exp = ((b6 ^ 1) << 10) | ((0xFF if b6 else 0) << 2) | mid
        return (s << 63) | (exp << 52) | (frac << 48)
    exp = ((b6 ^ 1) << 7) | ((0x1F if b6 else 0) << 2) | mid
    return (s << 31) | (exp << 23) | (frac << 19)


class Fn:
    def __init__(self, start, end, words, model, exc=False, poll=False, known=None, abi_flush=True):
        self.abi_flush = abi_flush
        self.start, self.end, self.words = start, end, words
        self.model, self.exc, self.poll = model, exc, poll
        self.ux, self.wx, self.uv, self.wv = set(), set(), set(), set()
        self.callees = set()
        self.uses_dispatch = False
        self.known = known

    # ---- register access -------------------------------------------------------------------
    def X(self, n, write=False):
        (self.wx if write else self.ux).add(n)
        self.ux.add(n)
        return "x%d" % n if self.model == "local" else "c->x[%d]" % n

    def SP(self, write=False):
        (self.wx if write else self.ux).add(32)
        self.ux.add(32)
        return "sp" if self.model == "local" else "c->sp"

    def F(self, f, write=False):
        key = 40 + "nzcv".index(f)
        (self.wx if write else self.ux).add(key)
        self.ux.add(key)
        return "f" + f if self.model == "local" else "c->f" + f

    def VL(self, n, write=False):
        (self.wv if write else self.uv).add(n)
        self.uv.add(n)
        return "v%dl" % n if self.model == "local" else "c->v[%d].l" % n

    def VH(self, n, write=False):
        (self.wv if write else self.uv).add(n + 32)
        self.uv.add(n + 32)
        return "v%dh" % n if self.model == "local" else "c->v[%d].h" % n

    def rx(self, n, sf, sp_ok=False):
        if n == 31:
            if not sp_ok:
                return "0"
            e = self.SP()
        else:
            e = self.X(n)
        return e if sf else "((u32)%s)" % e

    def wx_(self, n, sf, expr, sp_ok=False):
        if n == 31 and not sp_ok:
            return "(void)(%s);" % expr
        d = self.SP(True) if n == 31 else self.X(n, True)
        return "%s = %s;" % (d, expr if sf else "(u64)(u32)(%s)" % expr)

    def cond(self, c):
        s = COND[c]
        for f in "nzcv":
            if "f" + f in s:
                s = s.replace("f" + f, self.F(f))
        return s

    def set_flags(self, n, z, c, v):
        return "%s=%s; %s=%s; %s=%s; %s=%s;" % (self.F("n", 1), n, self.F("z", 1), z, self.F("c", 1), c,
                                                 self.F("v", 1), v)

    # ---- helpers ----------------------------------------------------------------------------
    @staticmethod
    def T(sf):
        return "u64" if sf else "u32"

    def shifted(self, val, shift, amt, sf):
        T = self.T(sf)
        S = "i64" if sf else "i32"
        if amt == 0:
            return "((%s)%s)" % (T, val)
        if shift == 0:
            return "((%s)((%s)%s << %d))" % (T, T, val, amt)
        if shift == 1:
            return "((%s)%s >> %d)" % (T, val, amt)
        if shift == 2:
            return "((%s)((%s)(%s)%s >> %d))" % (T, S, T, val, amt)
        return "ror%d((%s)%s, %d)" % (64 if sf else 32, T, val, amt)

    @staticmethod
    def extend(val, option, amt):
        e = ["(u64)(u8)%s", "(u64)(u16)%s", "(u64)(u32)%s", "(u64)%s", "(u64)(i64)(i8)%s", "(u64)(i64)(i16)%s",
             "(u64)(i64)(i32)%s", "(u64)%s"][option] % val
        return "(%s << %d)" % (e, amt) if amt else e

    def addsub(self, d, a, bexpr, sf, sub, s, sp_dst):
        T = self.T(sf)
        top = 63 if sf else 31
        if not s:
            return self.wx_(d, sf, "(%s)%s %s (%s)%s" % (T, a, "-" if sub else "+", T, bexpr), sp_ok=sp_dst)
        if sub:
            fl = self.set_flags("r_>>%d" % top, "r_==0", "a_>=b_", "((a_^b_)&(a_^r_))>>%d" % top)
            body = "%s r_ = a_ - b_; %s" % (T, fl)
        else:
            fl = self.set_flags("r_>>%d" % top, "r_==0", "r_<a_", "((a_^r_)&(b_^r_))>>%d" % top)
            body = "%s r_ = a_ + b_; %s" % (T, fl)
        return "{ %s a_ = %s; %s b_ = %s; %s %s }" % (T, a, T, bexpr, body, self.wx_(d, sf, "r_"))

    # ---- memory -----------------------------------------------------------------------------
    def addr(self, i):
        base = self.rx(i["rn"], 1, sp_ok=True)
        if i.get("rm") is not None:
            off = self.extend(self.rx(i["rm"], 1), i["option"], i["amt"])
            return "u64 a_ = %s + %s;" % (base, off), ""
        imm = i["imm"]
        wb = i.get("wb")
        if wb == "pre":
            return "u64 a_ = %s + (u64)%dLL;" % (base, imm), self.wx_(i["rn"], 1, "a_", sp_ok=True)
        if wb == "post":
            return "u64 a_ = %s;" % base, self.wx_(i["rn"], 1, "a_ + (u64)%dLL" % imm, sp_ok=True)
        return "u64 a_ = %s + (u64)%dLL;" % (base, imm), ""

    def load_to(self, rt, nb, v, signed, dst64, a):
        if v:
            if nb == 16:
                return "%s = LD64(%s); %s = LD64(%s + 8);" % (self.VL(rt, 1), a, self.VH(rt, 1), a)
            return "%s = (u64)LD%d(%s); %s = 0;" % (self.VL(rt, 1), nb * 8, a, self.VH(rt, 1))
        ld = "LD%d(%s)" % (nb * 8, a)
        if signed:
            e = "(u64)(i64)(i%d)%s" % (nb * 8, ld) if dst64 else "(u32)(i32)(i%d)%s" % (nb * 8, ld)
            return self.wx_(rt, 1 if dst64 else 0, e)
        return self.wx_(rt, 1, "(u64)" + ld)

    def store_from(self, rt, nb, v, a):
        if v:
            if nb == 16:
                return "ST64(%s, %s); ST64(%s + 8, %s);" % (a, self.VL(rt), a, self.VH(rt))
            return "ST%d(%s, (u%d)%s);" % (nb * 8, a, nb * 8, self.VL(rt))
        return "ST%d(%s, (u%d)%s);" % (nb * 8, a, nb * 8, self.rx(rt, 1))

    # ---- calls and exits ----------------------------------------------------------------------
    def tail(self, target):
        self.callees.add(target)
        return "@FLUSHCALL@ __attribute__((musttail)) return f_%x(c);" % target

    def after_call(self):
        s = "@RELOAD@"
        if self.exc:
            s += " if (__builtin_expect(c->exc != 0, 0)) return;"
        return s

    # ---- lowering ---------------------------------------------------------------------------
    def lower(self, i, pc):
        op = i["op"]
        inr = lambda t: self.start <= t < self.end
        if op in ("adrp", "adr"):
            return self.wx_(i["rd"], 1, "0x%xULL" % i["val"])
        if op == "addsub_imm":
            sf = i["sf"]
            a = self.rx(i["rn"], sf, sp_ok=True)
            return self.addsub(i["rd"], a, "0x%xULL" % i["imm"], sf, i["sub"], i["s"], sp_dst=not i["s"])
        if op == "addsub_reg":
            sf = i["sf"]
            return self.addsub(i["rd"], self.rx(i["rn"], sf), self.shifted(self.rx(i["rm"], sf), i["shift"], i["amt"], sf),
                               sf, i["sub"], i["s"], sp_dst=False)
        if op == "addsub_ext":
            sf = i["sf"]
            return self.addsub(i["rd"], self.rx(i["rn"], sf, sp_ok=True),
                               self.extend(self.rx(i["rm"], 1), i["option"], i["amt"]), sf, i["sub"], i["s"],
                               sp_dst=not i["s"])
        if op in ("log_imm", "log_reg"):
            sf, T = i["sf"], self.T(i["sf"])
            a = self.rx(i["rn"], sf)
            if op == "log_imm":
                bexpr, opc = "0x%xULL" % i["imm"], i["opc"]
            else:
                bexpr = self.shifted(self.rx(i["rm"], sf), i["shift"], i["amt"], sf)
                if i["neg"]:
                    bexpr = "(%s)~%s" % (T, bexpr)
                opc = i["opc"]
            e = "((%s)%s %s (%s)%s)" % (T, a, "&|^&"[opc], T, bexpr)
            if opc == 3:
                top = 63 if sf else 31
                return "{ %s r_ = %s; %s %s }" % (T, e, self.set_flags("r_>>%d" % top, "r_==0", "0", "0"),
                                                 self.wx_(i["rd"], sf, "r_"))
            return self.wx_(i["rd"], sf, e, sp_ok=(op == "log_imm"))
        if op == "movw":
            sf, sh, imm = i["sf"], i["shift"], i["imm"]
            if i["opc"] == 2:
                return self.wx_(i["rd"], sf, "0x%xULL" % (imm << sh))
            if i["opc"] == 0:
                v = ~(imm << sh) & ((1 << 64) - 1 if sf else 0xFFFFFFFF)
                return self.wx_(i["rd"], sf, "0x%xULL" % v)
            return self.wx_(i["rd"], sf, "((%s & ~0x%xULL) | 0x%xULL)" % (self.rx(i["rd"], sf), 0xFFFF << sh, imm << sh))
        if op == "bfm":
            sf, ds = i["sf"], 64 if i["sf"] else 32
            T, S = self.T(sf), "i64" if sf else "i32"
            r, s = i["immr"], i["imms"]
            n = self.rx(i["rn"], sf)
            if s >= r:
                w = s - r + 1
                m = (1 << w) - 1
                field = "(((%s)%s >> %d) & 0x%xULL)" % (T, n, r, m)
                lsb = 0
            else:
                w = s + 1
                m = (1 << w) - 1
                lsb = ds - r
                field = "((%s)%s & 0x%xULL)" % (T, n, m)
            if i["opc"] == 0:
                sh = ds - w
                v = "((%s)((%s)((%s)%s << %d) >> %d) << %d)" % (T, S, T, field, sh, sh, lsb) if sh else \
                    "((%s)%s << %d)" % (T, field, lsb)
            elif i["opc"] == 2:
                v = "((%s)%s << %d)" % (T, field, lsb)
            else:
                d = self.rx(i["rd"], sf)
                v = "(((%s)%s & ~(0x%xULL << %d)) | ((%s)%s << %d))" % (T, d, m, lsb, T, field, lsb)
            return self.wx_(i["rd"], sf, v)
        if op == "extr":
            sf, ds, T = i["sf"], 64 if i["sf"] else 32, self.T(i["sf"])
            n, m, l = self.rx(i["rn"], sf), self.rx(i["rm"], sf), i["lsb"]
            if l == 0:
                return self.wx_(i["rd"], sf, m)
            return self.wx_(i["rd"], sf, "(((%s)%s >> %d) | ((%s)%s << %d))" % (T, m, l, T, n, ds - l))
        if op == "adc":
            sf, T = i["sf"], self.T(i["sf"])
            W = "u128" if sf else "u64"
            top = 63 if sf else 31
            b2 = "(%s)~(%s)%s" % (T, T, self.rx(i["rm"], sf)) if i["sub"] else "(%s)%s" % (T, self.rx(i["rm"], sf))
            body = "%s a_ = %s; %s b_ = %s; %s w_ = (%s)a_ + b_ + %s; %s r_ = (%s)w_;" % (
                T, self.rx(i["rn"], sf), T, b2, W, W, self.F("c"), T, T)
            if i["s"]:
                body += self.set_flags("r_>>%d" % top, "r_==0", "(u8)(w_>>%d)" % (top + 1),
                                       "((a_^r_)&(b_^r_))>>%d" % top)
            return "{ %s %s }" % (body, self.wx_(i["rd"], sf, "r_"))
        if op == "ccmp":
            sf, T = i["sf"], self.T(i["sf"])
            top = 63 if sf else 31
            bexpr = "0x%xULL" % i["rm"] if i["isimm"] else self.rx(i["rm"], sf)
            nz = i["nzcv"]
            if i["sub"]:
                fl = self.set_flags("r_>>%d" % top, "r_==0", "a_>=b_", "((a_^b_)&(a_^r_))>>%d" % top)
                r = "a_ - b_"
            else:
                fl = self.set_flags("r_>>%d" % top, "r_==0", "r_<a_", "((a_^r_)&(b_^r_))>>%d" % top)
                r = "a_ + b_"
            imm = self.set_flags(str(nz >> 3), str((nz >> 2) & 1), str((nz >> 1) & 1), str(nz & 1))
            return "if (%s) { %s a_ = %s; %s b_ = %s; %s r_ = %s; %s } else { %s }" % (
                self.cond(i["cond"]), T, self.rx(i["rn"], sf), T, bexpr, T, r, fl, imm)
        if op == "csel":
            sf, T = i["sf"], self.T(i["sf"])
            n, m = "(%s)%s" % (T, self.rx(i["rn"], sf)), "(%s)%s" % (T, self.rx(i["rm"], sf))
            alt = [m, "(%s)(%s + 1)" % (T, m), "(%s)~%s" % (T, m), "(%s)(0 - %s)" % (T, m)][i["kind"]]
            return self.wx_(i["rd"], sf, "(%s ? %s : %s)" % (self.cond(i["cond"]), n, alt))
        if op == "dp2":
            sf, T, S = i["sf"], self.T(i["sf"]), "i64" if i["sf"] else "i32"
            ds = 64 if sf else 32
            a, b2, k = "(%s)%s" % (T, self.rx(i["rn"], sf)), "(%s)%s" % (T, self.rx(i["rm"], sf)), i["kind"]
            e = {"udiv": "udiv%d(%s, %s)" % (ds, a, b2), "sdiv": "sdiv%d(%s, %s)" % (ds, a, b2),
                 "lslv": "(%s)(%s << (%s & %d))" % (T, a, b2, ds - 1),
                 "lsrv": "(%s)(%s >> (%s & %d))" % (T, a, b2, ds - 1),
                 "asrv": "(%s)((%s)%s >> (%s & %d))" % (T, S, a, b2, ds - 1),
                 "rorv": "ror%d(%s, (%s & %d))" % (ds, a, b2, ds - 1)}[k]
            return self.wx_(i["rd"], sf, e)
        if op == "dp1":
            sf, ds = i["sf"], 64 if i["sf"] else 32
            n = "(%s)%s" % (self.T(sf), self.rx(i["rn"], sf))
            e = {"rbit": "__builtin_bitreverse%d(%s)" % (ds, n), "rev": "__builtin_bswap%d(%s)" % (ds, n),
                 "rev16": "rev16_%d(%s)" % (ds, n), "rev32": "rev32_64(%s)" % n,
                 "clz": "clz%d(%s)" % (ds, n), "cls": "cls%d(%s)" % (ds, n)}[i["kind"]]
            return self.wx_(i["rd"], sf, e)
        if op == "dp3":
            sf, T, k = i["sf"], self.T(i["sf"]), i["kind"]
            n, m, a = self.rx(i["rn"], sf), self.rx(i["rm"], sf), self.rx(i["ra"], sf)
            if k in ("madd", "msub"):
                e = "(%s)((%s)%s %s (%s)%s * (%s)%s)" % (T, T, a, "+" if k == "madd" else "-", T, n, T, m)
            elif k in ("smaddl", "smsubl"):
                e = "(u64)((i64)%s %s (i64)(i32)%s * (i64)(i32)%s)" % (
                    self.rx(i["ra"], 1), "+" if k == "smaddl" else "-", self.rx(i["rn"], 1), self.rx(i["rm"], 1))
            elif k in ("umaddl", "umsubl"):
                e = "(u64)(%s %s (u64)(u32)%s * (u64)(u32)%s)" % (
                    self.rx(i["ra"], 1), "+" if k == "umaddl" else "-", self.rx(i["rn"], 1), self.rx(i["rm"], 1))
            elif k == "smulh":
                e = "(u64)(((i128)(i64)%s * (i128)(i64)%s) >> 64)" % (n, m)
            else:
                e = "(u64)(((u128)%s * (u128)%s) >> 64)" % (n, m)
            return self.wx_(i["rd"], sf, e)
        if op in ("ldr", "str"):
            pre, wb = self.addr(i)
            if op == "ldr":
                body = self.load_to(i["rt"], i["nbytes"], i["v"], i["signed"], i["dst64"], "a_")
            else:
                body = self.store_from(i["rt"], i["nbytes"], i["v"], "a_")
            return "{ %s %s %s }" % (pre, body, wb)
        if op in ("ldp", "stp"):
            pre, wb = self.addr(dict(i, rm=None))
            nb = i["nbytes"]
            if op == "ldp":
                body = self.load_to(i["rt"], nb, i["v"], i["signed"], 1, "a_") + " " + \
                    self.load_to(i["rt2"], nb, i["v"], i["signed"], 1, "a_ + %d" % nb)
                if not i["v"] and nb == 4 and not i["signed"]:
                    body = self.load_to(i["rt"], 4, 0, 0, 1, "a_") + " " + self.load_to(i["rt2"], 4, 0, 0, 1, "a_ + 4")
            else:
                body = self.store_from(i["rt"], nb, i["v"], "a_") + " " + self.store_from(i["rt2"], nb, i["v"],
                                                                                         "a_ + %d" % nb)
            return "{ %s %s %s }" % (pre, body, wb)
        if op == "ldr_lit":
            return self.load_to(i["rt"], i["nbytes"], i["v"], i["signed"], i.get("dst64", 1), "0x%xULL" % i["addr"])
        if op in ("ldxr", "stxr", "ldar", "stlr"):
            nb = 1 << i["size"]
            a = self.rx(i["rn"], 1, sp_ok=True)
            if op == "ldxr":
                ld = "LDA%d(a_)" % (nb * 8) if i["ordered"] else "LD%d(a_)" % (nb * 8)
                return "{ u64 a_ = %s; u64 v_ = (u64)%s; c->excl_addr = a_; c->excl_val = v_; %s }" % (
                    a, ld, self.wx_(i["rt"], 1, "v_"))
            if op == "stxr":
                return "{ u64 a_ = %s; %s }" % (a, self.wx_(i["rs"], 0, "rt_stxr%d(c, a_, (u%d)%s)" % (
                    nb * 8, nb * 8, self.rx(i["rt"], 1))))
            if op == "ldar":
                return "{ u64 a_ = %s; %s }" % (a, self.wx_(i["rt"], 1, "(u64)LDA%d(a_)" % (nb * 8)))
            return "{ u64 a_ = %s; STL%d(a_, (u%d)%s); }" % (a, nb * 8, nb * 8, self.rx(i["rt"], 1))
        if op == "dmb":
            # ISHLD and ISHST order nothing x86 does not already order: compiler barrier only.
            if i["crm"] in (0b1001, 0b1010, 0b0101, 0b0110, 0b1101, 0b1110, 0b0001, 0b0010):
                return "__atomic_signal_fence(__ATOMIC_SEQ_CST);"
            return "__atomic_thread_fence(__ATOMIC_SEQ_CST);"
        if op == "clrex":
            return "c->excl_addr = ~0ULL;"
        if op == "nop":
            return ""
        if op == "mrs":
            sr = i["sysreg"]
            if sr == 0xDA10:
                return self.wx_(i["rt"], 1, "((u64)%s<<31|(u64)%s<<30|(u64)%s<<29|(u64)%s<<28)" % (
                    self.F("n"), self.F("z"), self.F("c"), self.F("v")))
            if sr == 0xDF02:
                return self.wx_(i["rt"], 1, "rt_cntvct()")
            if sr == 0xDF00:
                return self.wx_(i["rt"], 1, "19200000ULL")
            if sr == 0xD807:
                return self.wx_(i["rt"], 1, "4ULL")
            if sr == 0xD801:
                return self.wx_(i["rt"], 1, "0x8444c004ULL")
            if sr in SYSREG:
                return self.wx_(i["rt"], 1, "c->" + SYSREG[sr])
            raise Unsupported("mrs %x" % sr)
        if op == "msr":
            sr = i["sysreg"]
            v = self.rx(i["rt"], 1)
            if sr == 0xDA10:
                return "{ u64 t_ = %s; %s }" % (v, self.set_flags("(t_>>31)&1", "(t_>>30)&1", "(t_>>29)&1",
                                                                   "(t_>>28)&1"))
            if sr in SYSREG and sr != 0xDE83:
                return "c->%s = %s;" % (SYSREG[sr], v)
            raise Unsupported("msr %x" % sr)
        # ---- control flow ---------------------------------------------------------------------
        if op == "b":
            t = i["target"]
            if inr(t):
                return self.backedge(t, pc) + "goto L_%x;" % t
            return self.tail(t)
        if op in ("bcond", "cbz", "tbz"):
            t = i["target"]
            if op == "bcond":
                cnd = self.cond(i["cond"])
            elif op == "cbz":
                cnd = "(%s %s 0)" % (self.rx(i["rt"], i["sf"]), "!=" if i["nz"] else "==")
            else:
                cnd = "(((%s >> %d) & 1) %s 0)" % (self.rx(i["rt"], 1), i["bit"], "!=" if i["nz"] else "==")
            if inr(t):
                return "if (%s) { %sgoto L_%x; }" % (cnd, self.backedge(t, pc), t)
            return "if (%s) { %s }" % (cnd, self.tail(t))
        if op == "bl":
            t = i["target"]
            self.callees.add(t)
            return "%s = 0x%xULL; @FLUSHCALL@ f_%x(c); %s" % (self.X(30, True), pc + 4, t, self.after_call())
        if op == "blr":
            self.uses_dispatch = True
            return "{ u64 t_ = %s; %s = 0x%xULL; @FLUSHCALL@ rt_icall(c, t_); %s }" % (
                self.rx(i["rn"], 1), self.X(30, True), pc + 4, self.after_call())
        if op == "br":
            self.uses_dispatch = True
            return "c->pc = %s; @FLUSH@ __attribute__((musttail)) return rt_dispatch(c);" % self.rx(i["rn"], 1)
        if op == "ret":
            return "@FLUSHRET@ return;"
        if op == "svc":
            return "@FLUSH@ rt_svc(c, %d); @RELOADALL@" % i["imm"]
        if op == "brk":
            return "@FLUSH@ rt_brk(c, %d); return;" % i["imm"]
        # ---- FP ---------------------------------------------------------------------------------
        return self.lower_fp(i)

    def backedge(self, t, pc):
        if self.poll and t <= pc:
            return "if (__builtin_expect(g_rt_poll, 0)) { @FLUSH@ rt_safepoint(c); @RELOADALL@ } "
        return ""

    def fr(self, n, dbl):
        return ("u2d(%s)" if dbl else "u2f(%s)") % self.VL(n)

    def fw(self, n, dbl, expr):
        if dbl:
            return "%s = d2u(%s); %s = 0;" % (self.VL(n, 1), expr, self.VH(n, 1))
        return "%s = (u64)f2u(%s); %s = 0;" % (self.VL(n, 1), expr, self.VH(n, 1))

    def lower_fp(self, i):
        op = i["op"]
        if op == "vmov":
            return "%s = %s; %s = %s;" % (self.VL(i["rd"], 1), self.VL(i["rn"]), self.VH(i["rd"], 1),
                                          self.VH(i["rn"]) if i["q"] else "0")
        if op == "vzero":
            return "%s = 0; %s = 0;" % (self.VL(i["rd"], 1), self.VH(i["rd"], 1))
        dbl = i.get("dbl", 0)
        sfx = "d" if dbl else "f"
        if op == "fp1":
            k, n, d = i["kind"], i["rn"], i["rd"]
            if k == "fmov":
                m = "" if dbl else " & 0xffffffffULL"
                return "%s = %s%s; %s = 0;" % (self.VL(d, 1), self.VL(n), m, self.VH(d, 1))
            if k == "fcvt_s":
                return self.fw(d, 0, "(float)" + self.fr(n, dbl))
            if k == "fcvt_d":
                return self.fw(d, 1, "(double)" + self.fr(n, dbl))
            fs = "" if dbl else "f"
            e = {"fabs": "__builtin_fabs" + fs + "(%s)", "fneg": "(-%s)", "fsqrt": "__builtin_sqrt" + fs + "(%s)",
                 "frintn": "__builtin_roundeven" + fs + "(%s)", "frintp": "__builtin_ceil" + fs + "(%s)",
                 "frintm": "__builtin_floor" + fs + "(%s)", "frintz": "__builtin_trunc" + fs + "(%s)",
                 "frinta": "__builtin_round" + fs + "(%s)", "frintx": "__builtin_rint" + fs + "(%s)",
                 "frinti": "__builtin_nearbyint" + fs + "(%s)"}[k] % self.fr(n, dbl)
            return self.fw(d, dbl, e)
        if op == "fp2":
            a, b2, k = self.fr(i["rn"], dbl), self.fr(i["rm"], dbl), i["kind"]
            e = {"fmul": "(%s * %s)", "fdiv": "(%s / %s)", "fadd": "(%s + %s)", "fsub": "(%s - %s)",
                 "fnmul": "(-(%s * %s))", "fmax": "fmax_" + sfx + "(%s, %s)", "fmin": "fmin_" + sfx + "(%s, %s)",
                 "fmaxnm": "fmaxnm_" + sfx + "(%s, %s)", "fminnm": "fminnm_" + sfx + "(%s, %s)"}[k] % (a, b2)
            return self.fw(i["rd"], dbl, e)
        if op == "fp3":
            n, m, a, k = self.fr(i["rn"], dbl), self.fr(i["rm"], dbl), self.fr(i["ra"], dbl), i["kind"]
            f = "__builtin_fma" + ("" if dbl else "f")
            e = {"fmadd": "%s(%s, %s, %s)" % (f, n, m, a), "fmsub": "%s(-%s, %s, %s)" % (f, n, m, a),
                 "fnmadd": "%s(-%s, %s, -%s)" % (f, n, m, a), "fnmsub": "%s(%s, %s, -%s)" % (f, n, m, a)}[k]
            return self.fw(i["rd"], dbl, e)
        if op == "fcmp":
            b2 = "0.0" if i["zero"] else self.fr(i["rm"], dbl)
            return "{ u32 f_ = fcmp_%s(%s, %s); %s }" % (sfx, self.fr(i["rn"], dbl), b2, self.set_flags(
                "(f_>>3)&1", "(f_>>2)&1", "(f_>>1)&1", "f_&1"))
        if op == "fcsel":
            return "%s = %s ? %s : %s; %s = 0;" % (self.VL(i["rd"], 1), self.cond(i["cond"]), self.VL(i["rn"]),
                                                   self.VL(i["rm"]), self.VH(i["rd"], 1))
        if op == "fmov_imm":
            return "%s = 0x%xULL; %s = 0;" % (self.VL(i["rd"], 1), vfp_expand(i["imm8"], dbl), self.VH(i["rd"], 1))
        if op == "fmov_gen":
            if i["to_gen"]:
                return self.wx_(i["rd"], i["sf"], self.VL(i["rn"]) if i["sf"] else "(u32)" + self.VL(i["rn"]))
            v = self.rx(i["rn"], i["sf"])
            return "%s = (u64)%s; %s = 0;" % (self.VL(i["rd"], 1), v, self.VH(i["rd"], 1))
        if op == "cvtf":
            sf = i["sf"]
            src = self.rx(i["rn"], sf)
            cast = ("(u64)" if sf else "(u32)") if i["unsigned"] else ("(i64)" if sf else "(i32)")
            return self.fw(i["rd"], dbl, "(%s)%s%s" % ("double" if dbl else "float", cast, src))
        if op == "fcvti":
            sf = i["sf"]
            fn = "cvt%s%d_%s" % ("u" if i["unsigned"] else "s", 64 if sf else 32, sfx)
            return self.wx_(i["rd"], sf, "%s(%s, '%s')" % (fn, self.fr(i["rn"], dbl), i["rnd"]))
        raise Unsupported(op)

    # ---- function -----------------------------------------------------------------------------
    def emit(self, insns):
        targets = set()
        for pc, i in insns:
            t = i.get("target")
            if t is not None and self.start <= t < self.end and i["op"] != "bl":
                targets.add(t)
        body = []
        last = None
        for pc, i in insns:
            if pc in targets:
                body.append("L_%x:;" % pc)
            s = self.lower(i, pc)
            if s:
                body.append("  " + s)
            last = i["op"]
        if last not in ("b", "br", "ret", "brk"):
            body.append("  " + self.tail(self.end))
        text = "\n".join(body)
        if self.model == "local":
            def nm(k):
                return "sp" if k == 32 else ("f" + "nzcv"[k - 40] if k >= 40 else "x%d" % k)

            def cf(k):
                return "c->sp" if k == 32 else ("c->f" + "nzcv"[k - 40] if k >= 40 else "c->x[%d]" % k)

            def vn(k):
                return ("v%dh" % (k - 32), "c->v[%d].h" % (k - 32)) if k >= 32 else ("v%dl" % k, "c->v[%d].l" % k)

            def mkflush(xs, vs):
                return " ".join("%s = %s;" % (cf(k), nm(k)) for k in sorted(self.wx) if xs is None or k in xs) + \
                    " " + " ".join("%s = %s;" % (vn(k)[1], vn(k)[0]) for k in sorted(self.wv) if vs is None or k in vs)
            flush = mkflush(None, None)
            if self.abi_flush:
                # AAPCS64: across a call only arguments, x8, x16-x18, callee-saved, fp, lr and sp can be read;
                # at a return only results and callee-saved registers.
                flush_call = mkflush(set(range(0, 9)) | set(range(16, 31)) | {32},
                                     set(range(0, 16)) | set(range(32, 40)))
                flush_ret = mkflush({0, 1} | set(range(19, 30)) | {32}, set(range(0, 16)) | set(range(32, 40)))
            else:
                flush_call = flush_ret = flush
            reload_all = " ".join("%s = %s;" % (nm(k), cf(k)) for k in sorted(self.ux)) + " " + \
                " ".join("%s = %s;" % vn(k) for k in sorted(self.uv))
            # AAPCS64: x19-x29, sp and the low 64 bits of v8-v15 survive calls.
            rx = [k for k in sorted(self.ux) if k in CALLER_SAVED_X or k >= 40]
            rv = [k for k in sorted(self.uv) if not (8 <= k <= 15)]
            reload = " ".join("%s = %s;" % (nm(k), cf(k)) for k in rx) + " " + " ".join("%s = %s;" % vn(k) for k in rv)
            text = text.replace("@FLUSHCALL@", flush_call).replace("@FLUSHRET@", flush_ret).replace("@FLUSH@", flush).replace("@RELOADALL@", reload_all).replace("@RELOAD@", reload)
            decl = []
            for k in sorted(self.ux):
                decl.append("%s %s = %s;" % ("u8" if k >= 40 else "u64", nm(k), cf(k)))
            for k in sorted(self.uv):
                decl.append("u64 %s = %s;" % vn(k))
            head = "  " + " ".join(decl)
        else:
            text = text.replace("@FLUSHCALL@", "").replace("@FLUSHRET@", "").replace("@FLUSH@", "").replace("@RELOADALL@", "").replace("@RELOAD@", "")
            head = ""
        return "void f_%x(Ctx *restrict c) {\n%s\n%s\n}\n" % (self.start, head, text)


def words_of(blob, off, size):
    return struct.unpack_from("<%dI" % (size // 4), blob, off)
