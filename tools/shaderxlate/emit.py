"""IR to SPIR-V emitter (our own).

Two control-flow strategies share the per-block code generator:
- structured: for acyclic CFGs, nested selections built from immediate post-dominators;
- dispatcher: a loop around a switch on a block-id variable; always valid, used as fallback.
Resource layout follows docs/SHADER_CACHE.md (v1).
"""
import os
import struct

from spirv import Module, GLSL

SET_OF_STAGE = {'vs': 0, 'fs': 1, 'gs': 2, 'tcs': 3, 'tes': 4, 'cs': 0}
EXEC_OF_STAGE = {'vs': 'Vertex', 'fs': 'Fragment', 'gs': 'Geometry', 'tcs': 'TessellationControl',
                 'tes': 'TessellationEvaluation', 'cs': 'GLCompute'}
DIM_SPV = {'1d': 0, '2d': 1, '3d': 2, 'cube': 3}
IMG_BIAS, IMG_LOD, IMG_CONSTOFF = 0x1, 0x2, 0x8
EXIT_STATE = 0xFFFFFFFF
SCOPE_SUBGROUP = 3

FCMP = {1: 'FOrdLessThan', 2: 'FOrdEqual', 3: 'FOrdLessThanEqual', 4: 'FOrdGreaterThan',
        5: 'FOrdNotEqual', 6: 'FOrdGreaterThanEqual', 9: 'FUnordLessThan', 10: 'FUnordEqual',
        11: 'FUnordLessThanEqual', 12: 'FUnordGreaterThan', 13: 'FUnordNotEqual',
        14: 'FUnordGreaterThanEqual'}
ICMP = {'eq': 'IEqual', 'ne': 'INotEqual', 'slt': 'SLessThan', 'ult': 'ULessThan',
        'sle': 'SLessThanEqual', 'ule': 'ULessThanEqual', 'sgt': 'SGreaterThan',
        'ugt': 'UGreaterThan', 'sge': 'SGreaterThanEqual', 'uge': 'UGreaterThanEqual'}
BIN_U = {'iadd': 'IAdd', 'isub': 'ISub', 'imul': 'IMul', 'and': 'BitwiseAnd', 'or': 'BitwiseOr',
         'xor': 'BitwiseXor', 'shl': 'ShiftLeftLogical', 'shr': 'ShiftRightLogical',
         'sar': 'ShiftRightArithmetic'}
EXT_U2 = {'umin': 'UMin', 'umax': 'UMax', 'smin': 'SMin', 'smax': 'SMax'}
EXT_F1 = {'fsin': 'Sin', 'fcos': 'Cos', 'fex2': 'Exp2', 'flg2': 'Log2', 'frsq': 'InverseSqrt',
          'fsqrt': 'Sqrt', 'ffloor': 'Floor', 'fceil': 'Ceil', 'ftrunc': 'Trunc', 'fround': 'RoundEven',
          'fabs': 'FAbs'}
STATEMENTS = {'rset', 'pset', 'out', 'stl', 'demote'}


class EmitError(Exception):
    pass


class Fallback(Exception):
    pass


def f32_bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def _sext4(v):
    return v - 16 if v & 8 else v


# ---------------------------------------------------------------- analysis

def analyze(fn):
    """Reachable blocks, preds, register liveness and the set of live op numbers."""
    blocks = fn.reachable()
    ids = {b.id for b in blocks}
    for b in blocks:
        b.preds = []
    for b in blocks:
        for s in b.succs():
            s.preds.append(b)
    live_in = {b.id: frozenset() for b in blocks}

    def walk(b, live, mark, extra=frozenset()):
        need = set(extra)
        t = b.term
        if t and t[0] == 'cbr':
            need.add(t[1].n)
        for o in reversed(b.ops):
            root = False
            if o.op == 'rset':
                if o.attr in live:
                    root = True
                    live.discard(o.attr)
            elif o.op == 'pset':
                key = ('p', o.attr)
                if key in live:
                    root = True
                    live.discard(key)
            elif o.op in STATEMENTS:
                root = True
            if root:
                need.add(o.n)
            if o.n in need:
                if o.op == 'rget':
                    live.add(o.attr)
                elif o.op == 'pget':
                    live.add(('p', o.attr))
                args = o.args[1:] if o.op == 'tex' else o.args
                for a in args:
                    need.add(a.n)
        if mark is not None:
            mark |= need
        return live

    # Values normally stay inside their block (registers carry them across). The half_fold oracle
    # mirror (lift.fold_halves_across_blocks) uses values from dominating blocks; then repeat with the
    # needed set seeded so the producing block keeps them.
    extra = frozenset()
    while True:
        changed = True
        order = list(reversed(blocks))
        while changed:
            changed = False
            for b in order:
                out = set()
                for s in b.succs():
                    out |= live_in[s.id]
                li = frozenset(walk(b, out, None, extra))
                if li != live_in[b.id]:
                    live_in[b.id] = li
                    changed = True
        mark = set()
        for b in blocks:
            out = set()
            for s in b.succs():
                out |= live_in[s.id]
            walk(b, out, mark, extra)
        if 'half_fold' not in os.environ.get('SXL_COMPAT', '').split(',') or mark == extra:
            break
        extra = frozenset(mark)
    return blocks, mark, ids


def is_acyclic(blocks):
    state = {}
    for root in blocks[:1]:
        st = [(root, iter(root.succs()))]
        state[root.id] = 1
        while st:
            b, it = st[-1]
            nxt = next(it, None)
            if nxt is None:
                state[b.id] = 2
                st.pop()
                continue
            s = state.get(nxt.id, 0)
            if s == 1:
                return False
            if s == 0:
                state[nxt.id] = 1
                st.append((nxt, iter(nxt.succs())))
    return True


