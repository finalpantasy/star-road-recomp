"""Tests for tools/shaderxlate (task-10). Synthetic inputs only; no game data is needed or stored.

    python -I tests/shaderxlate/test_shaderxlate.py --report out.json [--corpus A:/AI/Recomps/smrpg-work/shaders]

With --corpus, also translates a fixed slice of the owner's local corpus and validates it (skipped when absent).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'shaderxlate'))

import decoder     # noqa: E402
import lift        # noqa: E402
import emit        # noqa: E402
import translate   # noqa: E402
import container   # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


# ---------------------------------------------------------------- tiny assembler for test programs

def enc(name, form, **f):
    """Encode an instruction from the decoder table plus field values {bitpos: value}."""
    row = [r for r in decoder.ROWS if r[3] == name and r[4] == form]
    assert row, (name, form)
    w = row[0][2]
    for pos, (val, width) in f.items():
        pos = int(pos[1:])
        w |= (val & ((1 << width) - 1)) << pos
    return w


def ins(name, form='-', rd=0xFF, ra=0xFF, rb=None, pred=7, neg=0, **f):
    w = enc(name, form, **f)
    w |= rd | (ra << 8) | (pred << 16) | (neg << 19)
    if rb is not None:
        w |= rb << 20
    return w


def mov32i(rd, bits):
    return ins('mov32i', 'I32', rd=rd, ra=0, b20=(bits, 32), b12=(0xF, 4))


def fadd_r(rd, ra, rb):
    return ins('fadd', 'R', rd=rd, ra=ra, rb=rb)


def exit_():
    return enc('exit', '-') | 0xF | (7 << 16)


def bra(rel):
    return enc('bra', '-') | 0xF | (7 << 16) | ((rel & 0xFFFFFF) << 20)


def pack_code(words):
    """Insert a scheduling word before every 3 instructions."""
    out = []
    for k in range(0, len(words), 3):
        out.append(0x001F8000FC0007E0)
        grp = words[k:k + 3]
        while len(grp) < 3:
            grp.append(enc('nop', '-') | (0xF << 8))
        out += grp
    return struct.pack('<%dQ' % len(out), *out)


def fs_sph(rt_mask_comps=0xF, imap=None):
    w = [0] * 20
    w[0] = 2 | (3 << 5) | (5 << 10)       # PS, version 3, stage FS
    for i, b in (imap or {}).items():
        w[6 + (i >> 2)] |= b << ((i & 3) * 8)
    w[18] = rt_mask_comps
    return struct.pack('<20I', *w)


def vs_sph():
    w = [0] * 20
    w[0] = 1 | (3 << 5) | (1 << 10)
    return struct.pack('<20I', *w)


def build(stage, code, sph, consts=b''):
    return lift.Program(stage, code, sph, consts, 'test')


def spv_ok(spv):
    if not os.path.exists(translate.SPIRV_VAL):
        return None, 'spirv-val not found'
    with tempfile.NamedTemporaryFile(suffix='.spv', delete=False) as f:
        f.write(spv)
    try:
        return translate.validate(f.name)
    finally:
        os.remove(f.name)


def full(prog, mode='structured'):
    fn, info = lift.lift(prog)
    spv, em = emit.emit(fn, info, prog, mode)
    return fn, info, spv, em


# ---------------------------------------------------------------- tests

@test
def decoder_table_unique():
    """Every table row matches its own pattern and no two rows have identical patterns."""
    seen = set()
    for _, m, v, n, f in decoder.ROWS:
        assert (m, v) not in seen, n
        seen.add((m, v))
        assert decoder.match(v)[0] is not None


@test
def decoder_known_words():
    assert decoder.match(exit_()) == ('exit', '-')
    assert decoder.match(mov32i(1, 0x3F800000)) == ('mov32i', 'I32')
    assert decoder.match(fadd_r(0, 1, 2)) == ('fadd', 'R')
    assert decoder.match(enc('tex_b', '-'))[0] == 'tex_b'
    assert decoder.match(enc('votevtg', '-'))[0] == 'votevtg'


@test
def decoder_skips_sched_words():
    code = pack_code([mov32i(0, 1), exit_(), bra(-8)])
    insts = decoder.decode_code(code)
    assert [i.name for i in insts] == ['mov32i', 'exit', 'bra']
    assert [i.addr for i in insts] == [8, 16, 24]


@test
def container_entry_triples():
    """Chunk 0 starts with n then n (offset, length, segment) triples, offsets per segment."""
    seg1 = b'xxxxHELLO'
    head = struct.pack('<I6I', 2, 4 + 24, 3, 0, 4, 5, 1) + b'abc'
    got = container.entries([head, seg1])
    assert got == [b'abc', b'HELLO'], got


@test
def fs_constant_color_validates():
    code = pack_code([mov32i(0, 0x3F800000), mov32i(1, 0x3F000000), mov32i(2, 0), mov32i(3, 0x3F800000),
                      exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    assert info.rt_mask == 1
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def fs_add_two_inputs():
    """IPA reads, FADD, outputs; checks interpolation decoration and input bookkeeping."""
    ipa = lambda rd, a: ins('ipa', '-', rd=rd, ra=0xFF, rb=0xFF, b28=(a, 10), b39=(0xFF, 8))
    code = pack_code([ipa(4, 0x80), ipa(5, 0x84), fadd_r(0, 4, 5), mov32i(1, 0), mov32i(2, 0), mov32i(3, 0),
                      exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph(imap={0: 0x0A})))   # x,y perspective
    assert info.inputs == {0: {0, 1}}, info.inputs
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def vs_position_invariant():
    ast = lambda src, a, n: ins('ast', '-', rd=src, ra=0xFF, b20=(a, 11), b47=(n - 1, 2), b39=(0xFF, 8))
    code = pack_code([mov32i(0, 0), mov32i(1, 0), mov32i(2, 0), mov32i(3, 0x3F800000), ast(0, 0x70, 4),
                      exit_(), bra(-8)])
    fn, info, spv, em = full(build('vs', code, vs_sph()))
    assert 'position' in info.builtins_out
    # Invariant decoration (18) on the BuiltIn Position (0) variable
    words = struct.unpack('<%dI' % (len(spv) // 4), spv)
    i, decos = 5, {}
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        if op == 71:
            decos.setdefault(words[i + 1], []).append(tuple(words[i + 2:i + n]))
        i += n
    pos = [v for v, d in decos.items() if (11, 0) in d]
    assert pos and (18,) in decos[pos[0]], decos
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def vs_unwritten_outputs_default_w1():
    # Position and generic outputs start at (0, 0, 0, 1): a blit VS that writes only .xy (the game's
    # built-in MSAA resolve VS) must still give w = 1, or every primitive is clipped.
    ast = lambda src, a, n: ins('ast', '-', rd=src, ra=0xFF, b20=(a, 11), b47=(n - 1, 2), b39=(0xFF, 8))
    code = pack_code([mov32i(0, 0), mov32i(1, 0), ast(0, 0x70, 2), ast(0, 0x80, 2), exit_(), bra(-8)])
    fn, info, spv, em = full(build('vs', code, vs_sph()))
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    i, consts, comps, init, builtin, names = 5, {}, {}, {}, {}, {}
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if op == 43 and n == 4:
            consts[w[i + 2]] = w[i + 3]
        elif op == 44:
            comps[w[i + 2]] = w[i + 3:i + n]
        elif op == 59 and n == 5 and w[i + 3] == 3:
            init[w[i + 2]] = w[i + 4]
        elif op == 71 and w[i + 2] == 11:
            builtin[w[i + 1]] = w[i + 3]
        elif op == 71 and w[i + 2] == 30:
            names[w[i + 1]] = w[i + 3]
        i += n
    pos = [v for v, b in builtin.items() if b == 0 and v in init]
    gen = [v for v, l in names.items() if l == 0 and v in init]
    assert pos and gen, (builtin, names, init)
    for v in (pos[0], gen[0]):
        parts = comps[init[v]]
        assert [consts[c] for c in parts] == [0, 0, 0, 0x3F800000], parts
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def vtg_cull_exit_not_taken():
    """VOTE.VTG then EXIT with a condition-code test: a cull hint, never an unconditional exit."""
    ast = lambda src, a, n: ins('ast', '-', rd=src, ra=0xFF, b20=(a, 11), b47=(n - 1, 2), b39=(0xFF, 8))
    cc_exit = enc('exit', '-') | 0x1C | (7 << 16)
    code = pack_code([enc('votevtg', '-'), cc_exit, mov32i(3, 0x3F800000), ast(0, 0x70, 4), exit_(), bra(-8)])
    fn, info, spv, em = full(build('vs', code, vs_sph()))
    outs = [o for b in fn.blocks for o in b.ops if o.op == 'out']
    assert len(outs) == 4, 'position store must survive the cull exit'


@test
def branch_target_on_sched_word():
    """A branch whose target lands on a scheduling word goes to the first instruction of that bundle."""
    # bundle 0: sched, [bra +X, mov, mov]; bundle 1: sched, [exit, bra self, nop]
    code = pack_code([bra(8 * 2), mov32i(0, 0), mov32i(1, 0), exit_(), bra(-8)])
    insts = decoder.decode_code(code)
    lf = lift.Lifter(build('vs', code, vs_sph()))
    assert lf.branch_target(insts[0]) == 0x28, hex(lf.branch_target(insts[0]))


@test
def loop_is_structured():
    """A counted loop (backward BRA) is emitted as a structured OpLoopMerge, not the dispatcher."""
    isetp = ins('isetp', 'I', rd=(0 << 3) | 7, ra=1, b20=(4, 19), b49=(1, 3), b39=(7, 3))   # P0 = R1 < 4
    iadd = ins('iadd32i', 'I32', rd=1, ra=1, b20=(1, 32))
    braP = lambda rel: enc('bra', '-') | 0xF | (0 << 16) | ((rel & 0xFFFFFF) << 20)
    # 0x08 mov R1=0 ; 0x10 iadd ; 0x18 isetp ; 0x28 @P0 bra 0x10 ; 0x30 mov R0.. ; exit
    code = pack_code([mov32i(1, 0), iadd, isetp, braP(0x10 - 0x30), mov32i(0, 0), mov32i(2, 0),
                      mov32i(3, 0), exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph(0x1)))
    assert em.cf_mode == 'structured', em.fallback_reason
    words = struct.unpack('<%dI' % (len(spv) // 4), spv)
    assert any((w & 0xFFFF) == 246 for w in words)     # OpLoopMerge present
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def dispatcher_fallback_validates():
    code = pack_code([mov32i(0, 0x3F800000), mov32i(1, 0), mov32i(2, 0), mov32i(3, 0), exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()), mode='dispatcher')
    assert em.cf_mode == 'dispatcher'
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def bindless_handle_or_traced():
    """TEX.B with handle = c[2][0x168] | c[2][0x568] maps to a separate texture r=0 and sampler s=0."""
    mov_c = ins('mov', 'C', rd=8, ra=0, b20=(0x168 >> 2, 14), b34=(2, 5), b39=(0xF, 4))
    lop_c = ins('lop', 'C', rd=8, ra=8, b20=(0x568 >> 2, 14), b34=(2, 5), b41=(1, 2), b48=(7, 3))
    tex = ins('tex_b', '-', rd=0, ra=4, rb=8, b28=(2, 3), b31=(0xF, 4))
    code = pack_code([mov_c, lop_c, mov32i(4, 0), mov32i(5, 0), tex, exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    man = em.manifest('k')
    t = man['textures']
    assert len(t) == 1 and t[0]['nvn'] == {'kind': 'separate', 'index': 0, 'sampler_index': 0}, t
    assert not man['ubos'], 'c[2] handle reads must not become a UBO'
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def unsupported_reports_reason():
    code = pack_code([enc('brx', '-') | (0xF << 0) | (7 << 16), exit_(), bra(-8)])
    try:
        lift.lift(build('fs', code, fs_sph()))
    except lift.Unsupported as e:
        assert 'BRX' in str(e)
    else:
        raise AssertionError('BRX must fail with a reason')


# ---------------------------------------------------------------- regressions found by the oracle (AC3)

def ops_of(fn):
    return [o for b in fn.blocks for o in b.ops]


def root(v):
    """Strip bitcasts and sign ops to find where an operand comes from."""
    while v.op in ('bitcast_f', 'bitcast_u', 'fneg', 'fabs'):
        v = v.args[0]
    return v


def lift_words(words, stage='fs', consts=b''):
    fn, info = lift.lift(build(stage, pack_code(list(words) + [exit_(), bra(-8)]), fs_sph(), consts))
    return fn


@test
def ffma_rc_takes_b_from_register():
    """FFMA RC form (0x51a): Rd = Ra * Rc + cbuf. B is the register at bits 39-46, C the cbuf."""
    w = 0x51a0061800270a03                 # FFMA R3, R10, R12, c[6][0x8]
    i = decoder.decode_code(pack_code([w]))[0]
    f = [o for o in ops_of(lift_words([w])) if o.op == 'ffma']
    assert len(f) == 1, f
    b, c = root(f[0].args[1]), root(f[0].args[2])
    assert b.op == 'rget' and b.attr == i.rc == 12, (b, i.rc)
    assert c.op == 'cbuf' and c.attr == 6, c


@test
def ffma_fmz_stays_fused():
    """FFMA.FMZ is one fused multiply-add plus the zero rule, never fmul then fadd."""
    w = ins('ffma', 'R', rd=1, ra=2, rb=3, b39=(4, 8), b53=(2, 2))     # FFMA.FMZ R1, R2, R3, R4
    names = [o.op for o in ops_of(lift_words([w]))]
    assert 'ffma' in names and 'fmul' not in names, names


@test
def hfma2_rc_operands():
    """HFMA2 RC form (0x60a): B is register Rc with the swizzle at bits 53-54, C the cbuf word as one f32."""
    w = 0x60a48b8400070c0c                 # HFMA2.MRG_H0 R12, R12, R23, c[1][0x0]
    i = decoder.decode_code(pack_code([w]))[0]
    f = [o for o in ops_of(lift_words([w], consts=struct.pack('<4f', 2.0, 0, 0, 0))) if o.op == 'ffma']
    assert len(f) == 2, f
    for x in f:
        b = x.args[1]
        while b.op in ('fneg', 'fabs', 'h2f_lo', 'shr', 'bitcast_f'):
            b = b.args[0]
        assert b.op == 'rget' and b.attr == i.rc, (b, i.rc)
        assert root(x.args[2]).op != 'h2f_lo', 'cbuf operand of HFMA2 RC is an f32, not a half pair'


@test
def mrg_h1_writes_lane_one():
    """HADD2.MRG_H1 puts the lane 1 result into H1 and keeps the old H0."""
    w = ins('hadd2', 'R', rd=0, ra=1, rb=2, b49=(3, 2))
    packs = [o for o in ops_of(lift_words([w])) if o.op == 'f2h_pack']
    assert len(packs) == 1, packs
    hi = packs[0].args[1]
    assert hi.op == 'fadd' and hi.args[0].op == 'h2f_lo' and hi.args[0].args[0].op == 'shr', hi


@test
def half_results_round_through_fp16():
    """A half result read back by the next half op goes through pack/unpack (fp16 rounding), as on hardware."""
    w1 = ins('hadd2', 'R', rd=0, ra=1, rb=2)                 # F16 pair output
    w2 = ins('hadd2', 'R', rd=3, ra=0, rb=2, b49=(1, 2))     # F32 output reading R0
    ops = ops_of(lift_words([w1, w2]))
    unpacks = [o for o in ops if o.op == 'h2f_lo' and o.args[0].op == 'f2h_pack']
    assert unpacks, 'R0 lane 0 must be unpacked from the packed fp16 value'


@test
def fmul_scale_codes():
    """FMUL scale field (bits 41-43): 1 D2, 2 D4, 3 D8, 4 M8, 5 M4, 6 M2."""
    want = {1: 0.5, 2: 0.25, 3: 0.125, 4: 8.0, 5: 4.0, 6: 2.0}
    for code, k in want.items():
        w = ins('fmul', 'R', rd=0, ra=1, rb=2, b41=(code, 3))
        consts = [root(a) for o in ops_of(lift_words([w])) if o.op == 'fmul' for a in o.args]
        vals = [c.attr for c in consts if c.op == 'const']
        assert k in vals or struct.unpack('<I', struct.pack('<f', k))[0] in vals, (code, vals)


@test
def iadd_carry_chain():
    """IADD.CC sets the carry predicate; IADD.X adds it back. Both lift and validate."""
    cc = ins('iadd', 'R', rd=0, ra=1, rb=2, b47=(1, 1))
    x = ins('iadd', 'R', rd=3, ra=4, rb=5, b43=(1, 1))
    fn, info, spv, em = full(build('fs', pack_code([cc, x, exit_(), bra(-8)]), fs_sph()))
    names = [o.op for o in ops_of(fn)]
    assert ('icmp', 'ult') in [(o.op, o.attr) for o in ops_of(fn)], names
    assert 'select' in names, names
    ok, msg = spv_ok(spv)
    assert ok is not False, msg



@test
def i2f_f16_result():
    """I2F.F16 converts in f32 and packs the result into H0 with H1 cleared."""
    w = ins('i2f', 'R', rd=0, ra=0, rb=1, b8=(1, 2), b10=(2, 2))
    packs = [o for o in ops_of(lift_words([w])) if o.op == 'f2h_pack']
    assert len(packs) == 1 and packs[0].args[0].op in ('u2f', 's2f'), packs


@test
def half_fold_crosses_untouched_loop():
    """Oracle mirror only: a packed lane read after a loop that never writes the register folds to the
    unrounded value (Ryubing's SSA has no phi there). Shipped output is not affected."""
    loop = (bra(-8) & ~(0xF << 16))                            # @P0 BRA to itself
    w0 = ins('hadd2', 'R', rd=0, ra=1, rb=2)                   # R0 = F16 pair
    w2 = ins('hadd2', 'R', rd=3, ra=0, rb=2, b49=(1, 2))       # F32 result reading R0
    had = 'half_fold' in lift.COMPAT
    lift.COMPAT.add('half_fold')
    try:
        fn = lift_words([w0, loop, w2])
    finally:
        if not had:
            lift.COMPAT.discard('half_fold')
    last = [o for o in ops_of(fn) if o.op == 'fadd'][-1]
    assert last.args[0].op == 'fadd', 'lane of R0 should fold to the first HADD2 sum, got %s' % last.args[0].op



@test
def fset_cc_drives_neu_branch():
    """FSET.CC sets Z and N from its result; BRA CC.NEU (test 13) branches on N or not Z."""
    fset = 0x588283800ff70aff                              # FSET.CC RZ, R10, RZ (seen in the corpus)
    br = (bra(8) & ~0x1F) | 13                              # BRA CC.NEU over the next instruction
    fn = lift_words([fset, br, mov32i(0, 0)])
    names = [(o.op, o.attr) for o in ops_of(fn)]
    assert ('pset', 'zf') in names and ('pset', 'nf') in names, names
    assert any(b.term[0] == 'cbr' for b in fn.blocks), [b.term[0] for b in fn.blocks]


def corpus_slice(shaders, n=40):
    corpus = Path(shaders) / 'corpus'
    if not corpus.is_dir():
        return [{'name': 'corpus slice', 'status': 'skipped', 'detail': 'no local corpus'}]
    stages = {}
    with open(Path(shaders) / 'variants.jsonl') as f:
        for line in f:
            for st, k in (json.loads(line).get('stages') or {}).items():
                stages[k] = st
    keys = sorted(stages)[::max(1, len(stages) // n)][:n]
    bad = []
    for k in keys:
        data = (corpus / (k + '.data')).read_bytes()
        ctrl = (corpus / (k + '.ctrl')).read_bytes()
        try:
            spv, man = translate.translate(stages[k], ctrl, data)
        except translate.Failed:
            continue
        ok, msg = spv_ok(spv)
        if ok is False:
            bad.append(k[:12] + ': ' + msg[:120])
    return [{'name': 'corpus slice of %d validates' % len(keys), 'status': 'failed' if bad else 'passed',
             'detail': '; '.join(bad[:5])}]


@test
def ldg_through_storage_descriptor():
    """LDG.E from c[0][0x110] + offset: a read-only SSBO for NVN descriptor 0, address minus the
    256-aligned base, B64 fills two registers, and the manifest names the descriptor."""
    lo = 0x4c1080000447000c                # IADD.CC R12, R0, c[0][0x110]
    hi = 0x4c1008000457ff0d                # IADD.X  R13, RZ, c[0][0x114]
    ldg = 0xeed5a00000070c00 | (8 << 20)   # LDG.E.64 R0, [R12 + 0x8]
    fn, info, spv, em = full(build('fs', pack_code([lo, hi, ldg, exit_(), bra(-8)]), fs_sph()))
    loads = [o for o in ops_of(fn) if o.op == 'sload']
    assert len(loads) == 2 and all(o.attr == 0 for o in loads), loads
    assert ('and', 0xFFFFFF00) in [(o.op, o.args[1].attr) for o in ops_of(fn)
                                   if o.op == 'and' and o.args[1].op == 'const'], 'base not aligned down'
    man = em.manifest('t')['ssbos']
    assert man == [{'binding': man[0]['binding'], 'desc': {'cbuf': 0, 'offset': 0x110}, 'nvn_index': 0,
                    'write': False, 'base_align': 256}], man
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def ldg_untraced_base_uses_only_descriptor():
    """Base added in an earlier block: with one descriptor read in the program, LDG uses it."""
    lo = 0x4c1080000447000c
    ldg = 0xeed4a00000070c00               # LDG.E.32 R0, [R12]
    fn = lift_words([lo, bra(0), ldg])
    assert [o.attr for o in ops_of(fn) if o.op == 'sload'] == [0]
    try:
        lift_words([ldg])
        raise AssertionError('LDG without a descriptor should be unsupported')
    except lift.Unsupported:
        pass


def gs_sph(max_vertices=3, topology=7, invocations=1):
    w = [0] * 20
    w[0] = 1 | (3 << 5) | (4 << 10)
    w[2] = invocations << 24
    w[3] = topology << 24
    w[4] = max_vertices
    return struct.pack('<20I', *w)


GS_PROLOGUE = [0xf0c8000001d7000d,     # S2R R13, SR_INVOCATION_INFO
               0x5f047f8020d70d00,     # VMAD R0, R13.B0, R13.B2, RZ
               0x50f41ba06ff70d07,     # VSETP P0, R13.B2, 0 (vertex count check)
               0xefd0000000070004,     # ISBERD R4, [R0]
               0x010800000000f004]     # @P0 MOV32I R4, 0x80000000


@test
def vmad_selects_bytes():
    """VMAD U8B0 * U8B2: byte extracts at bit 0 and bit 16 of the same register."""
    fn = lift_words(GS_PROLOGUE[:2] + [0xeff000000647ff00], stage='vs')
    bfes = sorted(o.args[1].attr for o in ops_of(fn) if o.op == 'bfe_u' and o.args[1].op == 'const'
                  and o.args[2].op == 'const' and o.args[2].attr == 8)
    assert bfes[:2] == [0, 16], bfes


@test
def gs_vertex_reads_emit_and_cut():
    """Geometry: ISBERD handle indexes the per-vertex inputs, OUT emits and cuts, SPH gives the modes."""
    words = GS_PROLOGUE + [0xefd802000707ff02,    # ALD R2, a[0x70], R4  (vertex R4 position.x)
                           0xefd800000807ff03,    # ALD R3, a[0x80], R0  (vertex R0 generic 0.x)
                           0xeff000000707ff02,    # AST a[0x70], R2
                           0xeff000000807ff03,    # AST a[0x80], R3
                           0xfbe000800ff70000,    # OUT.EMIT R0, R0, RZ
                           0xfbe001000ff70000,    # OUT.CUT  R0, R0, RZ
                           exit_(), bra(-8)]
    fn, info, spv, em = full(build('gs', pack_code(words), gs_sph()))
    ops = [o.op for o in ops_of(fn)]
    assert ops.count('gs_emit') == 1 and ops.count('gs_cut') == 1, ops
    assert all(o.args for o in ops_of(fn) if o.op == 'in'), 'per-vertex reads must carry the vertex index'
    g = em.manifest('t')['geometry']
    assert g == {'input': 'triangles', 'input_vertices': 3, 'output': 'triangle_strip', 'max_vertices': 3,
                 'invocations': 1}, g
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def vsetp_compares_selected_bytes():
    """VSETP of an unsigned byte against an immediate is a 32-bit unsigned compare of the extract."""
    fn = lift_words(GS_PROLOGUE[:1] + [GS_PROLOGUE[2]], stage='vs')
    cmps = [o for o in ops_of(fn) if o.op == 'icmp']
    assert cmps and any(root(c.args[0]).op == 'bfe_u' or c.args[0].op == 'bfe_u' for c in cmps), cmps


def spv_opcodes(spv):
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    out, k = [], 5
    while k < len(w):
        out.append(w[k] & 0xFFFF)
        k += w[k] >> 16
    return out


def c2_handle(rd, tex_off, smp_off=None):
    """MOV rd, c[2][tex_off] (then LOP.OR rd, rd, c[2][smp_off])."""
    out = [ins('mov', 'C', rd=rd, ra=0, b20=(tex_off >> 2, 14), b34=(2, 5), b39=(0xF, 4))]
    if smp_off is not None:
        out.append(ins('lop', 'C', rd=rd, ra=rd, b20=(smp_off >> 2, 14), b34=(2, 5), b41=(1, 2), b48=(7, 3)))
    return out


TXQ_B = 0xdf50000180470000        # TXQ.B R0, R0, TEX_HEADER_DIMENSION, mask xy
TXQ_BOUND = 0xdf48048380470000    # TXQ R0, R0, TidB 0x48 (c[2][0x120]), TEX_HEADER_DIMENSION, mask xyz
SUST_B_3D = 0xeb20038a00f70400    # SUST.B.3D.P.RGBA [R4], R0, R7


def txq_mask(w, mask):
    return (w & ~(0xF << 31)) | (mask << 31)


@test
def txq_b_size_query():
    """TXQ.B dimension query with mask xy: width and height of the level in Ra+1 into R0, R1, through
    a 2D binding for the separate texture and sampler words of the handle."""
    txq = TXQ_B | 4 << 8
    code = pack_code(c2_handle(4, 0x168, 0x568) + [mov32i(5, 0), txq, exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    q = [o for o in ops_of(fn) if o.op == 'txq']
    assert sorted(o.attr['comp'] for o in q) == [0, 1], q
    t = em.manifest('k')['textures']
    assert len(t) == 1 and t[0]['dim'] == '2d' and t[0]['nvn'] == {'kind': 'separate', 'index': 0, 'sampler_index': 0}, t
    assert 103 in spv_opcodes(spv), 'no OpImageQuerySizeLod'
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def txq_b_levels_and_absent_depth():
    """Mask zw. Alone, the query takes a 3D binding (depth asked for). With a 2D TLD.B of the same
    handle it takes that binding: depth reads 1 and the level count is OpImageQueryLevels."""
    txq = txq_mask(TXQ_B | 4 << 8, 0xC)
    code = pack_code(c2_handle(4, 0x168, 0x568) + [mov32i(5, 0), txq, exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    assert sorted(o.attr['comp'] for o in ops_of(fn) if o.op == 'txq') == [2, 3]
    assert [t['dim'] for t in em.manifest('k')['textures']] == ['3d']
    ops = spv_opcodes(spv)
    assert 106 in ops and 103 in ops, ops
    ok, msg = spv_ok(spv)
    assert ok is not False, msg
    tld = ins('tld_b', '-', rd=8, ra=0, rb=4, b28=(2, 3), b31=(0xF, 4))
    code = pack_code(c2_handle(4, 0x168, 0x568) + [mov32i(5, 0), txq, tld, exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    assert [t['dim'] for t in em.manifest('k')['textures']] == ['2d']
    ops = spv_opcodes(spv)
    assert 106 in ops and 103 not in ops, ops
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def txq_b_shares_fetch_binding():
    """A size query and a TLD.B of the same handle use one binding of the fetch's type (2D)."""
    tld = ins('tld_b', '-', rd=8, ra=0, rb=4, b28=(2, 3), b31=(0xF, 4))      # TLD.B.LZ 2D
    code = pack_code(c2_handle(4, 0x170, 0x570) + [mov32i(5, 0), TXQ_B | 4 << 8, tld, exit_(), bra(-8)])
    fn, info, spv, em = full(build('fs', code, fs_sph()))
    t = em.manifest('k')['textures']
    assert len(t) == 1 and t[0]['nvn'] == {'kind': 'separate', 'index': 1, 'sampler_index': 1}, t
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def compute_bound_txq_and_sust_image():
    """Compute: bound TXQ (TidB 0x48 = c[2][0x120]) queries the storage image a SUST.B writes through
    the same handle; image slot 0, 3D, size via OpImageQuerySize; workgroup size from the program as
    specialization constants 0..2."""
    code = pack_code([mov32i(0, 0), TXQ_BOUND] + c2_handle(7, 0x120) +
                     [mov32i(4, 1), mov32i(5, 2), mov32i(6, 3), SUST_B_3D, exit_(), bra(-8)])
    prog = build('cs', code, b'')
    prog.local_size = (4, 4, 4)
    fn, info, spv, em = full(prog)
    man = em.manifest('k')
    assert man['images'] == [{'binding': man['images'][0]['binding'], 'dim': '3d', 'format': 'unknown',
                              'handle': {'cbuf': 2, 'offset': 0x120}, 'nvn': {'kind': 'image', 'index': 0}}], man['images']
    assert not man['textures'] and man['local_size'] == [4, 4, 4], man
    ops = spv_opcodes(spv)
    assert 104 in ops and 99 in ops and 51 in ops, 'need OpImageQuerySize, OpImageWrite, OpSpecConstantComposite'
    ok, msg = spv_ok(spv)
    assert ok is not False, msg


@test
def control_section_stage_and_local_size():
    """NVN control section: word 0x714 names the stage, 0x748..0x750 the compute workgroup size."""
    c = bytearray(0x880)
    struct.pack_into('<I', c, 0x714, 5)
    struct.pack_into('<3I', c, 0x748, 8, 8, 1)
    assert translate.nvn_stage(bytes(c)) == 'cs' and translate.local_size(bytes(c)) == (8, 8, 1)
    struct.pack_into('<I', c, 0x714, 1)
    assert translate.nvn_stage(bytes(c)) == 'fs'
    struct.pack_into('<3I', c, 0x748, 64, 64, 1)
    try:
        translate.local_size(bytes(c))
    except translate.Failed:
        pass
    else:
        raise AssertionError('4096 invocations accepted')


# Library programs the game binds that are not in the Unity bundles (task-12 NVN_DUMP_MISSING, kept in
# <shaders>/missing): 2 VS, 6 multisample resolve FS (TXQ.B), 1 compute 3D downsample (TXQ, SUST.B).
BUILTIN_PROGRAMS = """296bf434e3fd 3112cc6fd2f2 43094543406a 5db5e878fff0 68d5e0b3aa2d ad89da88c92d
b3913c5cf577 b8fa73d443d8 f6ef73e109cd""".split()


def builtin_programs(shaders):
    d = Path(shaders) / 'missing'
    if not d.is_dir():
        return [{'name': 'built-in programs', 'status': 'skipped', 'detail': 'no shaders/missing'}]
    bad = []
    for pre in BUILTIN_PROGRAMS:
        f = sorted(d.glob(pre + '*.data'))
        if len(f) != 1:
            bad.append(pre + ': not found')
            continue
        ctrl = f[0].with_suffix('.ctrl').read_bytes()
        try:
            spv, man = translate.translate(translate.nvn_stage(ctrl), ctrl, f[0].read_bytes())
        except translate.Failed as e:
            bad.append(pre + ': ' + str(e)[:100])
            continue
        ok, msg = spv_ok(spv)
        if ok is False:
            bad.append(pre + ': ' + msg[:100])
    return [{'name': 'built-in programs (%d) translate and validate' % len(BUILTIN_PROGRAMS),
             'status': 'failed' if bad else 'passed', 'detail': '; '.join(bad[:5])}]


# Programs that failed in cache v1 before the burn-down (LDG, BRX, VMNMX, multisample fetch, bindless
# spills and predicated handles, geometry). Key prefixes only; the corpus stays on A:.
FORMER_FAILURES = """00a3127a56ecb7ee 0a926e4e9805f891 14ab224b78edb3df 14b8a203a30919b9 17ce81bc103a17f2
18a9ec625e3c4317 198efd2699412d09 1b8226231086f574 1c021dcf39baaf3b 207944ae6da74b74 21e6ba7461c94e39
2deb5c81b8abf8b6 3506d31023542fdc 3ae72116b177b16e 3b694c51a6caadd4 3bed69cc9538562a 430ba47efbeac560
4897ebc31cc274c0 4bbb933cfbfee9b7 4d2ca0d97f9dba7b 5080b93472ccf5c7 56b8000eb313c522 5819fb6b8a06ddda
591a21587474fc69 5986df6f84020d6d 64aa631ddd959f59 6850018d22a44b6f 6c2f964e07b5987a 771974a55455b021
77d3caac1b97c19e 7964f2af655e975a 8cf3cf815abfec4c 8e98763380b00313 972008cce0b45d45 9b898d396abd82d7
9c3d16f10d8ef0de a1b346cb6098fa21 a6b6c7e882417345 ac9850e132b11ad9 ad6eaa56175794e4 b7eeb965a481e051
c8b12ba486d55626 c91e53beca0f0d1c cae7746810b695a4 cbeb67e62eee154e ccbb123b9a926356 d30675c262b2eaa5
d87303d388e0f0d2 e041b634aa2a1b85 f64779e1c51c4e2c fe2691f69b7c7dde""".split()


def former_failures(shaders):
    corpus = Path(shaders) / 'corpus'
    if not corpus.is_dir():
        return [{'name': 'former failures', 'status': 'skipped', 'detail': 'no local corpus'}]
    stages = {}
    with open(Path(shaders) / 'variants.jsonl') as f:
        for line in f:
            for st, k in (json.loads(line).get('stages') or {}).items():
                stages[k] = st
    full_keys = {k[:16]: k for k in stages}
    bad = []
    for pre in FORMER_FAILURES:
        k = full_keys.get(pre)
        if k is None:
            bad.append(pre + ': not in corpus')
            continue
        try:
            spv, man = translate.translate(stages[k], (corpus / (k + '.ctrl')).read_bytes(),
                                           (corpus / (k + '.data')).read_bytes())
        except translate.Failed as e:
            bad.append(pre + ': ' + str(e)[:100])
            continue
        ok, msg = spv_ok(spv)
        if ok is False:
            bad.append(pre + ': ' + msg[:100])
    return [{'name': 'former failures (%d) translate and validate' % len(FORMER_FAILURES),
             'status': 'failed' if bad else 'passed', 'detail': '; '.join(bad[:5])}]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', required=True)
    ap.add_argument('--corpus')
    a = ap.parse_args()
    out = []
    for t in TESTS:
        try:
            t()
            out.append({'name': t.__name__, 'status': 'passed', 'detail': ''})
        except Exception as e:
            out.append({'name': t.__name__, 'status': 'failed',
                        'detail': '%s: %s | %s' % (type(e).__name__, e, traceback.format_exc().splitlines()[-3][:200])})
    if a.corpus:
        out += corpus_slice(a.corpus)
        out += former_failures(a.corpus)
        out += builtin_programs(a.corpus)
    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(json.dumps({'tests': out}, indent=1), encoding='utf-8')
    bad = [t for t in out if t['status'] == 'failed']
    for t in out:
        print('%-8s %s %s' % (t['status'].upper(), t['name'], t['detail']))
    print('shaderxlate: %d passed, %d failed, %d skipped' % (
        sum(t['status'] == 'passed' for t in out), len(bad), sum(t['status'] == 'skipped' for t in out)))
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
