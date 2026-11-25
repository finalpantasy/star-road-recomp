"""Maxwell to IR lifter (our own).

lift(program) -> (ir.Function, Info). Raises Unsupported with a reason for anything it does not
handle; the batch records the reason instead of guessing.
"""
import os
import struct

import decoder
from ir import Op, Function


class Unsupported(Exception):
    pass


RZ = 255
PT = 7
F32_ONE = 0x3F800000

SIDE_EFFECT = {'ast', 'stl', 'stg', 'sts', 'kil', 'exit', 'bra', 'sync', 'brk', 'cont', 'out', 'red', 'atom',
               'atoms', 'sust', 'bar', 'membar', 'brx', 'ret', 'cal'}
NOP_OPS = {'nop', 'depbar', 'membar', 'votevtg', 'cctl', 'pbk', 'ssy', 'pcnt'}

TEX_DIMS = {0: ('1d', False), 1: ('1d', True), 2: ('2d', False), 3: ('2d', True), 4: ('3d', False),
            5: ('3d', True), 6: ('cube', False), 7: ('cube', True)}
DIM_COORDS = {'1d': 1, '2d': 2, '3d': 3, 'cube': 3}


class Program:
    """One NVN stage program: stage name, code bytes, SPH words, c1 constant bytes."""

    def __init__(self, stage, code, sph, consts, key=''):
        self.stage = stage
        self.code = code
        self.sph = struct.unpack('<20I', sph) if sph else None
        self.consts = consts
        self.key = key


class Info:
    def __init__(self):
        self.textures = {}      # key -> dict
        self.inputs = {}        # location -> set of comps; plus interp
        self.input_interp = {}  # location -> mode
        self.outputs = {}       # location -> set comps
        self.builtins_in = set()
        self.builtins_out = set()
        self.cbufs = {}         # slot -> max byte (or 0x10000 for indexed)
        self.local_words = 0
        self.uses_quad = False
        self.uses_subgroup = False
        self.demotes = False
        self.rt_mask = 0
        self.writes_depth = False
        self.ssbos = {}
        self.insns = 0
        self.clip_written = 0


def next_addr(a):
    a += 8
    return a + 8 if a % 32 == 0 else a


def f32_bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


# Oracle-compat switches (docs/SHADER_CACHE.md): each one reproduces a reference translator's
# choice where ours is a deliberate precision or semantics difference, so a differential test can
# separate explained divergences from bugs. Never set for the shipped cache.
COMPAT = set(x for x in os.environ.get('SXL_COMPAT', '').split(',') if x)


