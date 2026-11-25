"""Shader IR (our own): basic blocks of typed operations.

Registers (R0..R254, P0..P6) live in function variables; values never cross blocks except
through 'rget'/'rset' and 'pget'/'pset', so every block is self-contained SSA. That keeps the
emitter simple and lets the driver do register promotion.

Types: 'u32', 'f32', 'bool', 'v4f' (texture result), 'void'.
An Op is a value (or a statement when ty == 'void'). args hold Ops; attr holds plain data.
"""


class Op:
    __slots__ = ('op', 'ty', 'args', 'attr', 'sym', 'n', 'halves', 'psel')

    def __init__(self, op, ty, args=(), attr=None):
        self.op = op
        self.ty = ty
        self.args = list(args)
        self.attr = attr
        self.sym = None      # symbolic uniform expression (handles, addresses), filled by lifter
        self.n = 0

    def __repr__(self):
        a = ','.join('%%%d' % x.n if isinstance(x, Op) else repr(x) for x in self.args)
        return '%%%d = %s.%s(%s)%s' % (self.n, self.op, self.ty, a, (' ' + repr(self.attr)) if self.attr is not None else '')


class Block:
    __slots__ = ('id', 'ops', 'term', 'addr', 'preds')

    def __init__(self, bid, addr=None):
        self.id = bid
        self.ops = []
        self.term = None     # ('br', b) | ('cbr', cond, t, f) | ('ret',) | ('kill',) | ('switch', v, [(k, b)], default)
        self.addr = addr
        self.preds = []

    def succs(self):
        t = self.term
        if t is None:
            return []
        if t[0] == 'br':
            return [t[1]]
        if t[0] == 'cbr':
            return [t[2], t[3]]
        if t[0] == 'switch':
            return [b for _, b in t[2]] + [t[3]]
        return []


class Function:
    def __init__(self):
        self.blocks = []
        self.entry = None
        self.fold_undo = None   # half_fold oracle mirror only (lift.fold_halves_across_blocks)

    def new_block(self, addr=None):
        b = Block(len(self.blocks), addr)
        self.blocks.append(b)
        return b

    def compute_preds(self):
        for b in self.blocks:
            b.preds = []
        for b in self.blocks:
            for s in b.succs():
                s.preds.append(b)

    def reachable(self):
        seen = set()
        order = []
        st = [self.entry]
        while st:
            b = st.pop()
            if b.id in seen:
                continue
            seen.add(b.id)
            order.append(b)
            st.extend(reversed(b.succs()))
        return order

    def dump(self):
        out = []
        for b in self.blocks:
            out.append('B%d (addr %s):' % (b.id, hex(b.addr) if b.addr is not None else '-'))
            for o in b.ops:
                out.append('  ' + repr(o))
            t = b.term
            out.append('  -> ' + (repr(tuple(x.id if isinstance(x, Block) else (('%%%d' % x.n) if isinstance(x, Op) else x) for x in t)) if t else 'None'))
        return '\n'.join(out)
