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
