"""AC3 differential check: our SPIR-V against the Ryubing translator (run as an oracle, never copied).

    python oracle_diff.py --n 500 [--seed 1] [--work A:/AI/Recomps/smrpg-work/shaders/oracle]

1. Samples N Unity variants (vs + fs) with a fixed seed from cache/v1/variants.jsonl.
2. Translates each stage program with the Ryubing ShaderHarness (--nvn-serve) from the same data section.
3. Writes layout files for both translators, generates helper shaders (GLSL, glslangValidator), and runs
   vkdiff (tools/shaderxlate/oracle) on lavapipe: same inputs, compares every output component.
4. Writes report.json: per stage case and per variant (a variant is equal when its vs and fs cases are).
"""
import argparse
import collections
import json
import os
import random
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import container   # noqa: E402

SHADERS = r'A:\AI\Recomps\smrpg-work\shaders'
HARNESS = r'A:\AI\Recomps\new-pokemon-snap\build\release\harness_work\bin\Release\net10.0\win-x64\ShaderHarness.exe'
GLSLANG = r'A:\AI\Recomps\new-pokemon-snap\tools\vulkan_sdk\Bin\glslangValidator.exe'
VKDIFF = r'A:\AI\Recomps\smrpg-work\build\vkdiff\cmake\vkdiff.exe'
LVP_ICD = r'A:\AI\Recomps\tools-ext\mesa\x\x64\lvp_icd.x86_64.json'
STAGE_NUM = {'vs': 0, 'fs': 1}
RYU_DIM = {1: '1d', 3: '2d', 4: '3d', 5: 'cube'}


# ---------------------------------------------------------------- SPIR-V reflection (interface only)

def reflect(spv):
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    i = 5
    deco = collections.defaultdict(dict)
    var = {}
    ptr = {}
    types = {}
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        a = w[i + 1:i + n]
        if op == 71:                     # Decorate
            deco[a[0]][a[1]] = a[2] if len(a) > 2 else True
        elif op == 32:                   # TypePointer
            ptr[a[0]] = (a[1], a[2])
        elif op == 59:                   # Variable
            var[a[1]] = (a[0], a[2])
        elif op in (19, 20, 21, 22, 23, 25, 27, 28, 30):
            types[a[0]] = (op, a[1:])
        i += n
    res = {'in': {}, 'out': {}, 'desc': [], 'undef_loads': undefined_loads(w)}
    for v, (pt, sc) in var.items():
        d = deco.get(v, {})
        if sc in (1, 3) and 30 in d and 11 not in d:
            res['in' if sc == 1 else 'out'][d[30]] = types.get(ptr[pt][1], (None,))[0]
        if 34 in d and 33 in d:
            res['desc'].append((d[34], d[33], sc))
    return res


def undefined_loads(w):
    """Function variables (no initializer) that are loaded but never stored anywhere: the module reads
    an undefined value. Static and path-insensitive, so it only flags certain undefined reads."""
    i, fvars, base, stored, loaded = 5, set(), {}, set(), set()
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        a = w[i + 1:i + n]
        if op == 59 and a[2] == 7 and len(a) == 3:
            fvars.add(a[1])
        elif op == 65 or op == 66:            # OpAccessChain / OpInBoundsAccessChain
            base[a[1]] = base.get(a[2], a[2])
        elif op == 62:                        # OpStore
            stored.add(base.get(a[0], a[0]))
        elif op == 61:                        # OpLoad
            loaded.add(base.get(a[2], a[2]))
        i += n
    return len((loaded - stored) & fvars)


# ---------------------------------------------------------------- layouts

def tex_key(h_off, s_off):
    return 'c2_%x' % h_off + ('_%x' % s_off if s_off is not None else '')


def our_layout(meta, ref):
    st = meta['sets']['set']
    out = ['# ours %s' % meta['key']]
    for u in meta['ubos']:
        out.append('ubo %d %d %d' % (st, u['binding'], u['cbuf']))
    for t in meta['textures']:
        s = t['sampler_handle']
        out.append('tex %d %d %s %d %s' % (st, t['binding'], t['dim'], int(t['shadow']),
                                          tex_key(t['handle']['offset'], s['offset'] if s else None)))
    return out