def find_loops(blocks):
    """Natural loops of a reducible CFG: {header id: (body ids, exit block or None)}.
    Raises Fallback for irreducible flow or a loop with more than one exit target."""
    entry = blocks[0]
    byid = {b.id: b for b in blocks}
    rpo = []
    seen = {entry.id}
    onstack = {entry.id}
    retreat = []
    st = [(entry, iter(entry.succs()))]
    while st:
        x, it = st[-1]
        n = next(it, None)
        if n is None:
            onstack.discard(x.id)
            rpo.append(x)
            st.pop()
        elif n.id in onstack:
            retreat.append((x, n))
        elif n.id not in seen:
            seen.add(n.id)
            onstack.add(n.id)
            st.append((n, iter(n.succs())))
    rpo.reverse()
    if not retreat:
        return {}
    dom = {entry.id: {entry.id}}
    changed = True
    while changed:
        changed = False
        for b in rpo[1:]:
            ps = [dom[q.id] for q in b.preds if q.id in dom]
            d = (set.intersection(*ps) if ps else set()) | {b.id}
            if dom.get(b.id) != d:
                dom[b.id] = d
                changed = True
    loops = {}
    for src, h in retreat:
        if h.id not in dom[src.id]:
            raise Fallback('irreducible control flow')
        body = loops.setdefault(h.id, [set([h.id]), None])[0]
        work = [src]
        while work:
            x = work.pop()
            if x.id in body:
                continue
            body.add(x.id)
            work.extend(x.preds)
    for hid, ent in loops.items():
        body = ent[0]
        exits = {t.id for n in body for t in byid[n].succs() if t.id not in body}
        if len(exits) > 1:
            raise Fallback('loop with %d exit targets' % len(exits))
        ent[1] = byid[exits.pop()] if exits else None
    return {h: (b, m) for h, (b, m) in loops.items()}


def ipdoms(blocks, succ=None):
    """Immediate post-dominator of each block of an acyclic CFG (None = virtual exit)."""
    succ = succ or (lambda b: b.succs())
    order = []
    seen = set()

    def post(b):
        st = [(b, iter(succ(b)))]
        seen.add(b.id)
        while st:
            x, it = st[-1]
            n = next(it, None)
            if n is None:
                order.append(x)
                st.pop()
            elif n.id not in seen:
                seen.add(n.id)
                st.append((n, iter(succ(n))))
    post(blocks[0])
    pd = {}
    for b in order:      # successors first
        ss = succ(b)
        if not ss:
            pd[b.id] = frozenset([b.id])
            continue
        acc = None
        for s in ss:
            acc = pd[s.id] if acc is None else acc & pd[s.id]
        pd[b.id] = acc | {b.id}
    byid = {b.id: b for b in blocks}
    res = {}
    for b in blocks:
        strict = pd[b.id] - {b.id}
        best = None
        for c in strict:
            if best is None or len(pd[c]) > len(pd[best]):
                best = c
        res[b.id] = byid[best] if best is not None else None
    return res


# ---------------------------------------------------------------- emitter