class Lifter:
    def __init__(self, prog):
        self.p = prog
        self.fn = Function()
        self.info = Info()
        self.cur = None
        self.cache = {}       # reg -> Op (current block)
        self.pcache = {}
        self.n = 0
        self.pcond = None
        self.vtg_cc = False

    # ---------- emission helpers ----------
    def emit(self, op, ty, *args, attr=None):
        o = Op(op, ty, args, attr)
        self.n += 1
        o.n = self.n
        self.cur.ops.append(o)
        return o

    def const(self, v, ty='u32'):
        return self.emit('const', ty, attr=v & 0xFFFFFFFF if ty != 'bool' else bool(v))

    def fconst(self, f):
        return self.const(f32_bits(f), 'f32')

    def set_block(self, b):
        self.cur = b
        self.cache = {}
        self.pcache = {}

    def rget(self, r):
        if r == RZ:
            c = self.const(0)
            c.sym = ('imm', 0)
            return c
        v = self.cache.get(r)
        if v is None:
            v = self.emit('rget', 'u32', attr=r)
            self.cache[r] = v
        return v

    def rset(self, r, v):
        if r == RZ:
            return
        if v.ty == 'f32':
            v = self.bc_u(v)
        assert v.ty == 'u32', v
        if self.pcond is not None:
            old = self.rget(r)
            sym = v.sym
            v = self.emit('select', 'u32', self.pcond, v, old)
            # handle tracing: a predicated MOV/LOP of a handle is read under the same predicate
            v.sym = sym
        self.emit('rset', 'void', v, attr=r)
        self.cache[r] = v

    def pget(self, p, neg=False):
        if p == PT:
            v = self.const(1, 'bool')
        else:
            v = self.pcache.get(p)
            if v is None:
                v = self.emit('pget', 'bool', attr=p)
                self.pcache[p] = v
        return self.emit('lnot', 'bool', v) if neg else v

    def pset(self, p, v):
        if p == PT:
            return
        if self.pcond is not None:
            old = self.pget(p)
            v = self.emit('select', 'bool', self.pcond, v, old)
        self.emit('pset', 'void', v, attr=p)
        self.pcache[p] = v

    def bc_f(self, v):
        if v.ty == 'f32':
            return v
        if v.op == 'bitcast_u':
            return v.args[0]
        if v.op == 'const':
            return self.const(v.attr, 'f32')
        return self.emit('bitcast_f', 'f32', v)

    def bc_u(self, v):
        if v.ty == 'u32':
            return v
        if v.op == 'bitcast_f':
            return v.args[0]
        if v.op == 'const':
            return self.const(v.attr)
        return self.emit('bitcast_u', 'u32', v)

    def fr(self, r):
        return self.bc_f(self.rget(r))

    # ---------- operand B helpers ----------
    def cbuf_const(self, slot, off):
        """Constant buffer word at a constant byte offset."""
        if slot == 1:
            o = off
            if 0 <= o and o + 4 <= len(self.p.consts):
                v = self.const(struct.unpack_from('<I', self.p.consts, o)[0])
            else:
                v = self.const(0)
            v.sym = ('imm', v.attr)
            return v
        self.info.cbufs[slot] = max(self.info.cbufs.get(slot, 0), off + 4)
        v = self.emit('cbuf', 'u32', self.const(off), attr=slot)
        v.sym = ('cb', slot, off)
        return v

    def opb(self, i, kind='u'):
        """b operand for R/I/C forms. RC callers must take b from Rc and c from the cbuf themselves."""
        f = i.form
        if f == 'R':
            v = self.rget(i.rb)
        elif f == 'I':
            if kind == 'f':
                v = self.const(i.imm20f)
            else:
                v = self.const(i.imm20i)
            v.sym = ('imm', v.attr)
        elif f in ('C', 'RC'):
            v = self.cbuf_const(i.cb_slot, i.cb_off)
        elif f == 'I32':
            v = self.const(i.imm32)
            v.sym = ('imm', v.attr)
        else:
            raise Unsupported('operand form %s for %s' % (f, i.name))
        return v

    def opc(self, i):
        """c operand: register Rc (bits 39-46). Not valid for RC forms, where c is the cbuf."""
        return self.rget(i.rc)

    def fneg_abs(self, v, neg, ab):
        v = self.bc_f(v)
        if ab:
            v = self.emit('fabs', 'f32', v)
        if neg:
            v = self.emit('fneg', 'f32', v)
        return v

    def sat(self, v):
        # Maxwell .SAT: NaN -> 0, clamp to [0, 1]
        z = self.fconst(0.0)
        one = self.fconst(1.0)
        gt0 = self.emit('fcmp', 'bool', v, z, attr=4)       # ordered >
        lt1 = self.emit('fcmp', 'bool', v, one, attr=1)     # ordered <
        inner = self.emit('select', 'f32', lt1, v, one)
        return self.emit('select', 'f32', gt0, inner, z)

    def fmz_mul(self, a, b, mode):
        r = self.emit('fmul', 'f32', a, b)
        if mode in (1, 2):   # FMZ / FMZ+FTZ: 0 * anything = 0
            z = self.fconst(0.0)
            az = self.emit('fcmp', 'bool', a, z, attr=2)
            bz = self.emit('fcmp', 'bool', b, z, attr=2)
            anyz = self.emit('lor', 'bool', az, bz)
            r = self.emit('select', 'f32', anyz, z, r)
        return r

    # ---------- comparisons ----------
    def fcomp(self, a, b, code):
        if code == 0:
            return self.const(0, 'bool')
        if code == 15:
            return self.const(1, 'bool')
        return self.emit('fcmp', 'bool', a, b, attr=code)

    def icomp(self, a, b, code, signed):
        names = {1: 'lt', 2: 'eq', 3: 'le', 4: 'gt', 5: 'ne', 6: 'ge'}
        if code == 0:
            return self.const(0, 'bool')
        if code == 7:
            return self.const(1, 'bool')
        k = names[code]
        if k not in ('eq', 'ne'):
            k = ('s' if signed else 'u') + k
        return self.emit('icmp', 'bool', a, b, attr=k)

    def bop(self, a, b, op):
        if op == 0:
            return self.emit('land', 'bool', a, b)
        if op == 1:
            return self.emit('lor', 'bool', a, b)
        if op == 2:
            return self.emit('lxor', 'bool', a, b)
        raise Unsupported('bool op 3')

    def setp_out(self, i, r, srcpred, srcpred_inv, bop):
        sp = self.pget(srcpred, srcpred_inv)
        d0 = self.bop(r, sp, bop)
        d1 = self.bop(self.emit('lnot', 'bool', r), sp, bop)
        self.pset(i.f(3, 3), d0)
        self.pset(i.f(0, 3), d1)

    # ---------- main ----------
    def run(self):
        p = self.p
        insts = decoder.decode_code(p.code)
        self.info.insns = len(insts)
        self.vtg_cc = any(i.name == 'votevtg' for i in insts)
        # .CC writers we model for Z/N tests: FSET (bit 47). Any other ALU op with a CC write in a
        # program that tests Z/N leaves the flags unknown, so such programs stay unsupported.
        self.zn_cc = (not self.vtg_cc and any(i.name == 'fset' and i.b(47) for i in insts) and
                      any(i.name in ('bra', 'exit', 'sync', 'brk', 'cont', 'kil') and i.f(0, 5) not in (0, 0xF)
                          for i in insts))
        if not insts:
            raise Unsupported('empty program')
        by_addr = {i.addr: k for k, i in enumerate(insts)}
        for i in insts:
            if i.name is None:
                raise Unsupported('unknown opcode %016x at %#x' % (i.w, i.addr))
        # leaders
        leaders = {insts[0].addr}
        for k, i in enumerate(insts):
            nx = insts[k + 1].addr if k + 1 < len(insts) else None
            if i.name in ('bra', 'ssy', 'pbk', 'pcnt'):
                t = self.branch_target(i)
                if t not in by_addr:
                    if i.name == 'bra' and t == i.addr:
                        pass
                    else:
                        raise Unsupported('%s target %#x outside code' % (i.name, t))
                leaders.add(t)
            if i.name in ('bra', 'exit', 'sync', 'brk', 'cont', 'kil', 'brx', 'ret') and nx is not None:
                leaders.add(nx)
        self.insts = insts
        self.by_addr = by_addr
        starts = sorted(leaders)
        self.mblocks = {}
        for k, s in enumerate(starts):
            e = starts[k + 1] if k + 1 < len(starts) else insts[-1].addr + 8
            self.mblocks[s] = [x for x in insts[by_addr[s]:] if x.addr < e]
        # IR block per Maxwell block start, created lazily per (addr, stack)
        self.irb = {}
        self.work = []
        entry = self.get_block(insts[0].addr, ())
        self.fn.entry = entry
        while self.work:
            addr, stack, blk = self.work.pop()
            self.lift_block(addr, stack, blk)
        self.fn.compute_preds()
        if 'half_fold' in COMPAT:
            self.fold_halves_across_blocks()
        self.resolve_textures()
        return self.fn, self.info

    def fold_halves_across_blocks(self):
        """Oracle mirror only (half_fold): Ryubing's SSA folds UnpackHalf2x16(PackHalf2x16(x)) into x
        also when the pack sits in a block that dominates the unpack without a phi. Our register cache
        is per block, so carry the unrounded lanes into a block when every path
        brings in the same packed value (no phi in SSA terms)."""
        top, bot = object(), object()
        cache = {}

        def in_vals(reg):
            """Per block, the one value every path brings in for reg (no phi in SSA), else None.
            Optimistic fixpoint, so a loop that leaves reg alone does not block it (Ryubing's SSA
            has no phi there either)."""
            if reg in cache:
                return cache[reg]
            blocks = self.fn.blocks
            gen = {}
            for blk in blocks:
                g = None
                for o in reversed(blk.ops):
                    if o.op == 'rset' and o.attr == reg:
                        g = o.args[0]
                        break
                gen[blk.id] = g
            ins = {blk.id: top for blk in blocks}
            changed = True
            while changed:
                changed = False
                for blk in blocks:
                    if blk is self.fn.entry or not blk.preds:
                        new = bot       # the entry value of a register is undefined, not a pack
                    else:
                        new = top
                        for pb in blk.preds:
                            v = gen[pb.id] if gen[pb.id] is not None else ins[pb.id]
                            if v is top:
                                continue
                            if new is top:
                                new = v
                            elif new is not v:
                                new = bot
                                break
                    if new is not ins[blk.id]:
                        ins[blk.id] = new
                        changed = True
            res = {k: (None if v is top or v is bot else v) for k, v in ins.items()}
            cache[reg] = res
            return res

        repl = {}
        for blk in self.fn.blocks:
            if not blk.preds:
                continue
            for o in blk.ops:
                if o.op != 'h2f_lo':
                    continue
                src, lane = o.args[0], 0
                if src.op == 'shr' and src.args[1].op == 'const' and src.args[1].attr == 16:
                    src, lane = src.args[0], 1
                if src.op != 'rget' or src not in blk.ops:
                    continue
                v = in_vals(src.attr)[blk.id]
                h = getattr(v, 'halves', None)
                if h is not None:
                    repl[id(o)] = h[lane]
        if not repl:
            return
        for k in list(repl):        # a folded lane can itself be a lane read folded in another block
            t, seen = repl[k], {k}
            while id(t) in repl and id(t) not in seen:
                seen.add(id(t))
                t = repl[id(t)]
            repl[k] = t
        undo = []           # the emitter reverts these when it falls back to the dispatcher form
        for blk in self.fn.blocks:
            for o in blk.ops:
                if any(id(a) in repl for a in o.args if isinstance(a, Op)):
                    undo.append((o, list(o.args)))
                    o.args = [repl.get(id(a), a) if isinstance(a, Op) else a for a in o.args]
        self.fn.fold_undo = undo

    def branch_target(self, i):
        t = i.addr + 8 + decoder.sext(i.f(20, 24), 24)
        if t % 32 == 0:
            t += 8     # a target on a scheduling word means the first instruction of that bundle
        return t

    def get_block(self, addr, stack):
        # Reaching the target of the top sync entry by fall-through pops it.
        while stack and stack[-1][1] == addr:
            stack = stack[:-1]
        key = (addr, stack)
        b = self.irb.get(key)
        if b is None:
            if len(self.irb) > 4 * len(self.mblocks) + 64:
                raise Unsupported('sync stack does not converge')
            b = self.fn.new_block(addr)
            self.irb[key] = b
            self.work.append((addr, stack, b))
        return b

    def lift_block(self, addr, stack, blk):
        self.set_block(blk)
        stack = list(stack)
        ins = self.mblocks.get(addr)
        if ins is None:
            raise Unsupported('jump into the middle of a block at %#x' % addr)
        for k, i in enumerate(ins):
            self.pcond = None
            guarded = not (i.pred == PT and not i.pred_neg)
            if i.pred == PT and i.pred_neg:
                continue    # never executes
            name = i.name
            if name in ('ssy', 'pbk', 'pcnt'):
                stack.append((name, self.branch_target(i)))
                continue
            if name in NOP_OPS:
                continue
            last = (k == len(ins) - 1)
            if name in ('bra', 'exit', 'sync', 'brk', 'cont', 'kil', 'brx', 'ret'):
                self.lift_flow(i, guarded, stack, ins, k)
                if name == 'kil':
                    continue
                return
            if guarded and name in SIDE_EFFECT:
                cond = self.pget(i.pred, i.pred_neg)
                then = self.fn.new_block()
                join = self.fn.new_block()
                self.cur.term = ('cbr', cond, then, join)
                self.set_block(then)
                self.lift_inst(i)
                self.cur.term = ('br', join)
                self.set_block(join)
                continue
            if guarded:
                self.pcond = self.pget(i.pred, i.pred_neg)
            self.lift_inst(i)
            self.pcond = None
        # fall through
        nx = next_addr(ins[-1].addr)
        if nx not in self.mblocks:
            raise Unsupported('falls off the end of the code')
        self.cur.term = ('br', self.get_block(nx, tuple(stack)))

    def ccc_cond(self, i):
        c = i.f(0, 5)
        if c == 0xF:
            return None      # always
        if c == 0:
            return False
        # Only VOTE.VTG sets the codes our corpus tests; its early exit is a cull optimization.
        if self.vtg_cc:
            return False
        # Tests that read only Z and N, set by the .CC forms we model (set_zn). C and V are not modeled.
        zn = {2: lambda z, n: self.emit('land', 'bool', self.emit('lnot', 'bool', n), z),    # EQ
              5: lambda z, n: self.emit('lnot', 'bool', z),                                  # NE
              7: lambda z, n: self.emit('lnot', 'bool', self.emit('land', 'bool', n, z)),    # NUM
              8: lambda z, n: self.emit('land', 'bool', n, z),                               # NAN
              10: lambda z, n: z,                                                            # EQU
              13: lambda z, n: self.emit('lor', 'bool', n, self.emit('lnot', 'bool', z)),    # NEU
              18: lambda z, n: self.emit('lnot', 'bool', n),                                 # SFF
              21: lambda z, n: n}                                                            # SFT
        if c in zn and self.zn_cc:
            return zn[c](self.pget('zf'), self.pget('nf'))
        raise Unsupported('condition code test %d on %s at %#x' % (c, i.name, i.addr))

    def set_zn(self, v, fp):
        """Z and N flags from a result, as a .CC write sets them (fp: compare as float)."""
        zero = self.fconst(0.0) if fp else self.const(0)
        if fp:
            z, n = self.fcomp(v, zero, 2), self.fcomp(v, zero, 1)
        else:
            z, n = self.icomp(v, zero, 2, True), self.icomp(v, zero, 1, True)
        self.pset('zf', z)
        self.pset('nf', n)

    def lift_flow(self, i, guarded, stack, ins, k):
        name = i.name
        nx_addr = next_addr(i.addr)
        cc = self.ccc_cond(i) if name in ('bra', 'exit', 'sync', 'brk', 'cont', 'kil') else None
        if cc is False:
            # never taken: fall through
            if name == 'kil':
                return
            self.cur.term = ('br', self.get_block(nx_addr, tuple(stack)))
            return
        cond = self.pget(i.pred, i.pred_neg) if guarded else None
        if cc is not None:
            cond = cc if cond is None else self.emit('land', 'bool', cond, cc)

        def fall():
            return self.get_block(nx_addr, tuple(stack))

        if name == 'kil':
            self.info.demotes = True
            if cond is None:
                self.emit('demote', 'void')
            else:
                then = self.fn.new_block()
                join = self.fn.new_block()
                self.cur.term = ('cbr', cond, then, join)
                self.set_block(then)
                self.emit('demote', 'void')
                self.cur.term = ('br', join)
                self.set_block(join)
            return
        if name == 'bra':
            if i.b(5):
                raise Unsupported('BRA.CA (indirect via constant buffer)')
            t = self.branch_target(i)
            if t == i.addr and cond is None:
                self.cur.term = ('ret',)   # trailing self-loop after the last EXIT
                return
            tb = self.get_block(t, tuple(stack))
        elif name == 'exit':
            tb = self.exit_block()
        elif name in ('sync', 'brk', 'cont'):
            want = {'sync': 'ssy', 'brk': 'pbk', 'cont': 'pcnt'}[name]
            st = list(stack)
            while st and st[-1][0] != want:
                st.pop()
            if not st:
                raise Unsupported('%s with no %s on the stack at %#x' % (name, want, i.addr))
            t = st[-1][1]
            tb = self.get_block(t, tuple(st[:-1]))
        elif name == 'brx':
            raise Unsupported('BRX (indirect branch) at %#x' % i.addr)
        else:
            raise Unsupported(name)
        if cond is None:
            self.cur.term = ('br', tb)
        else:
            self.cur.term = ('cbr', cond, tb, fall())

    def exit_block(self):
        b = getattr(self, '_exit', None)
        if b is not None:
            return b
        b = self.fn.new_block()
        saved = (self.cur, self.cache, self.pcache)
        self.set_block(b)
        if self.p.stage == 'fs':
            self.emit_frag_outputs()
        b.term = ('ret',)
        self.cur, self.cache, self.pcache = saved
        self._exit = b
        return b

    def emit_frag_outputs(self):
        sph = self.p.sph
        targets = sph[18]
        reg = 0
        for rt in range(8):
            m = (targets >> (4 * rt)) & 0xF
            if not m:
                continue
            self.info.rt_mask |= 1 << rt
            for c in range(4):
                if m & (1 << c):
                    v = self.bc_f(self.rget(reg))
                    self.emit('out', 'void', v, attr=('frag', rt, c))
                    self.info.outputs.setdefault(rt, set()).add(c)
                    reg += 1
        if sph[19] & 1:
            reg += 1   # sample mask register (not written by Unity shaders; skipped)
            raise Unsupported('fragment sample mask output')
        if sph[19] & 2:
            self.info.writes_depth = True
            self.emit('out', 'void', self.bc_f(self.rget(reg)), attr=('depth', 0, 0))

    # ---------- instructions ----------
    def lift_inst(self, i):
        fn = getattr(self, 'i_' + i.name, None)
        if fn is None:
            raise Unsupported('opcode %s at %#x' % (i.name, i.addr))
        fn(i)

    # moves
    def i_mov(self, i):
        v = self.opb(i)
        if i.form == 'R' or i.form == 'C':
            if i.f(39, 4) != 0xF:
                raise Unsupported('MOV quad mask')
        self.rset(i.rd, v)

    def i_mov32i(self, i):
        if i.f(12, 4) != 0xF:
            raise Unsupported('MOV32I quad mask')
        v = self.const(i.imm32)
        v.sym = ('imm', v.attr)
        self.rset(i.rd, v)

    def i_sel(self, i):
        a = self.rget(i.ra)
        b = self.opb(i)
        c = self.pget(i.f(39, 3), i.b(42))
        self.rset(i.rd, self.emit('select', 'u32', c, a, b))

    def i_s2r(self, i):
        sr = i.f(20, 8)
        if sr == 0:
            self.info.uses_subgroup = True
            v = self.emit('builtin', 'u32', attr='lane_id')
        elif sr in (30, 31):
            v = self.const(0)          # W-scale factors: only feed the VTG cull vote
        elif sr == 18:
            v = self.const(F32_ONE)    # Y direction
        elif sr == 19:
            v = self.const(0)          # thread kill
        elif sr == 29:
            v = self.const(0 if self.p.stage in ('fs', 'cs') else 3 << 16)
        elif sr == 17:
            v = self.emit('builtin', 'u32', attr='invocation_id')
        elif sr in (33, 34, 35):
            v = self.emit('builtin', 'u32', attr='tid%d' % (sr - 33))
        elif sr in (37, 38, 39):
            v = self.emit('builtin', 'u32', attr='ctaid%d' % (sr - 37))
        else:
            raise Unsupported('S2R SR%d' % sr)
        self.rset(i.rd, v)

    # float arithmetic
    def i_fadd(self, i):
        if i.form == 'I32':
            a = self.fneg_abs(self.rget(i.ra), i.b(56), i.b(54))
            b = self.fneg_abs(self.opb(i), i.b(53), i.b(57))
            sat = False
        else:
            a = self.fneg_abs(self.rget(i.ra), i.b(48), i.b(46))
            b = self.fneg_abs(self.opb(i, 'f'), i.b(45), i.b(49))
            sat = i.b(50)
        r = self.emit('fadd', 'f32', a, b)
        if sat:
            r = self.sat(r)
        self.rset(i.rd, r)

    i_fadd32i = i_fadd

    def i_fmul(self, i):
        if i.form == 'I32':
            a = self.fr(i.ra)
            b = self.bc_f(self.opb(i))
            sat = i.b(55)
            fmz = i.f(53, 2)
            scale = 0
        else:
            a = self.fneg_abs(self.rget(i.ra), i.b(48), False)
            b = self.bc_f(self.opb(i, 'f'))
            sat = i.b(50)
            fmz = i.f(44, 2)
            scale = i.f(41, 3)
        r = self.fmz_mul(a, b, fmz)
        if scale:
            mul = {1: 0.5, 2: 0.25, 3: 0.125, 4: 8.0, 5: 4.0, 6: 2.0}.get(scale)
            if mul is None:
                raise Unsupported('FMUL scale %d' % scale)
            r = self.emit('fmul', 'f32', r, self.fconst(mul))
        if sat:
            r = self.sat(r)
        self.rset(i.rd, r)

    i_fmul32i = i_fmul

    def i_ffma(self, i):
        a = self.fr(i.ra)
        if i.form == 'I32':
            b = self.bc_f(self.opb(i))
            c = self.fr(i.rd)
            nega, negc, sat, fmz = i.b(56), i.b(57), i.b(55), i.f(53, 2)
        else:
            if i.form == 'RC':      # RC: b is register Rc, c is the cbuf
                b = self.fr(i.rc)
                c = self.bc_f(self.cbuf_const(i.cb_slot, i.cb_off))
            else:
                b = self.bc_f(self.opb(i, 'f'))
                c = self.fr(i.rc)
            nega, negc, sat, fmz = i.b(48), i.b(49), i.b(50), i.f(53, 2)
        if nega:
            a = self.emit('fneg', 'f32', a)
        if negc:
            c = self.emit('fneg', 'f32', c)
        if fmz in (1, 2):
            # FFMA stays fused (one rounding); FMZ only changes the case where a or b is zero:
            # 0 * anything = +0, so the result is 0 + c
            z = self.fconst(0.0)
            az = self.emit('fcmp', 'bool', a, z, attr=2)
            bz = self.emit('fcmp', 'bool', b, z, attr=2)
            anyz = self.emit('lor', 'bool', az, bz)
            r = self.emit('select', 'f32', anyz, self.emit('fadd', 'f32', z, c), self.emit('ffma', 'f32', a, b, c))
        else:
            r = self.emit('ffma', 'f32', a, b, c)
        if sat:
            r = self.sat(r)
        self.rset(i.rd, r)

    i_ffma32i = i_ffma

    def i_fmnmx(self, i):
        a = self.fneg_abs(self.rget(i.ra), i.b(48), i.b(46))
        b = self.fneg_abs(self.opb(i, 'f'), i.b(45), i.b(49))
        p = self.pget(i.f(39, 3), i.b(42))
        mn = self.emit('fmin', 'f32', a, b)
        mx = self.emit('fmax', 'f32', a, b)
        self.rset(i.rd, self.emit('select', 'f32', p, mn, mx))

    def i_mufu(self, i):
        op = i.f(20, 4)
        a = self.fneg_abs(self.rget(i.ra), i.b(48), i.b(46))
        names = {0: 'fcos', 1: 'fsin', 2: 'fex2', 3: 'flg2', 4: 'frcp', 5: 'frsq', 8: 'fsqrt'}
        if op not in names:
            raise Unsupported('MUFU op %d' % op)
        r = self.emit(names[op], 'f32', a)
        if i.b(50):
            r = self.sat(r)
        self.rset(i.rd, r)

    def i_rro(self, i):
        # RRO prepares the operand of MUFU.SIN/COS/EX2; MUFU takes radians/plain values after it.
        b = self.fneg_abs(self.opb(i, 'f'), i.b(45), i.b(49))
        self.rset(i.rd, b)

    def i_fsetp(self, i):
        a = self.fneg_abs(self.rget(i.ra), i.b(43), i.b(7))
        b = self.fneg_abs(self.opb(i, 'f'), i.b(6), i.b(44))
        r = self.fcomp(a, b, i.f(48, 4))
        self.setp_out(i, r, i.f(39, 3), i.b(42), i.f(45, 2))

    def i_fset(self, i):
        if i.form == 'I' or i.form == 'R' or i.form == 'C':
            a = self.fneg_abs(self.rget(i.ra), i.b(43), i.b(54))
            b = self.fneg_abs(self.opb(i, 'f'), i.b(53), i.b(44))
        r = self.fcomp(a, b, i.f(48, 4))
        r = self.bop(r, self.pget(i.f(39, 3), i.b(42)), i.f(45, 2))
        if i.b(52):
            t, f = self.fconst(1.0), self.fconst(0.0)
            v = self.emit('select', 'f32', r, t, f)
        else:
            v = self.emit('select', 'u32', r, self.const(0xFFFFFFFF), self.const(0))
        self.rset(i.rd, v)
        if i.b(47) and self.zn_cc:
            self.set_zn(v, i.b(52))

    def i_fcmp(self, i):
        a = self.rget(i.ra)
        if i.form == 'RC':
            b = self.rget(i.rc)
            c = self.bc_f(self.cbuf_const(i.cb_slot, i.cb_off))
        else:
            b = self.opb(i, 'f')
            c = self.fr(i.rc)
        r = self.fcomp(c, self.fconst(0.0), i.f(48, 4))
        self.rset(i.rd, self.emit('select', 'u32', r, a, self.bc_u(b)))

    # conversions
    def float_src(self, i, sf):
        """F2F/F2I b operand as f32: F16 (half selected by .H1, bit 41) or F32."""
        if sf == 1:
            return self.half_lane(self.opb(i), 1 if i.b(41) else 0)
        if sf == 2:
            return self.bc_f(self.opb(i, 'f'))
        raise Unsupported('%s source format %d' % (i.name.upper(), sf))

    def i_f2f(self, i):
        sf, df = i.f(10, 2), i.f(8, 2)
        rm = ((i.w >> 40) & 4) | ((i.w >> 39) & 3)
        b = self.fneg_abs(self.float_src(i, sf), i.b(45), i.b(49))
        if df not in (1, 2):
            raise Unsupported('F2F %d->%d' % (sf, df))
        if sf == df:
            # same format: integer rounding modes (RN/RM/RP/RZ as floor/ceil/trunc/round-even)
            op = {0: None, 1: 'ffloor', 2: 'fceil', 3: 'ftrunc', 4: 'fround', 5: 'ffloor', 6: 'fceil', 7: 'ftrunc'}[rm]
            if op:
                b = self.emit(op, 'f32', b)
        if i.b(50):
            b = self.sat(b)
        if df == 2:
            self.rset(i.rd, b)
        else:
            r = self.emit('f2h_pack', 'u32', b, self.fconst(0.0))
            if 'half_fold' in COMPAT:
                r.halves = (b, self.fconst(0.0))
            self.rset(i.rd, r)

    def i_f2i(self, i):
        sf = i.f(10, 2)
        dfmt = ((i.w >> 10) & 4) | ((i.w >> 8) & 3)
        b = self.fneg_abs(self.float_src(i, sf), i.b(45), i.b(49))
        rm = i.f(39, 2)
        op = {0: 'fround', 1: 'ffloor', 2: 'fceil', 3: 'ftrunc'}[rm]
        b = self.emit(op, 'f32', b)
        signed = bool(dfmt & 4)
        bits = {0: 8, 1: 16, 2: 32, 3: 64}[dfmt & 3]
        if bits == 64:
            raise Unsupported('F2I 64-bit')
        if signed:
            lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        else:
            lo, hi = 0, (1 << bits) - 1
        # clamp in float (exact for these bounds except 32-bit hi, handled by the compare)
        flo = self.fconst(float(lo))
        r = self.emit('f2s' if signed else 'f2u', 'u32', self.emit('fmax', 'f32', b, flo))
        hi_c = self.fconst(float(hi) if bits < 32 else (2147483648.0 if signed else 4294967296.0))
        over = self.emit('fcmp', 'bool', b, hi_c, attr=6 if bits == 32 else 4)
        r = self.emit('select', 'u32', over, self.const(hi & 0xFFFFFFFF), r)
        nan = self.emit('fcmp', 'bool', b, b, attr=8)
        r = self.emit('select', 'u32', nan, self.const(0), r)
        self.rset(i.rd, r)

    def i_i2f(self, i):
        sfmt = ((i.w >> 11) & 4) | ((i.w >> 10) & 3)
        dfmt = i.f(8, 2)
        if dfmt not in (1, 2):
            raise Unsupported('I2F to F64')
        b = self.opb(i)
        sel = i.f(41, 2)
        bits = {0: 8, 1: 16, 2: 32, 3: 64}[sfmt & 3]
        signed = bool(sfmt & 4)
        if bits == 64:
            raise Unsupported('I2F 64-bit')
        if bits < 32:
            b = self.emit('bfe_s' if signed else 'bfe_u', 'u32', b, self.const(sel * 8), self.const(bits))
        if i.b(49):
            b = self.emit('sabs', 'u32', b)
        if i.b(45):
            b = self.emit('ineg', 'u32', b)
        r = self.emit('s2f' if signed else 'u2f', 'f32', b)
        if dfmt == 1:       # F16 result in H0, H1 cleared (as F2F.F16)
            f = r
            r = self.emit('f2h_pack', 'u32', f, self.fconst(0.0))
            if 'half_fold' in COMPAT:
                r.halves = (f, self.fconst(0.0))
        self.rset(i.rd, r)

    def i_i2i(self, i):
        sfmt = ((i.w >> 11) & 4) | ((i.w >> 10) & 3)
        dfmt = ((i.w >> 10) & 4) | ((i.w >> 8) & 3)
        b = self.opb(i)
        sel = i.f(41, 2)
        sb = {0: 8, 1: 16, 2: 32, 3: 64}[sfmt & 3]
        db = {0: 8, 1: 16, 2: 32, 3: 64}[dfmt & 3]
        if sb == 64 or db == 64:
            raise Unsupported('I2I 64-bit')
        if sb < 32:
            b = self.emit('bfe_s' if sfmt & 4 else 'bfe_u', 'u32', b, self.const(sel * 8), self.const(sb))
        if i.b(49):
            b = self.emit('sabs', 'u32', b)
        if i.b(45):
            b = self.emit('ineg', 'u32', b)
        if i.b(50):
            if dfmt & 4:
                lo, hi = -(1 << (db - 1)), (1 << (db - 1)) - 1
                b = self.emit('smax', 'u32', b, self.const(lo))
                b = self.emit('smin', 'u32', b, self.const(hi))
            else:
                if sfmt & 4:
                    b = self.emit('smax', 'u32', b, self.const(0))
                b = self.emit('umin', 'u32', b, self.const((1 << db) - 1 if db < 32 else 0xFFFFFFFF))
        elif db < 32:
            b = self.emit('bfe_s' if dfmt & 4 else 'bfe_u', 'u32', b, self.const(0), self.const(db))
        self.rset(i.rd, b)

    # integer
    def i_iadd(self, i):
        a = self.rget(i.ra)
        b = self.opb(i)
        if i.form == 'I32':
            avg, x, sat = i.f(55, 2), i.b(53), i.b(54)
        else:
            avg, x, sat = i.f(48, 2), i.b(43), i.b(50)
        cc = i.b(52) if i.form == 'I32' else i.b(47)
        if sat:
            raise Unsupported('IADD.SAT')
        if avg == 1:
            b = self.emit('ineg', 'u32', b)
        elif avg == 2:
            a = self.emit('ineg', 'u32', a)
        r = self.emit('iadd', 'u32', a, b)
        if avg == 3:
            if x or cc:
                raise Unsupported('IADD.PO with carry')
            r = self.emit('iadd', 'u32', r, self.const(1))
        cin = None
        if x:
            # carry-in from the CC flag written by an earlier IADD.CC (stored as predicate 'cc')
            cin = self.pget('cc')
            r = self.emit('iadd', 'u32', r, self.emit('select', 'u32', cin, self.const(1), self.const(0)))
        r.sym = self.sym_bin('add', a, b) if avg == 0 and not x else None
        if cc:
            # unsigned carry-out of a + b (+ cin)
            co = self.emit('icmp', 'bool', r, a, attr='ult')
            if cin is not None:
                eq = self.emit('icmp', 'bool', r, a, attr='eq')
                co = self.emit('lor', 'bool', co, self.emit('land', 'bool', cin, eq))
            self.pset('cc', co)
        self.rset(i.rd, r)

    i_iadd32i = i_iadd

    def i_iadd3(self, i):
        if i.f(31, 6) or i.f(37, 2) or i.b(48):
            raise Unsupported('IADD3 with parts/shift/carry')
        a, b, c = self.rget(i.ra), self.opb(i), self.rget(i.rc)
        if i.b(51):
            a = self.emit('ineg', 'u32', a)
        if i.b(50):
            b = self.emit('ineg', 'u32', b)
        if i.b(49):
            c = self.emit('ineg', 'u32', c)
        self.rset(i.rd, self.emit('iadd', 'u32', self.emit('iadd', 'u32', a, b), c))

    def i_iscadd(self, i):
        a = self.rget(i.ra)
        b = self.opb(i)
        if i.form == 'I32':
            sh = i.f(53, 5)
            avg = 0
        else:
            sh = i.f(39, 5)
            avg = i.f(48, 2)
        a = self.emit('shl', 'u32', a, self.const(sh))
        if avg == 1:
            b = self.emit('ineg', 'u32', b)
        elif avg == 2:
            a = self.emit('ineg', 'u32', a)
        elif avg == 3:
            raise Unsupported('ISCADD.PO')
        self.rset(i.rd, self.emit('iadd', 'u32', a, b))

    i_iscadd32i = i_iscadd

    def i_xmad(self, i):
        a = self.rget(i.ra)
        if i.form == 'I':
            b = self.const(i.f(20, 16))
            hib = False
            c = self.rget(i.rc)
            cop = i.f(50, 3)
        elif i.form == 'C':
            b = self.cbuf_const(i.cb_slot, i.cb_off)
            hib = i.b(52)
            c = self.rget(i.rc)
            cop = i.f(50, 2)
        elif i.form == 'RC':
            b = self.rget(i.rc)
            hib = i.b(52)
            c = self.cbuf_const(i.cb_slot, i.cb_off)
            cop = i.f(50, 2)
        else:
            b = self.rget(i.rb)
            hib = i.b(35)
            c = self.rget(i.rc)
            cop = i.f(50, 3)
        hia, sa, sb, psl, mrg, x = i.b(53), i.b(48), i.b(49), i.b(36), i.b(37), i.b(38)
        if x:
            raise Unsupported('XMAD.X')
        b_raw = b

        def ext16(v, high, signed):
            if signed and high:
                return self.emit('sar', 'u32', v, self.const(16))
            if signed:
                return self.emit('bfe_s', 'u32', v, self.const(0), self.const(16))
            if high:
                return self.emit('shr', 'u32', v, self.const(16))
            return self.emit('and', 'u32', v, self.const(0xFFFF))
        ea = ext16(a, hia, sa)
        eb = ext16(b, hib, sb)
        r = self.emit('imul', 'u32', ea, eb)
        if psl:
            r = self.emit('shl', 'u32', r, self.const(16))
        if i.form in ('C', 'RC'):
            cop = {0: 0, 1: 1, 2: 2, 3: 3}[cop]
        if cop == 1:
            c = self.emit('and', 'u32', c, self.const(0xFFFF))
        elif cop == 2:
            c = self.emit('shr', 'u32', c, self.const(16))
        elif cop == 3:
            adj_a = self.emit('shl', 'u32', self.emit('shr', 'u32', ea, self.const(31)), self.const(16))
            adj_b = self.emit('shl', 'u32', self.emit('shr', 'u32', eb, self.const(31)), self.const(16))
            c = self.emit('isub', 'u32', c, self.emit('iadd', 'u32', adj_a, adj_b))
        elif cop == 4:
            c = self.emit('iadd', 'u32', c, self.emit('shl', 'u32', b_raw, self.const(16)))
        elif cop != 0:
            raise Unsupported('XMAD mode %d' % cop)
        r = self.emit('iadd', 'u32', r, c)
        if mrg:
            r = self.emit('or', 'u32', self.emit('and', 'u32', r, self.const(0xFFFF)),
                          self.emit('shl', 'u32', b_raw, self.const(16)))
        self.rset(i.rd, r)

    def i_isetp(self, i):
        if i.b(43):
            raise Unsupported('ISETP.X')
        a = self.rget(i.ra)
        b = self.opb(i)
        r = self.icomp(a, b, i.f(49, 3), i.b(48))
        self.setp_out(i, r, i.f(39, 3), i.b(42), i.f(45, 2))

    def i_iset(self, i):
        if i.b(43):
            raise Unsupported('ISET.X')
        a = self.rget(i.ra)
        b = self.opb(i)
        r = self.icomp(a, b, i.f(49, 3), i.b(48))
        r = self.bop(r, self.pget(i.f(39, 3), i.b(42)), i.f(45, 2))
        if i.b(44):
            self.rset(i.rd, self.emit('select', 'u32', r, self.const(F32_ONE), self.const(0)))
        else:
            self.rset(i.rd, self.emit('select', 'u32', r, self.const(0xFFFFFFFF), self.const(0)))

    def i_imnmx(self, i):
        if i.f(43, 2):
            raise Unsupported('IMNMX extended mode')
        a, b = self.rget(i.ra), self.opb(i)
        s = i.b(48)
        p = self.pget(i.f(39, 3), i.b(42))
        mn = self.emit('smin' if s else 'umin', 'u32', a, b)
        mx = self.emit('smax' if s else 'umax', 'u32', a, b)
        self.rset(i.rd, self.emit('select', 'u32', p, mn, mx))

    def lop_core(self, a, b, op):
        if op == 0:
            return self.emit('and', 'u32', a, b)
        if op == 1:
            return self.emit('or', 'u32', a, b)
        if op == 2:
            return self.emit('xor', 'u32', a, b)
        return b

    def i_lop(self, i):
        a = self.rget(i.ra)
        b = self.opb(i)
        if i.form == 'I32':
            op, na, nb, x = i.f(53, 2), i.b(55), i.b(56), i.b(57)
            pop = None
        else:
            op, na, nb, x = i.f(41, 2), i.b(39), i.b(40), i.b(43)
            pop = i.f(44, 2)
        if x:
            raise Unsupported('LOP.X')
        if na:
            a = self.emit('not', 'u32', a)
        if nb:
            b = self.emit('not', 'u32', b)
        r = self.lop_core(a, b, op)
        if op == 1 and not na and not nb:
            r.sym = self.sym_bin('or', a, b)
        elif op == 3 and not nb:
            r.sym = b.sym
        self.rset(i.rd, r)
        if pop is not None and i.f(48, 3) != PT:
            if pop == 0:
                pv = self.const(0, 'bool')
            elif pop == 1:
                pv = self.const(1, 'bool')
            else:
                pv = self.emit('icmp', 'bool', r, self.const(0), attr='eq' if pop == 2 else 'ne')
            self.pset(i.f(48, 3), pv)

    i_lop32i = i_lop

    def i_lop3(self, i):
        if i.form != 'R':
            raise Unsupported('LOP3 form %s' % i.form)
        a, b, c = self.rget(i.ra), self.rget(i.rb), self.rget(i.rc)
        lut = i.f(28, 8)
        # sum of minterms
        res = self.const(0)
        na, nb, nc = (self.emit('not', 'u32', x) for x in (a, b, c))
        for m in range(8):
            if lut & (1 << m):
                t = self.emit('and', 'u32', a if m & 4 else na, b if m & 2 else nb)
                t = self.emit('and', 'u32', t, c if m & 1 else nc)
                res = self.emit('or', 'u32', res, t)
        self.rset(i.rd, res)
        if i.f(48, 3) != PT:
            raise Unsupported('LOP3 predicate output')

    def shift_amount_clamp(self, v, x, amount, left, signed, wrap):
        if wrap:
            amount = self.emit('and', 'u32', amount, self.const(31))
            return self.emit('shl' if left else ('sar' if signed else 'shr'), 'u32', v, amount)
        big = self.emit('icmp', 'bool', amount, self.const(31), attr='ugt')
        am = self.emit('umin', 'u32', amount, self.const(31))
        r = self.emit('shl' if left else ('sar' if signed else 'shr'), 'u32', v, am)
        if left or not signed:
            fill = self.const(0)
        else:
            fill = self.emit('sar', 'u32', v, self.const(31))
        return self.emit('select', 'u32', big, fill, r)

    def i_shl(self, i):
        if i.b(43):
            raise Unsupported('SHL.X')
        a = self.rget(i.ra)
        b = self.opb(i)
        if b.op == 'const' and b.attr < 32:
            r = self.emit('shl', 'u32', a, b)
        else:
            r = self.shift_amount_clamp(a, None, b, True, False, i.b(39))
        self.rset(i.rd, r)

    def i_shr(self, i):
        if i.f(43, 2) or i.b(40):
            raise Unsupported('SHR.X/BREV')
        a = self.rget(i.ra)
        b = self.opb(i)
        s = i.b(48)
        if b.op == 'const' and b.attr < 32:
            r = self.emit('sar' if s else 'shr', 'u32', a, b)
        else:
            r = self.shift_amount_clamp(a, None, b, False, s, i.b(39))
        self.rset(i.rd, r)

    def i_bfe(self, i):
        if i.b(40):
            raise Unsupported('BFE.BREV')
        a = self.rget(i.ra)
        b = self.opb(i)
        pos = self.emit('and', 'u32', b, self.const(0xFF))
        ln = self.emit('bfe_u', 'u32', b, self.const(8), self.const(8))
        s = i.b(48)
        # Maxwell clamps; for in-range fields (all our corpus uses constants) plain extract is exact
        if b.op == 'const' and (b.attr & 0xFF) + ((b.attr >> 8) & 0xFF) <= 32:
            r = self.emit('bfe_s' if s else 'bfe_u', 'u32', a, pos, ln)
        else:
            raise Unsupported('BFE with variable field')
        self.rset(i.rd, r)

    def i_bfi(self, i):
        a = self.rget(i.ra)
        if i.form == 'RC':
            b = self.rget(i.rc)
            c = self.cbuf_const(i.cb_slot, i.cb_off)
        else:
            b = self.opb(i)
            c = self.rget(i.rc)
        if b.op != 'const':
            raise Unsupported('BFI with variable field')
        pos, ln = b.attr & 0xFF, (b.attr >> 8) & 0xFF
        if pos + ln > 32:
            ln = max(0, 32 - pos)
        if pos >= 32:
            self.rset(i.rd, c)
            return
        r = self.emit('bfi', 'u32', c, a, self.const(pos), self.const(ln))
        self.rset(i.rd, r)

    def i_popc(self, i):
        b = self.opb(i)
        if i.b(40):
            b = self.emit('not', 'u32', b)
        self.rset(i.rd, self.emit('popc', 'u32', b))

    def i_flo(self, i):
        b = self.opb(i)
        if i.b(40):
            b = self.emit('not', 'u32', b)
        r = self.emit('findmsb_s' if i.b(48) else 'findmsb_u', 'u32', b)
        if i.b(41):
            # shift amount form: 31 - msb (0xFFFFFFFF stays)
            ok = self.emit('icmp', 'bool', r, self.const(0xFFFFFFFF), attr='ne')
            r = self.emit('select', 'u32', ok, self.emit('isub', 'u32', self.const(31), r), r)
        self.rset(i.rd, r)

    def i_pset(self, i):
        a = self.pget(i.f(12, 3), i.b(15))
        b = self.pget(i.f(29, 3), i.b(32))
        c = self.pget(i.f(39, 3), i.b(42))
        r = self.bop(self.bop(a, b, i.f(24, 2)), c, i.f(45, 2))
        if i.b(44):
            self.rset(i.rd, self.emit('select', 'u32', r, self.const(F32_ONE), self.const(0)))
        else:
            self.rset(i.rd, self.emit('select', 'u32', r, self.const(0xFFFFFFFF), self.const(0)))

    def i_psetp(self, i):
        a = self.pget(i.f(12, 3), i.b(15))
        b = self.pget(i.f(29, 3), i.b(32))
        ab = self.bop(a, b, i.f(24, 2))
        self.setp_out(i, ab, i.f(39, 3), i.b(42), i.f(45, 2))

    def i_p2r(self, i):
        raise Unsupported('P2R')

    def i_r2p(self, i):
        raise Unsupported('R2P')

    # half precision (computed in f32, packed back to f16)
    def half_src(self, v, swz, nega=False, absa=False):
        if swz == 1:      # F32 operand
            f = self.bc_f(v)
            lo = hi = f
        elif swz == 0:
            lo, hi = self.half_lane(v, 0), self.half_lane(v, 1)
        elif swz == 2:
            lo = hi = self.half_lane(v, 0)
        else:
            lo = hi = self.half_lane(v, 1)
        out = []
        for x in (lo, hi):
            if absa:
                x = self.emit('fabs', 'f32', x)
            if nega:
                x = self.emit('fneg', 'f32', x)
            out.append(x)
        return out

    def half_imm(self, i):
        h0 = i.f(20, 10) << 6
        h1 = (((i.w >> 47) & 0x200) | ((i.w >> 30) & 0x1FF)) << 6
        return self.const(h0 | (h1 << 16))

    def half_out(self, i, lo, hi, ofmt, sat):
        if sat:
            lo, hi = self.sat(lo), self.sat(hi)
        fold = 'half_fold' in COMPAT
        if ofmt == 0:
            r = self.emit('f2h_pack', 'u32', lo, hi)
            halves = (lo, hi)
        elif ofmt == 1:
            r = self.bc_u(lo)
            halves = None
        elif ofmt == 2:     # MRG_H0: lane 0 result into H0, keep the old H1
            old = self.rget(i.rd)
            p = self.emit('f2h_pack', 'u32', lo, self.fconst(0.0))
            r = self.emit('or', 'u32', self.emit('and', 'u32', old, self.const(0xFFFF0000)), p)
            halves = (lo, self.half_lane(old, 1)) if fold else None
        else:               # MRG_H1: lane 1 result into H1, keep the old H0
            old = self.rget(i.rd)
            p = self.emit('f2h_pack', 'u32', self.fconst(0.0), hi)
            r = self.emit('or', 'u32', self.emit('and', 'u32', old, self.const(0xFFFF)), p)
            halves = (self.half_lane(old, 0), hi) if fold else None
        if fold and halves:
            r.halves = halves
        self.rset(i.rd, r)

    def half_lane(self, v, lane):
        """Half lane of a packed register as f32. Under the half_fold oracle switch a lane that was
        just packed from an f32 value returns that value unrounded (what Ryubing's optimizer does)."""
        h = getattr(v, 'halves', None)
        if h is not None and 'half_fold' in COMPAT:
            return h[lane]
        if lane:
            v = self.emit('shr', 'u32', v, self.const(16))
        return self.emit('h2f_lo', 'f32', v)

    def i_hadd2(self, i):
        f = i.form
        if f == 'I32':
            raise Unsupported('HADD2_32I')
        a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(43), i.b(44))
        if f == 'R':
            b = self.half_src(self.rget(i.rb), i.f(28, 2), i.b(31), i.b(30))
            sat = i.b(32)
        elif f == 'I':
            b = self.half_src(self.half_imm(i), 0)
            sat = i.b(52)
        else:
            b = self.half_src(self.cbuf_const(i.cb_slot, i.cb_off), 1, i.b(56), i.b(54))
            sat = i.b(52)
        lo = self.emit('fadd', 'f32', a[0], b[0])
        hi = self.emit('fadd', 'f32', a[1], b[1])
        self.half_out(i, lo, hi, i.f(49, 2), sat)

    def i_hmul2(self, i):
        f = i.form
        if f == 'I32':
            raise Unsupported('HMUL2_32I')
        if f == 'R':
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(31), i.b(44))
            b = self.half_src(self.rget(i.rb), i.f(28, 2), False, i.b(30))
            sat = i.b(32)
        elif f == 'I':
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(43), i.b(44))
            b = self.half_src(self.half_imm(i), 0)
            sat = i.b(52)
        else:
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(43), i.b(44))
            b = self.half_src(self.cbuf_const(i.cb_slot, i.cb_off), 1, False, i.b(54))
            sat = i.b(52)
        lo = self.emit('fmul', 'f32', a[0], b[0])
        hi = self.emit('fmul', 'f32', a[1], b[1])
        self.half_out(i, lo, hi, i.f(49, 2), sat)

    def i_hfma2(self, i):
        f = i.form
        if f == 'I32':
            raise Unsupported('HFMA2_32I')
        if f == 'R':
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(31))
            b = self.half_src(self.rget(i.rb), i.f(28, 2))
            c = self.half_src(self.rget(i.rc), i.f(35, 2), i.b(30))
            sat = i.b(32)
        elif f == 'I':
            a = self.half_src(self.rget(i.ra), i.f(47, 2))
            b = self.half_src(self.half_imm(i), 0)
            c = self.half_src(self.rget(i.rc), i.f(53, 2), i.b(51))
            sat = i.b(52)
        elif f == 'C':
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(56))
            b = self.half_src(self.cbuf_const(i.cb_slot, i.cb_off), 1)
            c = self.half_src(self.rget(i.rc), i.f(53, 2), i.b(51))
            sat = i.b(52)
        else:  # RC: b is register Rc with the swizzle at bits 53-54, c is the cbuf word as one f32
            a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(56))
            b = self.half_src(self.rget(i.rc), i.f(53, 2))
            c = self.half_src(self.cbuf_const(i.cb_slot, i.cb_off), 1, i.b(51))
            sat = i.b(52)
        lo = self.emit('ffma', 'f32', a[0], b[0], c[0])
        hi = self.emit('ffma', 'f32', a[1], b[1], c[1])
        self.half_out(i, lo, hi, i.f(49, 2), sat)

    def i_hsetp2(self, i):
        f = i.form
        a = self.half_src(self.rget(i.ra), i.f(47, 2), i.b(43), i.b(44))
        if f == 'R':
            b = self.half_src(self.rget(i.rb), i.f(28, 2), i.b(31), i.b(30))
            cmp, hand = i.f(35, 4), i.b(49)
        elif f == 'I':
            b = self.half_src(self.half_imm(i), 0)
            cmp, hand = i.f(49, 4), i.b(53)
        else:
            b = self.half_src(self.cbuf_const(i.cb_slot, i.cb_off), 1, i.b(56), i.b(54))
            cmp, hand = i.f(49, 4), i.b(53)
        r0 = self.fcomp(a[0], b[0], cmp)
        r1 = self.fcomp(a[1], b[1], cmp)
        sp = self.pget(i.f(39, 3), i.b(42))
        bop = i.f(45, 2)
        if hand:
            r = self.emit('land', 'bool', r0, r1)
            self.pset(i.f(3, 3), self.bop(r, sp, bop))
            self.pset(i.f(0, 3), self.bop(self.emit('lnot', 'bool', r), sp, bop))
        else:
            self.pset(i.f(3, 3), self.bop(r0, sp, bop))
            self.pset(i.f(0, 3), self.bop(r1, sp, bop))

    # constants and memory
    def i_ldc(self, i):
        size = i.f(48, 3)
        slot = i.f(36, 5)
        off = decoder.sext(i.f(20, 16), 16)
        mode = i.f(44, 2)
        if mode != 0:
            raise Unsupported('LDC address mode %d' % mode)
        ra = self.rget(i.ra)
        nwords = {4: 1, 5: 2}.get(size)
        if nwords is None:
            raise Unsupported('LDC size %d' % size)
        if ra.op == 'const':
            base = (ra.attr + off) & 0xFFFFFFFF
            for k in range(nwords):
                self.rset(i.rd + k, self.cbuf_const(slot, base + 4 * k))
            return
        if slot == 1:
            for k in range(nwords):
                addr = self.emit('iadd', 'u32', ra, self.const(off + 4 * k))
                self.rset(i.rd + k, self.emit('c1_dyn', 'u32', addr))
            return
        self.info.cbufs[slot] = 0x10000
        for k in range(nwords):
            addr = self.emit('iadd', 'u32', ra, self.const(off + 4 * k))
            v = self.emit('cbuf', 'u32', addr, attr=slot)
            v.sym = ('cbx', slot, ra.sym, off + 4 * k) if ra.sym else None
            self.rset(i.rd + k, v)

    def local_addr(self, i, nbytes):
        """Byte address of an LDL/STL as an IR value (constant when known)."""
        ra = self.rget(i.ra)
        off = decoder.sext(i.f(20, 24), 24)
        if ra.op == 'const':
            a = (ra.attr + off) & 0xFFFFFFFF
            if a > 0x10000:
                raise Unsupported('local address out of range')
            self.info.local_words = max(self.info.local_words, (a + nbytes + 3) // 4)
            return self.const(a)
        self.info.local_dynamic = True
        return self.emit('iadd', 'u32', ra, self.const(off))

    LS_SIZE = {0: (8, False), 1: (8, True), 2: (16, False), 3: (16, True), 4: (32, False), 5: (64, False),
               6: (128, False)}

    def word_of(self, addr, k=0):
        w = self.emit('shr', 'u32', addr, self.const(2))
        return self.emit('iadd', 'u32', w, self.const(k)) if k else w

    def sub_shift(self, addr):
        return self.emit('shl', 'u32', self.emit('and', 'u32', addr, self.const(3)), self.const(3))

    def i_ldl(self, i):
        bits, signed = self.LS_SIZE.get(i.f(48, 3), (None, None))
        if bits is None:
            raise Unsupported('LDL size %d' % i.f(48, 3))
        addr = self.local_addr(i, max(4, bits // 8))
        if bits >= 32:
            for k in range(bits // 32):
                self.rset(i.rd + k, self.emit('ldl', 'u32', self.word_of(addr, k)))
            return
        w = self.emit('ldl', 'u32', self.word_of(addr))
        v = self.emit('bfe_s' if signed else 'bfe_u', 'u32', w, self.sub_shift(addr), self.const(bits))
        self.rset(i.rd, v)

    def i_stl(self, i):
        bits, _ = self.LS_SIZE.get(i.f(48, 3), (None, None))
        if bits is None:
            raise Unsupported('STL size %d' % i.f(48, 3))
        addr = self.local_addr(i, max(4, bits // 8))
        if bits >= 32:
            for k in range(bits // 32):
                self.emit('stl', 'void', self.word_of(addr, k), self.rget(i.rd + k))
            return
        widx = self.word_of(addr)
        old = self.emit('ldl', 'u32', widx)
        new = self.emit('bfi', 'u32', old, self.rget(i.rd), self.sub_shift(addr), self.const(bits))
        self.emit('stl', 'void', widx, new)

    def i_ldg(self, i):
        raise Unsupported('LDG (global memory)')

    def i_stg(self, i):
        raise Unsupported('STG (global memory)')

    # attributes
    def attr_kind(self, a, out):
        if 0x80 <= a < 0x280:
            return ('gen', (a - 0x80) >> 4, (a >> 2) & 3)
        if 0x70 <= a < 0x80:
            return ('position', 0, (a >> 2) & 3)
        if a == 0x6c:
            return ('point_size', 0, 0)
        if a == 0x64:
            return ('layer', 0, 0)
        if a == 0x68:
            return ('viewport', 0, 0)
        if 0x2c0 <= a < 0x2e0:
            return ('clip', (a - 0x2c0) >> 2, 0)
        if a in (0x2e0, 0x2e4):
            return ('point_coord', 0, (a >> 2) & 1)
        if a == 0x2f8:
            return ('instance_id', 0, 0)
        if a == 0x2fc:
            return ('vertex_id', 0, 0)
        if a == 0x3fc:
            return ('front_facing', 0, 0)
        if a == 0x60:
            return ('primitive_id', 0, 0)
        raise Unsupported('attribute %#x' % a)

    def read_attr(self, a):
        k = self.attr_kind(a, False)
        kind, idx, comp = k
        if kind == 'gen':
            self.info.inputs.setdefault(idx, set()).add(comp)
            return self.emit('in', 'f32', attr=k)
        if kind in ('instance_id', 'vertex_id', 'primitive_id'):
            self.info.builtins_in.add(kind)
            return self.emit('in', 'u32', attr=k)
        if kind == 'front_facing':
            self.info.builtins_in.add(kind)
            ff = self.emit('in', 'bool', attr=k)
            return self.emit('select', 'u32', ff, self.const(0xFFFFFFFF), self.const(0))
        if kind == 'position' and self.p.stage == 'fs':
            self.info.builtins_in.add('frag_coord')
            return self.emit('in', 'f32', attr=('frag_coord', 0, comp))
        if kind == 'point_coord':
            self.info.builtins_in.add(kind)
            return self.emit('in', 'f32', attr=k)
        raise Unsupported('read of attribute %s in %s' % (kind, self.p.stage))

    def i_ald(self, i):
        if i.b(31) or i.b(32):
            raise Unsupported('ALD.P/O')
        if i.ra != RZ:
            raise Unsupported('ALD indexed')
        base = i.f(20, 11)
        n = i.f(47, 2) + 1
        for k in range(n):
            v = self.read_attr(base + 4 * k)
            self.rset(i.rd + k, v)

    def i_ast(self, i):
        if i.b(31):
            raise Unsupported('AST.P')
        if i.ra != RZ:
            raise Unsupported('AST indexed')
        base = i.f(20, 11)
        n = i.f(47, 2) + 1
        src = i.f(0, 8)
        for k in range(n):
            kind, idx, comp = self.attr_kind(base + 4 * k, True)
            v = self.rget(src + k)
            if kind == 'gen':
                self.info.outputs.setdefault(idx, set()).add(comp)
            elif kind == 'clip':
                self.info.clip_written |= 1 << idx
                self.info.builtins_out.add('clip')
            else:
                self.info.builtins_out.add(kind)
            if kind in ('layer', 'viewport'):
                self.emit('out', 'void', v, attr=(kind, idx, comp))
            else:
                self.emit('out', 'void', self.bc_f(v), attr=(kind, idx, comp))

    def i_ipa(self, i):
        a = i.f(28, 10)
        if i.b(38):
            raise Unsupported('IPA indexed')
        mode = i.f(54, 2)
        k = self.attr_kind(a, False)
        if k[0] == 'gen':
            loc = k[1]
            interp = self.p.sph[6 + (loc >> 2)] >> ((loc & 3) * 8) & 0xFF
            t = (interp >> (2 * k[2])) & 3
            self.info.input_interp[loc] = max(self.info.input_interp.get(loc, 0), t)
            v = self.bc_f(self.read_attr(a))
            if mode == 1:
                # IPA.MUL: hardware interpolates attr/w and multiplies by Rb; with Rb = w (the
                # glslc idiom, Rb = rcp(IPA.PASS a[0x7c])) the host's smooth value is identical.
                rb = self.rget(i.rb)
                if 'ipa_mul' in COMPAT or not self.is_w_reciprocal(rb):
                    fw = self.emit('in', 'f32', attr=('frag_coord', 0, 3))
                    self.info.builtins_in.add('frag_coord')
                    v = self.emit('fmul', 'f32', self.emit('fmul', 'f32', v, fw), self.bc_f(rb))
            elif mode == 0 and t == 2:
                # IPA.PASS of a perspective input returns attr/w linearly interpolated
                fw = self.emit('in', 'f32', attr=('frag_coord', 0, 3))
                self.info.builtins_in.add('frag_coord')
                v = self.emit('fmul', 'f32', v, fw)
        else:
            v = self.bc_f(self.read_attr(a))
        if i.b(51):
            v = self.sat(v)
        self.rset(i.rd, v)

    def is_w_reciprocal(self, v):
        # pattern: MUFU.RCP(IPA.PASS a[0x7c]) possibly through a register in this block
        if v.op == 'bitcast_u':
            v = v.args[0]
        if v.op == 'frcp':
            x = v.args[0]
            if x.op == 'bitcast_f':
                x = x.args[0]
            if x.op == 'rget':
                return getattr(self, 'w_regs', set()) and x.attr in self.w_regs
            return x.op == 'in' and x.attr == ('frag_coord', 0, 3)
        if v.op == 'rget':
            return v.attr in getattr(self, 'wrcp_regs', set())
        return False

    # warp
    def i_shfl(self, i):
        mode = i.f(30, 2)
        a = self.rget(i.ra)
        if i.b(28):
            b = self.const(i.f(20, 5))
        else:
            b = self.rget(i.rb)
        if i.b(29):
            c = i.f(34, 13)
        else:
            raise Unsupported('SHFL with register c operand')
        if i.f(48, 3) != PT:
            raise Unsupported('SHFL predicate output')
        seg = (c >> 8) & 0x1F
        clamp = c & 0x1F
        if mode == 0 and seg == 0x1C and clamp == 3 and b.op == 'const':
            self.info.uses_quad = True
            r = self.emit('quad_bcast', 'u32', a, attr=b.attr & 3)
        else:
            raise Unsupported('SHFL mode %d seg %#x clamp %d' % (mode, seg, clamp))
        self.rset(i.rd, r)

    def i_fswzadd(self, i):
        if i.b(38):
            pass  # NDV: no effect on the result
        a = self.fr(i.ra)
        b = self.fr(i.rb)
        self.info.uses_quad = True
        self.info.uses_subgroup = True
        r = self.emit('fswzadd', 'f32', a, b, attr=i.f(28, 8))
        self.rset(i.rd, r)

    def i_vote(self, i):
        raise Unsupported('VOTE')

    # textures
    def tex_common(self, i, kind):
        if kind == 'tex':
            dim, arr = TEX_DIMS[i.f(28, 3)]
            lod = i.f(37, 3)
            aoffi = i.b(36)
            dc = i.b(50)
            mask = i.f(31, 4)
            lc = i.b(40)
            ms = False
            comp = 0
        elif kind == 'tld':
            dim, arr = TEX_DIMS[i.f(28, 3)]
            lod = 3 if i.b(55) else 1
            aoffi = i.b(35)
            dc = False
            mask = i.f(31, 4)
            lc = i.b(54)
            ms = i.b(50)
            comp = 0
        else:  # tld4
            dim, arr = TEX_DIMS[i.f(28, 3)]
            lod = 0
            toff = i.f(36, 2)
            if toff == 2:
                raise Unsupported('TLD4.PTP')
            aoffi = toff == 1
            dc = i.b(50)
            mask = i.f(31, 4)
            lc = i.b(40)
            ms = False
            comp = i.f(38, 2)
        if lc:
            raise Unsupported('texture LOD clamp')
        if lod in (6, 7):
            raise Unsupported('texture LBA/LLA')
        rd = i.rd
        if rd == RZ:
            return
        ra, rb = i.ra, i.rb
        ra_regs = []
        rb_regs = []

        def take_a():
            nonlocal ra
            if ra == RZ:
                return self.const(0)
            v = self.rget(ra)
            ra += 1
            return v

        def take_b():
            nonlocal rb
            if rb == RZ:
                return self.const(0)
            v = self.rget(rb)
            rb += 1
            return v
        array_idx = take_a() if arr else None
        handle = take_b()
        ncoord = DIM_COORDS[dim]
        coords = [take_a() for _ in range(ncoord)]
        lodv = take_b() if lod > 1 else None
        offs = take_b() if aoffi else None
        dref = take_b() if dc else None
        msv = take_b() if ms else None
        if ms:
            raise Unsupported('multisample fetch')
        tkey = self.texture_key(handle, dim, arr, dc, i)
        args = [handle] + coords
        attr = dict(kind=kind, dim=dim, arr=arr, lod=lod, dc=dc, comp=comp, mask=mask, tkey=tkey,
                    ncoord=ncoord, has_arr=arr, has_lod=lodv is not None, has_off=offs is not None,
                    has_dref=dref is not None)
        extra = [x for x in (array_idx, lodv, offs, dref) if x is not None]
        v = self.emit('tex', 'v4f', *(args + extra), attr=attr)
        out = rd
        for c in range(4):
            if mask & (1 << c):
                e = self.emit('extract', 'f32', v, attr=c)
                self.rset(out, e)
                out += 1
                if out > 254:
                    break

    def texture_key(self, handle, dim, arr, dc, i):
        self._tex_pending = getattr(self, '_tex_pending', [])
        rec = {'handle': handle, 'dim': dim, 'arr': arr, 'dc': dc, 'addr': i.addr, 'block': self.cur}
        self._tex_pending.append(rec)
        return rec

    def i_tex_b(self, i):
        self.tex_common(i, 'tex')

    def i_tld_b(self, i):
        self.tex_common(i, 'tld')

    def i_tld4_b(self, i):
        self.tex_common(i, 'tld4')

    def i_tex(self, i):
        raise Unsupported('TEX (bound)')

    def i_texs(self, i):
        raise Unsupported('TEXS')

    # ---------- symbolic values ----------
    @staticmethod
    def sym_bin(kind, a, b):
        if a.sym is None or b.sym is None:
            return None
        sa, sb = a.sym, b.sym
        if kind == 'or' and sa[0] == 'imm' and sa[1] == 0:
            return sb
        if kind == 'or' and sb[0] == 'imm' and sb[1] == 0:
            return sa
        if kind == 'add' and sa[0] == 'imm' and sb[0] == 'imm':
            return ('imm', (sa[1] + sb[1]) & 0xFFFFFFFF)
        return (kind,) + tuple(sorted([sa, sb]))

    def reg_sym_in(self, blk, reg, seen):
        """Symbolic value of register `reg` at the start of IR block `blk` (None if unknown)."""
        if blk.id in seen:
            return seen[blk.id]
        seen[blk.id] = 'pending'
        res = None
        vals = set()
        if not blk.preds:
            seen[blk.id] = None
            return None
        for p in blk.preds:
            v = self.reg_sym_out(p, reg, seen)
            if v == 'pending':
                continue
            if v is None:
                vals = None
                break
            vals.add(v)
        if vals and len(vals) == 1:
            res = vals.pop()
        seen[blk.id] = res
        return res

    def reg_sym_out(self, blk, reg, seen):
        for o in reversed(blk.ops):
            if o.op == 'rset' and o.attr == reg:
                v = o.args[0]
                return self.value_sym(blk, v, seen)
        return self.reg_sym_in(blk, reg, seen)

    def value_sym(self, blk, v, seen):
        if v.sym is not None:
            return v.sym
        if v.op == 'rget':
            return self.reg_sym_in(blk, v.attr, seen)
        if v.op == 'or':
            a = self.value_sym(blk, v.args[0], seen)
            b = self.value_sym(blk, v.args[1], seen)
            if a is None or b is None or a == 'pending' or b == 'pending':
                return None
            return self.sym_bin('or', _S(a), _S(b))
        return None

    def resolve_textures(self):
        for rec in getattr(self, '_tex_pending', []):
            s = self.value_sym(rec['block'], rec['handle'], {})
            loc = None
            if s and s[0] == 'cb' and s[1] == 2:
                loc = ((2, s[2]), None)
            elif s and s[0] == 'or' and s[1][0] == 'cb' and s[2][0] == 'cb' and s[1][1] == 2 and s[2][1] == 2:
                t, smp = sorted([s[1][2], s[2][2]])
                loc = ((2, t), (2, smp))
            if loc is None:
                raise Unsupported('bindless texture handle not traced to c[2] (sym %r) at %#x' % (s, rec['addr']))
            key = (loc, rec['dim'], rec['arr'], rec['dc'])
            rec['key'] = key
            self.info.textures.setdefault(key, {'handle': loc[0], 'sampler': loc[1], 'dim': rec['dim'],
                                                'arr': rec['arr'], 'shadow': rec['dc']})


class _S:
    __slots__ = ('sym',)

    def __init__(self, s):
        self.sym = s


def lift(prog):
    lf = Lifter(prog)
    return lf.run()