def ryu_layout(js, ref, stage, c1_path):
    out = ['# ryubing']
    for set_, b, sc in ref['desc']:
        if set_ == 0 and b == 0:
            out.append('support 0 0')
    for c in js['cbuffers']:
        if c['slot'] == 1:
            # c[1] is the program's own constant section; we inline it, Ryubing reads it from a UBO
            out.append('ubofile %d %d %s' % (c['set'], c['binding'], c1_path))
        else:
            out.append('ubo %d %d %d' % (c['set'], c['binding'], c['slot']))
    for t in js['textures']:
        hi = t['handleIndex']
        o0, o1, kind = hi & 0x3FFF, (hi >> 14) & 0x3FFF, (hi >> 28) & 0xF
        typ = t['type']
        dim = RYU_DIM.get(typ & 0xFF)
        if dim is None:
            raise ValueError('ryubing texture type %#x' % typ)
        if typ & 0x100:
            dim += '_array'
        h, sec = o0 * 4, (o1 * 4 if kind else None)
        if sec is not None and h > sec:
            h, sec = sec, h          # texture handles live below the sampler handles (0x168 vs 0x568)
        out.append('tex %d %d %s %d %s' % (t['set'], t['binding'], dim, 1 if typ & 0x400 else 0,
                                          tex_key(h, sec)))
    return out


# ---------------------------------------------------------------- helper shaders

def glsl(path, src, stage):
    with open(path + '.' + stage, 'w') as f:
        f.write(src)
    r = subprocess.run([GLSLANG, '-V', '--target-env', 'vulkan1.3', '-o', path, path + '.' + stage],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('glslang: ' + r.stdout[-500:])
    return path


def helper_vs(path, locs):
    decl = ''.join('layout(location=%d) out vec4 v%d;\n' % (l, l) for l in locs)
    body = ''.join('  v%d = vec4(p.x * %.3f + %.3f, p.y * %.3f - %.3f, p.x * p.y + %.3f, 1.0 - p.x * %.3f);\n'
                   % (l, 0.5 + 0.1 * l, 0.05 * l, 0.7 + 0.03 * l, 0.02 * l, 0.01 * l, 0.25 + 0.01 * l) for l in locs)
    src = ('#version 450\n' + decl +
           'void main() {\n  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n'
           '  gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);\n' + body + '}\n')
    return glsl(path, src, 'vert')


def helper_gs(path, locs, width):
    n = 1 + len(locs)
    decl = ''.join('layout(location=%d) in vec4 i%d[];\n' % (l, l) for l in locs)
    emits = '  put(0, gl_in[0].gl_Position);\n' + ''.join('  put(%d, i%d[0]);\n' % (k + 1, l) for k, l in enumerate(locs))
    src = ('#version 450\nlayout(points) in;\nlayout(points, max_vertices=%d) out;\n' % n + decl +
           'layout(location=0) flat out vec4 val;\n'
           'void put(int k, vec4 v) {\n'
           '  gl_Position = vec4((float(k) + 0.5) * 2.0 / %d.0 - 1.0, (float(gl_PrimitiveIDIn) + 0.5) * 2.0 / 64.0 - 1.0, 0.5, 1.0);\n'
           '  val = v; EmitVertex(); EndPrimitive();\n}\n' % width +
           'void main() {\n' + emits + '}\n')
    return glsl(path, src, 'geom')


def helper_fs(path):
    src = '#version 450\nlayout(location=0) flat in vec4 val;\nlayout(location=0) out vec4 o;\nvoid main() { o = val; }\n'
    return glsl(path, src, 'frag')


# ---------------------------------------------------------------- driver

def below_normal():
    """Drop this process (and the workers and tools it starts, which inherit it) to BelowNormal."""
    if os.name == 'nt':
        import ctypes
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        k.SetPriorityClass.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)


def compat_one(job):
    """Translate one stage program with SXL_COMPAT (inherited from the parent) and validate it."""
    st, k, corpus, odir = job
    import translate
    container.below_normal()
    try:
        spv, meta = translate.translate(st, open(os.path.join(corpus, k + '.ctrl'), 'rb').read(),
                                        open(os.path.join(corpus, k + '.data'), 'rb').read())
        sp = os.path.join(odir, k + '.spv')
        open(sp, 'wb').write(spv)
        ok, msg = translate.validate(sp)
        if not ok:
            return k, 'compat spirv-val: ' + msg[:200]
        tmp = os.path.join(odir, k + '.json.tmp')
        json.dump(meta, open(tmp, 'w'))
        os.replace(tmp, os.path.join(odir, k + '.json'))
        return k, None
    except Exception as e:
        return k, '%s: %s' % (type(e).__name__, e)