class Emitter:
    def __init__(self, fn, info, prog, mode='structured'):
        self.fn = fn
        self.info = info
        self.p = prog
        self.stage = prog.stage
        self.mode = mode
        self.m = Module((1, 3))
        m = self.m
        m.cap('Shader')
        self.T_void = m.t_void()
        self.T_bool = m.t_bool()
        self.T_u32 = m.t_u32()
        self.T_i32 = m.t_i32()
        self.T_f32 = m.t_f32()
        self.T_v4f = m.t_vec(self.T_f32, 4)
        self.T_v2f = m.t_vec(self.T_f32, 2)
        self.T_v4u = m.t_vec(self.T_u32, 4)
        self.T_ivec = {1: self.T_i32, 2: m.t_vec(self.T_i32, 2), 3: m.t_vec(self.T_i32, 3),
                       4: m.t_vec(self.T_i32, 4)}
        self.T_fvec = {1: self.T_f32, 2: self.T_v2f, 3: m.t_vec(self.T_f32, 3), 4: self.T_v4f}
        self.TY = {'u32': self.T_u32, 'f32': self.T_f32, 'bool': self.T_bool, 'v4f': self.T_v4f}
        self.set = SET_OF_STAGE[self.stage]
        self.interface = []
        self.inputs = {}
        self.outputs = {}
        self.regvars = {}
        self.predvars = {}
        self.fvars = []
        self.ubos = {}
        self.textures = {}
        self.manifest_tex = []
        self.manifest_ubo = []
        self.c1var = None
        self.localvar = None
        self.uses_lane = False
        self.v = {}
        self.noc = []

    # ---- small helpers
    def ins(self, op, *w):
        Module.ins(self.code, op, *w)

    def new(self, op, ty, *w):
        r = self.m.id()
        Module.ins(self.code, op, ty, r, *w)
        return r

    def ext(self, ty, inst, *args):
        r = self.m.id()
        Module.ins(self.code, 'ExtInst', ty, r, self.m.glsl_id(), GLSL[inst], *args)
        return r

    def cu(self, v):
        return self.m.const_u32(v)

    def cf(self, f):
        return self.m.const_f32(f)

    def get(self, o, want):
        """SPIR-V id of IR value o, converted to type want ('u32'/'f32'/'bool')."""
        r = self.v[o.n]
        if o.ty == want or want is None:
            return r
        if {o.ty, want} == {'u32', 'f32'}:
            return self.new('Bitcast', self.TY[want], r)
        raise EmitError('type %s used as %s (%s)' % (o.ty, want, o.op))

    # ---- variables
    def fvar(self, ty, init=None):
        pt = self.m.t_ptr('Function', ty)
        v = self.m.id()
        if init is None:
            Module.ins(self.fvars, 'Variable', pt, v, 7)
        else:
            Module.ins(self.fvars, 'Variable', pt, v, 7, init)
        return v

    def regvar(self, r):
        v = self.regvars.get(r)
        if v is None:
            v = self.regvars[r] = self.fvar(self.T_u32, self.cu(0))
            self.m.name(v, 'R%d' % r)
        return v

    def predvar(self, p):
        v = self.predvars.get(p)
        if v is None:
            v = self.predvars[p] = self.fvar(self.T_bool, self.m.const_bool(False))
            self.m.name(v, 'P%s' % p)
        return v

    def io_var(self, sc, ty, name, init=False):
        m = self.m
        if init:
            v = m.gvar(m.t_ptr(sc, ty), sc, m.const_null(ty))
        else:
            v = m.gvar(m.t_ptr(sc, ty), sc)
        m.name(v, name)
        self.interface.append(v)
        return v

    def input_var(self, kind, idx):
        key = (kind, idx)
        v = self.inputs.get(key)
        if v is not None:
            return v
        m = self.m
        if kind == 'gen':
            v = self.io_var('Input', self.T_v4f, 'in%d' % idx)
            m.deco(v, 'Location', idx)
            if self.stage == 'fs':
                t = self.info.input_interp.get(idx, 2)
                if t == 1:
                    m.deco(v, 'Flat')
                elif t == 3:
                    m.deco(v, 'NoPerspective')
        elif kind == 'frag_coord':
            v = self.io_var('Input', self.T_v4f, 'FragCoord')
            m.deco(v, 'BuiltIn', 15)
        elif kind == 'point_coord':
            v = self.io_var('Input', self.T_v2f, 'PointCoord')
            m.deco(v, 'BuiltIn', 16)
        elif kind == 'front_facing':
            v = self.io_var('Input', self.T_bool, 'FrontFacing')
            m.deco(v, 'BuiltIn', 17)
        elif kind == 'vertex_id':
            v = self.io_var('Input', self.T_i32, 'VertexIndex')
            m.deco(v, 'BuiltIn', 42)
        elif kind == 'instance_id':
            v = self.io_var('Input', self.T_i32, 'InstanceIndex')
            m.deco(v, 'BuiltIn', 43)
        elif kind == 'lane_id':
            m.cap('GroupNonUniform')
            v = self.io_var('Input', self.T_u32, 'SubgroupLocalInvocationId')
            m.deco(v, 'BuiltIn', 41)
            if self.stage == 'fs':
                m.deco(v, 'Flat')
        else:
            raise EmitError('input %s' % kind)
        self.inputs[key] = v
        return v

    def output_var(self, kind, idx):
        key = (kind, idx)
        v = self.outputs.get(key)
        if v is not None:
            return v
        m = self.m
        if kind == 'gen':
            v = self.io_var('Output', self.T_v4f, 'out%d' % idx, init=True)
            m.deco(v, 'Location', idx)
        elif kind == 'frag':
            v = self.io_var('Output', self.T_v4f, 'rt%d' % idx, init=True)
            m.deco(v, 'Location', idx)
        elif kind == 'depth':
            v = self.io_var('Output', self.T_f32, 'FragDepth')
            m.deco(v, 'BuiltIn', 22)
        elif kind == 'position':
            v = self.io_var('Output', self.T_v4f, 'Position', init=True)
            m.deco(v, 'BuiltIn', 0)
            m.deco(v, 'Invariant')
        elif kind == 'point_size':
            v = self.io_var('Output', self.T_f32, 'PointSize')
            m.deco(v, 'BuiltIn', 1)
        elif kind == 'clip':
            n = max(1, self.info.clip_written.bit_length())
            v = self.io_var('Output', m.t_array(self.T_f32, n), 'ClipDistance')
            m.deco(v, 'BuiltIn', 3)
            m.cap('ClipDistance')
        elif kind in ('layer', 'viewport'):
            v = self.io_var('Output', self.T_i32, kind)
            m.deco(v, 'BuiltIn', 9 if kind == 'layer' else 10)
            if self.stage != 'gs':
                m.cap('ShaderViewportIndexLayerEXT')
                m.ext('SPV_EXT_shader_viewport_index_layer')
            elif kind == 'viewport':
                m.cap('MultiViewport')
        else:
            raise EmitError('output %s' % kind)
        self.outputs[key] = v
        return v

    # ---- resources
    def plan_resources(self, blocks, live):
        """Assign bindings: UBOs (by c-buffer slot) then textures, compact from 1."""
        slots = {}
        texkeys = []
        for b in blocks:
            for o in b.ops:
                if o.n not in live:
                    continue
                if o.op == 'cbuf':
                    off = o.args[0]
                    hi = 0x10000 if off.op != 'const' else off.attr + 4
                    slots[o.attr] = max(slots.get(o.attr, 0), hi)
                elif o.op == 'tex':
                    k = o.attr['tkey']['key']
                    if k not in texkeys:
                        texkeys.append(k)
        m = self.m
        binding = 1
        for slot in sorted(slots):
            size = min(0x10000, (slots[slot] + 15) & ~15)
            n = size // 16
            arr = m.t_array(self.T_v4u, n)
            m.deco(arr, 'ArrayStride', 16) if ('stride', arr) not in m._types else None
            m._types[('stride', arr)] = 0
            st = m.t_struct(('ubo', slot), arr)
            m.deco(st, 'Block')
            m.mdeco(st, 0, 'Offset', 0)
            var = m.gvar(m.t_ptr('Uniform', st), 'Uniform')
            m.name(var, 'c%d' % slot)
            m.deco(var, 'DescriptorSet', self.set)
            m.deco(var, 'Binding', binding)
            self.ubos[slot] = var
            self.manifest_ubo.append({'binding': binding, 'cbuf': slot, 'nvn_index': slot - 3, 'size': size})
            binding += 1
        for k in texkeys:
            (h, s), dim, arr, dc = k
            img = m.t_image(self.T_f32, DIM_SPV[dim], 1 if dc else 0, 1 if arr else 0, 0, 1, 0)
            if dim == '1d':
                m.cap('Sampled1D')
            if dim == 'cube' and arr:
                m.cap('ImageCubeArray')
            simg = m.t_sampled_image(img)
            var = m.gvar(m.t_ptr('UniformConstant', simg), 'UniformConstant')
            m.name(var, 'tex%d' % binding)
            m.deco(var, 'DescriptorSet', self.set)
            m.deco(var, 'Binding', binding)
            self.textures[k] = (var, img, simg)
            hoff, soff = h[1], (s[1] if s else None)
            if s is None:
                if 0x20 <= hoff < 0x168 and (hoff - 0x20) % 8 == 0:
                    nvn = {'kind': 'combined', 'index': (hoff - 0x20) // 8}
                else:
                    nvn = {'kind': 'raw'}
            else:
                if 0x168 <= hoff < 0x568 and 0x568 <= soff and (hoff - 0x168) % 8 == 0 and (soff - 0x568) % 8 == 0:
                    nvn = {'kind': 'separate', 'index': (hoff - 0x168) // 8, 'sampler_index': (soff - 0x568) // 8}
                else:
                    nvn = {'kind': 'raw'}
            self.manifest_tex.append({
                'binding': binding, 'dim': dim + ('_array' if arr else ''), 'shadow': bool(dc),
                'array_len': 1, 'handle': {'cbuf': h[0], 'offset': hoff},
                'sampler_handle': ({'cbuf': s[0], 'offset': soff} if s else None), 'nvn': nvn})
            binding += 1

    def c1_array(self):
        if self.c1var is None:
            m = self.m
            data = self.p.consts or b'\0\0\0\0'
            n = max(1, len(data) // 4)
            words = struct.unpack_from('<%dI' % n, data, 0)
            arr = m.t_array(self.T_u32, n)
            init = m.const_composite(arr, *[self.cu(w) for w in words])
            self.c1var = m.gvar(m.t_ptr('Private', arr), 'Private', init)
            m.name(self.c1var, 'c1')
            self.c1len = n
        return self.c1var

    def local_array(self):
        if self.localvar is None:
            n = self.info.local_words
            if getattr(self.info, 'local_dynamic', False):
                sph = self.p.sph
                n = max(n, ((sph[1] & 0xFFFFFF) + 3) // 4)
            n = max(1, n)
            self.local_len = n
            arr = self.m.t_array(self.T_u32, n)
            self.localvar = self.fvar(arr)
            self.m.name(self.localvar, 'lmem')
        return self.localvar

    # ---- op generation
    def gen_block(self, b, live):
        m = self.m
        for o in b.ops:
            if o.n not in live:
                continue
            op = o.op
            r = None
            if op == 'const':
                if o.ty == 'bool':
                    r = m.const_bool(o.attr)
                else:
                    r = m.const(self.TY[o.ty], o.attr & 0xFFFFFFFF)
            elif op == 'rget':
                r = self.new('Load', self.T_u32, self.regvar(o.attr))
            elif op == 'rset':
                self.ins('Store', self.regvar(o.attr), self.get(o.args[0], 'u32'))
            elif op == 'pget':
                r = self.new('Load', self.T_bool, self.predvar(o.attr))
            elif op == 'pset':
                self.ins('Store', self.predvar(o.attr), self.get(o.args[0], 'bool'))
            elif op in ('bitcast_f', 'bitcast_u'):
                want = 'f32' if op == 'bitcast_f' else 'u32'
                src = o.args[0]
                r = self.v[src.n] if src.ty == want else self.new('Bitcast', self.TY[want], self.v[src.n])
            elif op == 'select':
                t = o.ty
                r = self.new('Select', self.TY[t], self.get(o.args[0], 'bool'), self.get(o.args[1], t),
                             self.get(o.args[2], t))
            elif op in ('fadd', 'fmul', 'fsub'):
                name = {'fadd': 'FAdd', 'fmul': 'FMul', 'fsub': 'FSub'}[op]
                r = self.new(name, self.T_f32, self.get(o.args[0], 'f32'), self.get(o.args[1], 'f32'))
                self.noc.append(r)
            elif op == 'ffma':
                r = self.ext(self.T_f32, 'Fma', *[self.get(a, 'f32') for a in o.args])
            elif op == 'fneg':
                r = self.new('FNegate', self.T_f32, self.get(o.args[0], 'f32'))
            elif op in EXT_F1:
                r = self.ext(self.T_f32, EXT_F1[op], self.get(o.args[0], 'f32'))
            elif op == 'frcp':
                r = self.new('FDiv', self.T_f32, self.cf(1.0), self.get(o.args[0], 'f32'))
            elif op in ('fmin', 'fmax'):
                r = self.ext(self.T_f32, 'NMin' if op == 'fmin' else 'NMax', self.get(o.args[0], 'f32'),
                             self.get(o.args[1], 'f32'))
            elif op == 'fcmp':
                a, c = self.get(o.args[0], 'f32'), self.get(o.args[1], 'f32')
                code = o.attr
                if code in FCMP:
                    r = self.new(FCMP[code], self.T_bool, a, c)
                elif code in (7, 8):
                    na = self.new('IsNan', self.T_bool, a)
                    nc = self.new('IsNan', self.T_bool, c)
                    anyn = self.new('LogicalOr', self.T_bool, na, nc)
                    r = anyn if code == 8 else self.new('LogicalNot', self.T_bool, anyn)
                elif code == 0:
                    r = m.const_bool(False)
                elif code == 15:
                    r = m.const_bool(True)
                else:
                    raise EmitError('fcmp %d' % code)
            elif op == 'icmp':
                r = self.new(ICMP[o.attr], self.T_bool, self.get(o.args[0], 'u32'), self.get(o.args[1], 'u32'))
            elif op in ('land', 'lor', 'lxor'):
                name = {'land': 'LogicalAnd', 'lor': 'LogicalOr', 'lxor': 'LogicalNotEqual'}[op]
                r = self.new(name, self.T_bool, self.get(o.args[0], 'bool'), self.get(o.args[1], 'bool'))
            elif op == 'lnot':
                r = self.new('LogicalNot', self.T_bool, self.get(o.args[0], 'bool'))
            elif op in BIN_U:
                r = self.new(BIN_U[op], self.T_u32, self.get(o.args[0], 'u32'), self.get(o.args[1], 'u32'))
            elif op in EXT_U2:
                r = self.ext(self.T_u32, EXT_U2[op], self.get(o.args[0], 'u32'), self.get(o.args[1], 'u32'))
            elif op == 'not':
                r = self.new('Not', self.T_u32, self.get(o.args[0], 'u32'))
            elif op == 'ineg':
                r = self.new('SNegate', self.T_u32, self.get(o.args[0], 'u32'))
            elif op == 'sabs':
                r = self.ext(self.T_u32, 'SAbs', self.get(o.args[0], 'u32'))
            elif op in ('bfe_u', 'bfe_s'):
                r = self.new('BitFieldUExtract' if op == 'bfe_u' else 'BitFieldSExtract', self.T_u32,
                             *[self.get(a, 'u32') for a in o.args])
            elif op == 'bfi':
                r = self.new('BitFieldInsert', self.T_u32, *[self.get(a, 'u32') for a in o.args])
            elif op == 'popc':
                r = self.new('BitCount', self.T_u32, self.get(o.args[0], 'u32'))
            elif op in ('findmsb_u', 'findmsb_s'):
                r = self.ext(self.T_u32, 'FindUMsb' if op == 'findmsb_u' else 'FindSMsb', self.get(o.args[0], 'u32'))
            elif op in ('u2f', 's2f'):
                r = self.new('ConvertUToF' if op == 'u2f' else 'ConvertSToF', self.T_f32, self.get(o.args[0], 'u32'))
            elif op in ('f2u', 'f2s'):
                if op == 'f2u':
                    r = self.new('ConvertFToU', self.T_u32, self.get(o.args[0], 'f32'))
                else:
                    t = self.new('ConvertFToS', self.T_i32, self.get(o.args[0], 'f32'))
                    r = self.new('Bitcast', self.T_u32, t)
            elif op == 'f2h_pack':
                vec = self.new('CompositeConstruct', self.T_v2f, self.get(o.args[0], 'f32'), self.get(o.args[1], 'f32'))
                r = self.ext(self.T_u32, 'PackHalf2x16', vec)
            elif op == 'h2f_lo':
                vec = self.ext(self.T_v2f, 'UnpackHalf2x16', self.get(o.args[0], 'u32'))
                r = self.new('CompositeExtract', self.T_f32, vec, 0)
            elif op == 'extract':
                r = self.new('CompositeExtract', self.T_f32, self.v[o.args[0].n], o.attr)
            elif op == 'cbuf':
                r = self.gen_cbuf(o)
            elif op == 'c1_dyn':
                var = self.c1_array()
                idx = self.new('ShiftRightLogical', self.T_u32, self.get(o.args[0], 'u32'), self.cu(2))
                idx = self.ext(self.T_u32, 'UMin', idx, self.cu(self.c1len - 1))
                ptr = self.new('AccessChain', m.t_ptr('Private', self.T_u32), var, idx)
                r = self.new('Load', self.T_u32, ptr)
            elif op == 'in':
                r = self.gen_in(o)
            elif op == 'builtin':
                r = self.gen_builtin(o)
            elif op == 'out':
                self.gen_out(o)
            elif op in ('ldl', 'stl'):
                var = self.local_array()
                idx = self.get(o.args[0], 'u32')
                if o.args[0].op != 'const':
                    idx = self.ext(self.T_u32, 'UMin', idx, self.cu(self.local_len - 1))
                elif o.args[0].attr >= self.local_len:
                    raise EmitError('local index out of range')
                ptr = self.new('AccessChain', m.t_ptr('Function', self.T_u32), var, idx)
                if op == 'ldl':
                    r = self.new('Load', self.T_u32, ptr)
                else:
                    self.ins('Store', ptr, self.get(o.args[1], 'u32'))
            elif op == 'tex':
                r = self.gen_tex(o)
            elif op == 'quad_bcast':
                m.cap('GroupNonUniform')
                m.cap('GroupNonUniformQuad')
                r = self.new('GroupNonUniformQuadBroadcast', self.T_u32, self.cu(SCOPE_SUBGROUP),
                             self.get(o.args[0], 'u32'), self.cu(o.attr))
            elif op == 'fswzadd':
                r = self.gen_fswzadd(o)
            elif op == 'demote':
                m.cap('DemoteToHelperInvocation')
                m.ext('SPV_EXT_demote_to_helper_invocation')
                self.ins('DemoteToHelperInvocation')
            else:
                raise EmitError('no emitter for IR op %s' % op)
            if r is not None:
                self.v[o.n] = r

    def gen_cbuf(self, o):
        m = self.m
        var = self.ubos[o.attr]
        off = o.args[0]
        pu = m.t_ptr('Uniform', self.T_u32)
        if off.op == 'const':
            a = off.attr
            if a & 3:
                raise EmitError('unaligned constant buffer read')
            ptr = self.new('AccessChain', pu, var, self.cu(0), self.cu(a >> 4), self.cu((a >> 2) & 3))
        else:
            ov = self.get(off, 'u32')
            vi = self.new('ShiftRightLogical', self.T_u32, ov, self.cu(4))
            vi = self.new('BitwiseAnd', self.T_u32, vi, self.cu(0xFFF))
            ci = self.new('BitwiseAnd', self.T_u32, self.new('ShiftRightLogical', self.T_u32, ov, self.cu(2)), self.cu(3))
            ptr = self.new('AccessChain', pu, var, self.cu(0), vi, ci)
        return self.new('Load', self.T_u32, ptr)

    def gen_in(self, o):
        m = self.m
        kind, idx, comp = o.attr
        if kind == 'gen':
            var = self.input_var('gen', idx)
            ptr = self.new('AccessChain', m.t_ptr('Input', self.T_f32), var, self.cu(comp))
            return self.new('Load', self.T_f32, ptr)
        if kind == 'frag_coord':
            var = self.input_var('frag_coord', 0)
            ptr = self.new('AccessChain', m.t_ptr('Input', self.T_f32), var, self.cu(comp))
            return self.new('Load', self.T_f32, ptr)
        if kind == 'point_coord':
            var = self.input_var('point_coord', 0)
            ptr = self.new('AccessChain', m.t_ptr('Input', self.T_f32), var, self.cu(comp))
            return self.new('Load', self.T_f32, ptr)
        if kind == 'front_facing':
            return self.new('Load', self.T_bool, self.input_var('front_facing', 0))
        if kind in ('vertex_id', 'instance_id'):
            t = self.new('Load', self.T_i32, self.input_var(kind, 0))
            return self.new('Bitcast', self.T_u32, t)
        raise EmitError('input %s' % kind)

    def gen_builtin(self, o):
        if o.attr == 'lane_id':
            self.uses_lane = True
            return self.new('Load', self.T_u32, self.input_var('lane_id', 0))
        raise EmitError('builtin %s' % o.attr)

    def gen_out(self, o):
        m = self.m
        kind, idx, comp = o.attr
        val = o.args[0]
        if kind in ('gen', 'frag', 'position'):
            var = self.output_var(kind, idx)
            ptr = self.new('AccessChain', m.t_ptr('Output', self.T_f32), var, self.cu(comp))
            self.ins('Store', ptr, self.get(val, 'f32'))
        elif kind in ('depth', 'point_size'):
            self.ins('Store', self.output_var(kind, 0), self.get(val, 'f32'))
        elif kind == 'clip':
            var = self.output_var('clip', 0)
            ptr = self.new('AccessChain', m.t_ptr('Output', self.T_f32), var, self.cu(idx))
            self.ins('Store', ptr, self.get(val, 'f32'))
        elif kind in ('layer', 'viewport'):
            iv = self.new('Bitcast', self.T_i32, self.get(val, 'u32'))
            self.ins('Store', self.output_var(kind, 0), iv)
        else:
            raise EmitError('output %s' % kind)

    def gen_fswzadd(self, o):
        m = self.m
        m.cap('GroupNonUniform')
        a = self.get(o.args[0], 'f32')
        b = self.get(o.args[1], 'f32')
        lane = self.new('Load', self.T_u32, self.input_var('lane_id', 0))
        lane = self.new('BitwiseAnd', self.T_u32, lane, self.cu(3))
        sh = self.new('ShiftLeftLogical', self.T_u32, lane, self.cu(1))
        sel = self.new('BitwiseAnd', self.T_u32, self.new('ShiftRightLogical', self.T_u32, self.cu(o.attr), sh), self.cu(3))
        is1 = self.new('IEqual', self.T_bool, sel, self.cu(1))
        is2 = self.new('IEqual', self.T_bool, sel, self.cu(2))
        is3 = self.new('IEqual', self.T_bool, sel, self.cu(3))
        na = self.new('FNegate', self.T_f32, a)
        nb = self.new('FNegate', self.T_f32, b)
        av = self.new('Select', self.T_f32, is1, na, a)
        av = self.new('Select', self.T_f32, is3, self.cf(0.0), av)
        bv = self.new('Select', self.T_f32, is2, nb, b)
        r = self.new('FAdd', self.T_f32, av, bv)
        self.noc.append(r)
        return r

    def gen_tex(self, o):
        m = self.m
        at = o.attr
        key = at['tkey']['key']
        var, img, simg = self.textures[key]
        dim, arr, dc, kind = at['dim'], at['arr'], at['dc'], at['kind']
        nc = at['ncoord']
        args = o.args[1:]
        coords = args[:nc]
        extra = list(args[nc:])
        arr_idx = extra.pop(0) if arr else None
        lodv = extra.pop(0) if at['has_lod'] else None
        offv = extra.pop(0) if at['has_off'] else None
        dref = extra.pop(0) if at['has_dref'] else None
        si = self.new('Load', simg, var)
        constoff = None
        if offv is not None:
            if offv.op != 'const':
                raise EmitError('texture offset not constant')
            if dim == 'cube':
                raise EmitError('offset on cube texture')
            ofs = [_sext4((offv.attr >> (4 * k)) & 0xF) for k in range(nc)]
            if any(ofs):
                if nc == 1:
                    constoff = m.const_i32(ofs[0])
                else:
                    constoff = m.const_composite(self.T_ivec[nc], *[m.const_i32(x) for x in ofs])
        nall = nc + (1 if arr else 0)
        if kind == 'tld':
            comps = [self.new('Bitcast', self.T_i32, self.get(c, 'u32')) for c in coords]
            if arr:
                comps.append(self.new('Bitcast', self.T_i32, self.get(arr_idx, 'u32')))
            coord = comps[0] if nall == 1 else self.new('CompositeConstruct', self.T_ivec[nall], *comps)
            image = self.new('Image', img, si)
            lod = self.new('Bitcast', self.T_i32, self.get(lodv, 'u32')) if lodv is not None else m.const_i32(0)
            mask = IMG_LOD
            ops = [lod]
            if constoff is not None:
                mask |= IMG_CONSTOFF
                ops.append(constoff)
            return self.new('ImageFetch', self.T_v4f, image, coord, mask, *ops)
        comps = [self.get(c, 'f32') for c in coords]
        if arr:
            ai = self.get(arr_idx, 'u32')
            ai = self.new('BitwiseAnd', self.T_u32, ai, self.cu(0xFFFF))
            comps.append(self.new('ConvertUToF', self.T_f32, ai))
        coord = comps[0] if nall == 1 else self.new('CompositeConstruct', self.T_fvec[nall], *comps)
        if kind == 'tld4':
            if constoff is not None:
                m.cap('ImageGatherExtended')
            tail = [] if constoff is None else [IMG_CONSTOFF, constoff]
            if dc:
                return self.new('ImageDrefGather', self.T_v4f, si, coord, self.get(dref, 'f32'), *tail)
            return self.new('ImageGather', self.T_v4f, si, coord, self.cu(at['comp']), *tail)
        lodmode = at['lod']
        implicit = self.stage == 'fs' and lodmode in (0, 2)
        mask = 0
        ops = []
        if implicit:
            if lodmode == 2:
                mask |= IMG_BIAS
                ops.append(self.get(lodv, 'f32'))
        else:
            mask |= IMG_LOD
            ops.append(self.get(lodv, 'f32') if lodmode in (2, 3) else self.cf(0.0))
        if constoff is not None:
            mask |= IMG_CONSTOFF
            ops.append(constoff)
        tail = [mask] + ops if mask else []
        if dc:
            d = self.get(dref, 'f32')
            name = 'ImageSampleDrefImplicitLod' if implicit else 'ImageSampleDrefExplicitLod'
            s = self.new(name, self.T_f32, si, coord, d, *tail)
            return self.new('CompositeConstruct', self.T_v4f, s, s, s, s)
        name = 'ImageSampleImplicitLod' if implicit else 'ImageSampleExplicitLod'
        return self.new(name, self.T_v4f, si, coord, *tail)

    # ---- control flow
    def label(self, b):
        lb = self.labels.get(b.id)
        if lb is None:
            lb = self.labels[b.id] = self.m.id()
        return lb

    def cond_id(self, b):
        return self.get(b.term[1], 'bool')

    def emit_structured(self, blocks, live):
        loops = find_loops(blocks)
        hl_of = {h: self.m.id() for h in loops}

        def psucc(b):
            # post-dominance graph: a back edge counts as an edge to its loop's exit block
            out = []
            for t in b.succs():
                if t.id in loops and b.id in loops[t.id][0]:
                    if loops[t.id][1] is not None:
                        out.append(loops[t.id][1])
                else:
                    out.append(t)
            return out
        ipd = ipdoms(blocks, psucc)
        emitted = set()

        def enter(x):
            return hl_of[x.id] if x.id in loops else self.label(x)

        def special(x, stop, stop_lbl, exits):
            if x is stop:
                return stop_lbl
            return exits.get(x.id)

        def open_loop(h, stop, stop_lbl, exits):
            body, mb = loops[h.id]
            cl = self.m.id()
            fresh = mb is None or mb is stop or mb.id in exits
            ml = self.m.id() if fresh else self.label(mb)
            self.ins('Label', hl_of[h.id])
            self.ins('LoopMerge', ml, cl, 0)
            self.ins('Branch', self.label(h))
            region(h, mb, ml, {h.id: cl}, body, True)
            self.ins('Label', cl)
            self.ins('Branch', hl_of[h.id])
            if fresh:
                self.ins('Label', ml)
                if mb is None:
                    self.ins('Unreachable')
                else:
                    self.ins('Branch', special(mb, stop, stop_lbl, exits))
                return None
            return mb

        def region(b, stop, stop_lbl, exits, inside=None, in_loop_head=False):
            while True:
                if b is stop:
                    self.ins('Branch', stop_lbl)
                    return
                if b.id in loops and not in_loop_head:
                    if b.id in emitted:
                        raise Fallback('loop entered twice')
                    b = open_loop(b, stop, stop_lbl, exits)
                    if b is None:
                        return
                    continue
                in_loop_head = False
                if b.id in emitted:
                    raise Fallback('block reached twice')
                if inside is not None and b.id not in inside:
                    raise Fallback('loop body leaves its loop')
                emitted.add(b.id)
                self.ins('Label', self.label(b))
                self.gen_block(b, live)
                t = b.term
                if t[0] == 'ret':
                    self.ins('Return')
                    return
                if t[0] == 'cbr' and t[2] is t[3]:
                    t = ('br', t[2])
                if t[0] == 'br':
                    nxt = t[1]
                    sl = special(nxt, stop, stop_lbl, exits)
                    if sl is not None:
                        self.ins('Branch', sl)
                        return
                    self.ins('Branch', enter(nxt))
                    b = nxt
                    continue
                if t[0] != 'cbr':
                    raise Fallback('terminator %s' % t[0])
                c = self.cond_id(b)
                tb, fb = t[2], t[3]
                mb = ipd[b.id]
                if mb is not None and mb is not stop:
                    if mb.id in exits or (inside is not None and mb.id not in inside):
                        raise Fallback('selection merge outside its construct')
                if mb is None or mb is stop:
                    ml = self.m.id()

                    def tgt(x):
                        if x is stop:
                            return ml
                        return special(x, stop, stop_lbl, exits) or enter(x)
                    self.ins('SelectionMerge', ml, 0)
                    self.ins('BranchConditional', c, tgt(tb), tgt(fb))
                    for arm in (tb, fb):
                        if special(arm, stop, stop_lbl, exits) is None:
                            region(arm, stop, ml, exits, inside)
                    self.ins('Label', ml)
                    if stop is None:
                        self.ins('Unreachable')
                    else:
                        self.ins('Branch', stop_lbl)
                    return
                ml = self.label(mb)

                def tgt2(x):
                    if x is mb:
                        return ml
                    return special(x, stop, stop_lbl, exits) or enter(x)
                self.ins('SelectionMerge', ml, 0)
                self.ins('BranchConditional', c, tgt2(tb), tgt2(fb))
                for arm in (tb, fb):
                    if arm is not mb and special(arm, stop, stop_lbl, exits) is None:
                        region(arm, mb, ml, exits, inside)
                b = mb

        self.ins('Branch', enter(blocks[0]))
        region(blocks[0], None, None, {})

    def emit_dispatcher(self, blocks, live):
        m = self.m
        state = self.fvar(self.T_u32)
        m.name(state, 'pc')
        H, D, SW, C, M = (m.id() for _ in range(5))
        self.ins('Store', state, self.cu(blocks[0].id))
        self.ins('Branch', H)
        self.ins('Label', H)
        self.ins('LoopMerge', M, C, 0)
        self.ins('Branch', D)
        self.ins('Label', D)
        s = self.new('Load', self.T_u32, state)
        self.ins('SelectionMerge', SW, 0)
        cases = []
        for b in blocks:
            cases += [b.id, self.label(b)]
        self.ins('Switch', s, SW, *cases)
        for b in blocks:
            self.ins('Label', self.label(b))
            self.gen_block(b, live)
            t = b.term
            if t[0] == 'ret':
                nxt = self.cu(EXIT_STATE)
            elif t[0] == 'br':
                nxt = self.cu(t[1].id)
            elif t[0] == 'cbr':
                nxt = self.new('Select', self.T_u32, self.cond_id(b), self.cu(t[2].id), self.cu(t[3].id))
            else:
                raise EmitError('terminator %s' % t[0])
            self.ins('Store', state, nxt)
            self.ins('Branch', SW)
        self.ins('Label', SW)
        self.ins('Branch', C)
        self.ins('Label', C)
        s2 = self.new('Load', self.T_u32, state)
        go = self.new('INotEqual', self.T_bool, s2, self.cu(EXIT_STATE))
        self.ins('BranchConditional', go, H, M)
        self.ins('Label', M)
        self.ins('Return')

    # ---- driver
    def run(self):
        blocks, live, _ = analyze(self.fn)
        self.blocks = blocks
        self.plan_resources(blocks, live)
        m = self.m
        self.labels = {}
        self.code = []
        mode = self.mode
        self.fallback_reason = None
        if mode == 'structured':
            try:
                self.emit_structured(blocks, live)
            except Fallback as e:
                mode = 'dispatcher'
                self.fallback_reason = str(e)
        if mode == 'dispatcher':
            undo = getattr(self.fn, 'fold_undo', None)
            if undo:
                # half_fold cross-block values cannot dominate their uses inside the dispatcher switch
                for o, args in undo:
                    o.args = args
                blocks, live, _ = analyze(self.fn)
                self.blocks = blocks
            # restart with a clean function body (module-level ids already allocated stay unused)
            self.code = []
            self.labels = {}
            self.v = {}
            self.noc = []
            self.emit_dispatcher(blocks, live)
        self.cf_mode = mode
        for r in self.noc:
            m.deco(r, 'NoContraction')
        fn_t = m.t_func(self.T_void)
        fid = m.id()
        l0 = m.id()
        words = []
        Module.ins(words, 'Function', self.T_void, fid, 0, fn_t)
        Module.ins(words, 'Label', l0)
        words += self.fvars
        words += self.code
        Module.ins(words, 'FunctionEnd')
        m.funcs.append(words)
        Module.ins(m.entry, 'EntryPoint', self._exec(), fid, *m.string('main'), *self.interface)
        if self.stage == 'fs':
            Module.ins(m.modes, 'ExecutionMode', fid, 7)            # OriginUpperLeft
            if 'depth' in [k[0] for k in self.outputs]:
                Module.ins(m.modes, 'ExecutionMode', fid, 12)       # DepthReplacing
        m.name(fid, 'main')
        return m.assemble()

    def _exec(self):
        return {'Vertex': 0, 'TessellationControl': 1, 'TessellationEvaluation': 2, 'Geometry': 3,
                'Fragment': 4, 'GLCompute': 5}[EXEC_OF_STAGE[self.stage]]

    def manifest(self, key):
        info = self.info
        ins = []
        for (kind, idx), _ in sorted(self.inputs.items(), key=lambda kv: str(kv[0])):
            if kind == 'gen':
                comps = ''.join('xyzw'[c] for c in sorted(info.inputs.get(idx, ())))
                interp = {1: 'flat', 3: 'noperspective'}.get(info.input_interp.get(idx, 2), 'smooth') \
                    if self.stage == 'fs' else None
                d = {'location': idx, 'attr': idx, 'components': comps}
                if interp:
                    d['interp'] = interp
                ins.append(d)
        outs = []
        bout = []
        for (kind, idx) in sorted(self.outputs, key=str):
            if kind in ('gen', 'frag'):
                comps = ''.join('xyzw'[c] for c in sorted(info.outputs.get(idx, ())))
                outs.append({'location': idx, 'components': comps})
            elif kind == 'clip':
                bout.append('clip_distance[%d]' % max(1, info.clip_written.bit_length()))
            else:
                bout.append({'depth': 'frag_depth', 'viewport': 'viewport_index'}.get(kind, kind))
        bin_ = sorted(k for (k, _) in self.inputs if k != 'gen')
        d = {'key': key, 'stage': self.stage, 'spirv_version': '1.3', 'sets': {'set': self.set},
             'ubos': self.manifest_ubo, 'textures': self.manifest_tex, 'images': [], 'ssbos': [],
             'inputs': ins, 'outputs': outs, 'builtins_in': bin_, 'builtins_out': bout,
             'local_size': [1, 1, 1], 'shared_bytes': 0,
             'local_bytes': 4 * getattr(self, 'local_len', 0),
             'requires': {'subgroup_size_32': self.uses_lane and not self._lane_quad_only(),
                          'fragment_shader_interlock': False, 'geometry_passthrough': False},
             'constants_inlined': True, 'cf_mode': self.cf_mode,
             'cf_fallback': self.fallback_reason}
        if self.stage == 'fs':
            d['frag'] = {'rt_mask': info.rt_mask, 'writes_depth': info.writes_depth,
                         'early_fragment_tests': False, 'discards': info.demotes}
        return d

    def _lane_quad_only(self):
        return False


def emit(fn, info, prog, mode='structured'):
    e = Emitter(fn, info, prog, mode)
    spv = e.run()
    return spv, e