def compat_stamp(compat):
    """Translator source hash plus switches: compat outputs are reused only while it matches."""
    import hashlib
    h = hashlib.sha256(compat.encode())
    d = os.path.dirname(os.path.abspath(__file__))
    for n in sorted(os.listdir(d)):
        if n.endswith('.py') and n != 'oracle_diff.py':
            h.update(n.encode() + open(os.path.join(d, n), 'rb').read())
    return h.hexdigest()


def main():
    below_normal()
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=500)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--cache', default=os.path.join(SHADERS, 'cache', 'v1'))
    ap.add_argument('--work', default=os.path.join(SHADERS, 'oracle'))
    ap.add_argument('--compat', default='', help='oracle-compat switches for our side, e.g. ipa_mul')
    ap.add_argument('--cases', help='file of "stage key" lines: compare exactly these (no variant sample)')
    ap.add_argument('--corpus', default=os.path.join(SHADERS, 'corpus'))
    ap.add_argument('--budget', type=float, default=480,
                    help='seconds after which no new vkdiff chunk starts; rerun the same --work to continue')
    ap.add_argument('--jobs', type=int, default=16, help='processes for the compat translation prepass')
    a = ap.parse_args()
    t_start = time.time()
    os.environ['SXL_COMPAT'] = a.compat
    import translate   # after SXL_COMPAT is set (lift reads it at import)
    container.below_normal()
    os.makedirs(a.work, exist_ok=True)
    rdir = os.path.join(a.work, 'ryubing')
    hdir = os.path.join(a.work, 'helpers')
    ldir = os.path.join(a.work, 'layouts')
    cdir = os.path.join(a.work, 'c1')
    for d in (rdir, hdir, ldir, cdir):
        os.makedirs(d, exist_ok=True)
    index = {}
    for line in open(os.path.join(a.cache, 'index.jsonl')):
        r = json.loads(line)
        index[r['key']] = r
    variants = []
    for line in open(os.path.join(a.cache, 'variants.jsonl')):
        v = json.loads(line)
        st = v.get('stages') or {}
        if set(st) == {'vs', 'fs'}:
            variants.append(v)
    # one variant per distinct (vs, fs) pair, then a seeded sample
    uniq = {}
    for v in variants:
        uniq.setdefault((v['stages']['vs'], v['stages']['fs']), v)
    pool = sorted(uniq.values(), key=lambda v: (v['stages']['vs'], v['stages']['fs']))
    rng = random.Random(a.seed)
    sample = rng.sample(pool, min(a.n, len(pool)))
    keys = sorted({(st, k) for v in sample for st, k in v['stages'].items()})
    if a.cases:
        sample = []
        keys = [tuple(x.split()) for x in open(a.cases) if x.strip()]
    print('variants in pool %d, sampled %d, stage programs %d' % (len(pool), len(sample), len(keys)), flush=True)

    # Ryubing translations (one long-lived oracle process)
    corpus = a.corpus
    fresh = bool(a.compat or a.cases)   # translate our side now instead of reading the cache
    ryu = {}
    todo = [(st, k) for st, k in keys if not os.path.exists(os.path.join(rdir, k + '.json'))]
    if todo:
        p = subprocess.Popen([HARNESS, '--nvn-serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             creationflags=0x00004000)
        for st, k in todo:
            ctrl = open(os.path.join(corpus, k + '.ctrl'), 'rb').read()
            _, _, const_off, _, sph_off = struct.unpack_from('<5I', ctrl, 0x6F8)
            p.stdin.write('\t'.join([os.path.join(corpus, k + '.data'), os.path.join(rdir, k + '.spv'),
                                     os.path.join(rdir, k + '.json'), str(STAGE_NUM[st]), str(const_off),
                                     str(sph_off)]) + '\n')
            p.stdin.flush()
            ans = p.stdout.readline().strip()
            if ans != 'ok':
                ryu[k] = 'oracle failed: ' + ans
        p.stdin.close()
        p.wait(timeout=60)
    compat_err = {}
    if fresh:
        odir = os.path.join(a.work, 'ours_compat')
        os.makedirs(odir, exist_ok=True)
        stamp, sf = compat_stamp(a.compat), os.path.join(odir, 'stamp.txt')
        if not os.path.exists(sf) or open(sf).read().strip() != stamp:
            for n in os.listdir(odir):
                os.remove(os.path.join(odir, n))
            open(sf, 'w').write(stamp + '\n')
        need = [(st, k, corpus, odir) for st, k in keys if k not in ryu
                and not os.path.exists(os.path.join(odir, k + '.json'))]
        if need:
            import concurrent.futures as cf
            with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
                for k, err in ex.map(compat_one, need, chunksize=4):
                    if err:
                        compat_err[k] = err
        print('compat translations: %d new, %d failed, %.0f s' % (len(need), len(compat_err),
              time.time() - t_start), flush=True)
    # build jobs
    jobs = []
    case_info = {}
    for st, k in keys:
        if k in ryu:
            case_info[k] = {'stage': st, 'status': 'oracle_failed', 'message': ryu[k]}
            continue
        ix = index.get(k)
        if not fresh and (not ix or ix['status'] != 'ok'):
            case_info[k] = {'stage': st, 'status': 'ours_failed', 'message': (ix or {}).get('reason')}
            continue
        try:
            if fresh:
                # our side re-translated with the compat switches, validated like the cache
                if k in compat_err:
                    raise RuntimeError(compat_err[k])
                meta = json.load(open(os.path.join(odir, k + '.json')))
                our_spv = os.path.join(odir, k + '.spv')
            else:
                meta = json.load(open(os.path.join(a.cache, ix['meta'])))
                our_spv = os.path.join(a.cache, ix['spv'])
            ryu_spv = os.path.join(rdir, k + '.spv')
            rjs = json.load(open(os.path.join(rdir, k + '.json')))
            ro, rr = reflect(open(our_spv, 'rb').read()), reflect(open(ryu_spv, 'rb').read())
            c1 = os.path.join(cdir, k + '.c1')
            if not os.path.exists(c1):
                ns = container.NvnStage(st, open(os.path.join(corpus, k + '.ctrl'), 'rb').read(),
                                        open(os.path.join(corpus, k + '.data'), 'rb').read())
                open(c1, 'wb').write(bytes(ns.consts))
            la, lb = our_layout(meta, ro), ryu_layout(rjs, rr, st, c1)
            if st == 'fs':
                locs = sorted(set(ro['in']) | set(rr['in']))
                rts = sorted(set(ro['out']) | set(rr['out']))
                la += ['rt %d' % r for r in rts]
                lb += ['rt %d' % r for r in rts]
                hv = helper_vs(os.path.join(hdir, 'vs_' + '_'.join(map(str, locs)) + '.spv'), locs)
                helpers = (hv, '-', '-')
            else:
                attrs = sorted(set(ro['in']) | set(rr['in']))
                locs = sorted(set(ro['out']) | set(rr['out']))
                width = 1 + len(locs)
                for lay in (la, lb):
                    lay += ['attr %d' % x for x in attrs] + ['fb %d 64' % width]
                hg = helper_gs(os.path.join(hdir, 'gs_' + '_'.join(map(str, locs)) + '.spv'), locs, width)
                hf = os.path.join(hdir, 'fs_flat.spv')
                if not os.path.exists(hf):
                    helper_fs(hf)
                helpers = ('-', hg, hf)
            pa, pb = os.path.join(ldir, k + '.ours.txt'), os.path.join(ldir, k + '.ryu.txt')
            open(pa, 'w').write('\n'.join(la) + '\n')
            open(pb, 'w').write('\n'.join(lb) + '\n')
            jobs.append([k, st, helpers[0], helpers[1], helpers[2], our_spv, pa, ryu_spv, pb, str(a.seed)])
            case_info[k] = {'stage': st, 'ours_undef_loads': ro['undef_loads'], 'ryu_undef_loads': rr['undef_loads']}
        except Exception as e:
            case_info[k] = {'stage': st, 'status': 'setup_error', 'message': '%s: %s' % (type(e).__name__, e)}
    # run vkdiff, restarting after a timeout exit
    results = {}
    keep = os.path.join(a.work, 'results_all.tsv')     # results survive a rerun of the same work dir
    if os.path.exists(keep):
        ids = {j[0] for j in jobs}
        for line in open(keep):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 9 and f[0] in ids:
                results[f[0]] = f
    env = dict(os.environ, VK_ICD_FILENAMES=LVP_ICD, VK_DRIVER_FILES=LVP_ICD)
    t0 = time.time()
    pending = [j for j in jobs if j[0] not in results]
    while pending and time.time() - t_start < a.budget:
        jf = os.path.join(a.work, 'jobs.tsv')
        rf = os.path.join(a.work, 'results.tsv')
        chunk = pending[:150]     # keep each vkdiff process to a few minutes
        open(jf, 'w').write(''.join('\t'.join(j) + '\n' for j in chunk))
        if os.path.exists(rf):
            os.remove(rf)
        try:
            subprocess.run([VKDIFF, jf, rf], env=env, timeout=580, creationflags=0x00004000)
        except subprocess.TimeoutExpired:
            pass
        got = 0
        if os.path.exists(rf):
            for line in open(rf):
                f = line.rstrip('\n').split('\t')
                if len(f) >= 9:
                    results[f[0]] = f
                    got += 1
                    with open(keep, 'a') as kf:
                        kf.write(line if line.endswith('\n') else line + '\n')
        done = set(results)
        nxt = [j for j in pending if j[0] not in done]
        if len(nxt) == len(pending) and chunk:
            for j in nxt[:1]:
                results[j[0]] = [j[0], 'error', '0', '0', '0', '0', '0', '-', 'harness made no progress']
            nxt = nxt[1:]
        pending = nxt
    for k, f in results.items():
        case_info[k].update(status=f[1], values=int(f[2]), exact=int(f[3]), close=int(f[4]), bad=int(f[5]),
                            max_abs=f[6], first_bad=f[7], message=f[8])
    # explain differences that are not ours: the oracle module reads a never-written variable
    for k, c in case_info.items():
        if c.get('status') == 'differ' and c.get('ryu_undef_loads') and not c.get('ours_undef_loads'):
            c['class'] = 'oracle_undefined_read'
        elif c.get('status') == 'differ':
            c['class'] = 'unexplained'
    # variant view
    vres = collections.Counter()
    vlist = []
    for v in sample:
        s = [case_info[v['stages'][x]].get('status') for x in ('vs', 'fs')]
        cl = [case_info[v['stages'][x]].get('class') for x in ('vs', 'fs')]
        if s == ['equal', 'equal']:
            verdict = 'equal'
        elif 'differ' in s and 'unexplained' not in cl and all(x in ('equal', 'differ') for x in s):
            verdict = 'differ_oracle_undefined'
        elif 'differ' in s:
            verdict = 'differ'
        else:
            verdict = 'not_compared'
        vres[verdict] += 1
        vlist.append({'shader': v['shader'], 'pass': v['pass'], 'keywords': v['keywords'],
                      'vs': v['stages']['vs'], 'fs': v['stages']['fs'], 'verdict': verdict, 'stage_status': s})
    cases = collections.Counter((c['stage'], c.get('status')) for c in case_info.values())
    classes = collections.Counter((c['stage'], c.get('class')) for c in case_info.values() if c.get('class'))
    rep = {'oracle': 'Ryubing Ryujinx.Graphics.Shader via ShaderHarness --nvn-serve (run, not copied)',
           'harness': 'vkdiff on lavapipe, identical UBO, texture and vertex inputs; per-component compare '
                      '(exact bits, or |a-b| <= 1e-5*max(1,|a|,|b|))',
           'seed': a.seed, 'compat': a.compat or None, 'variants_sampled': len(sample), 'variant_verdicts': dict(vres),
           'stage_cases': {'%s_%s' % k: n for k, n in sorted(cases.items(), key=str)},
           'differ_classes': {'%s_%s' % k: n for k, n in sorted(classes.items(), key=str)},
           'pending_cases': len(pending), 'seconds': round(time.time() - t0, 1), 'cases': case_info, 'variants': vlist}
    json.dump(rep, open(os.path.join(a.work, 'report.json'), 'w'), indent=1)
    print(json.dumps({k: rep[k] for k in ('variants_sampled', 'variant_verdicts', 'stage_cases', 'differ_classes',
                                          'pending_cases', 'seconds')}))


if __name__ == '__main__':
    main()
