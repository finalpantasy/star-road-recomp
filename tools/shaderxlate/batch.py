"""Batch driver: translate every stage program in the scanned corpus into the v1 cache.

    python batch.py --shaders A:/AI/Recomps/smrpg-work/shaders [--jobs 16] [--limit N] [--no-val]

Input: <shaders>/corpus/<key>.data|.ctrl and <shaders>/variants.jsonl (written by scan.py), plus the
programs the game binds outside the Unity bundles, dumped by the runtime into <shaders>/missing
(NVN_DUMP_MISSING; same layout, stage from the control section).
Output: <shaders>/cache/v1/ as in docs/SHADER_CACHE.md (index.jsonl, spv/, meta/, variants.jsonl,
report.json). Runs at BelowNormal priority.
"""
import argparse
import collections
import json
import multiprocessing as mp
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import container   # noqa: E402
import translate   # noqa: E402

G = {}


def _init(corpus, out, val):
    container.below_normal()
    G.update(corpus=corpus, out=out, val=val, tmp=tempfile.mkdtemp(prefix='sxl_'))


def _write_atomic(path, data, mode='wb'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, mode) as f:
        f.write(data)
    os.replace(tmp, path)


def work(item):
    key, stage, corpus = item
    corpus, out = corpus or G['corpus'], G['out']
    t0 = time.perf_counter()
    rec = {'key': key, 'stage': stage, 'translator': translate.TRANSLATOR}
    try:
        data = open(os.path.join(corpus, key + '.data'), 'rb').read()
        ctrl = open(os.path.join(corpus, key + '.ctrl'), 'rb').read()
        modes = ['structured', 'dispatcher']
        last = None
        for mode in modes:
            spv, man = translate.translate(stage, ctrl, data, mode)
            if not G['val']:
                last = (spv, man, True, '')
                break
            tmp = os.path.join(G['tmp'], key + '.spv')
            with open(tmp, 'wb') as f:
                f.write(spv)
            ok, msg = translate.validate(tmp)
            os.remove(tmp)
            last = (spv, man, ok, msg)
            if ok or man.get('cf_mode') == 'dispatcher':
                break
        spv, man, ok, msg = last
        rec['insns'] = man['insns']
        rec['cf_mode'] = man['cf_mode']
        rec['spirv_val'] = 'ok' if ok else 'fail'
        if not ok:
            rec['status'] = 'failed'
            rec['reason'] = 'spirv-val: ' + msg.splitlines()[0][:300] if msg else 'spirv-val failed'
            rec['val_msg'] = msg[:2000]
        else:
            rel_spv = 'spv/%s/%s.spv' % (key[:2], key)
            rel_meta = 'meta/%s/%s.json' % (key[:2], key)
            _write_atomic(os.path.join(out, rel_spv), spv)
            man.update(status='ok', spv=rel_spv, meta=rel_meta)
            _write_atomic(os.path.join(out, rel_meta), json.dumps(man, indent=1), 'w')
            rec.update(status='ok', spv=rel_spv, meta=rel_meta)
    except translate.Failed as e:
        rec.update(status='failed', reason=str(e))
    except Exception as e:     # a translator bug: record it, never hide it
        rec.update(status='failed', reason='internal error: %s: %s' % (type(e).__name__, e))
    rec['ms'] = round((time.perf_counter() - t0) * 1000, 1)
    return rec


def reason_class(r):
    r = re.sub(r'0x[0-9a-f]+', 'ADDR', r)
    r = re.sub(r'\d+', 'N', r) if r.startswith('internal') else r
    return r[:160]


def below_normal():
    """Drop this process (and the workers and tools it starts, which inherit it) to BelowNormal."""
    if os.name == 'nt':
        import ctypes
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        k.SetPriorityClass.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)


def main():
    below_normal()
    if os.environ.get('SXL_COMPAT'):
        sys.exit('SXL_COMPAT is set: oracle-compat switches never go into the cache. Unset it.')
    ap = argparse.ArgumentParser()
    ap.add_argument('--shaders', default=r'A:\AI\Recomps\smrpg-work\shaders')
    ap.add_argument('--out')
    ap.add_argument('--jobs', type=int, default=min(16, os.cpu_count() or 4))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--no-val', action='store_true')
    ap.add_argument('--keys', help='file with one key per line (subset run)')
    ap.add_argument('--extra', action='append',
                    help='directory of programs the game binds outside the Unity bundles (corpus layout, '
                         'stage from the control section); default <shaders>/missing when it exists')
    a = ap.parse_args()
    container.below_normal()
    corpus = os.path.join(a.shaders, 'corpus')
    out = a.out or os.path.join(a.shaders, 'cache', 'v1')
    os.makedirs(out, exist_ok=True)
    stages = {}
    with open(os.path.join(a.shaders, 'variants.jsonl')) as f:
        for line in f:
            v = json.loads(line)
            for st, k in (v.get('stages') or {}).items():
                stages[k] = st
    # programs dumped at run time (NVN_DUMP_MISSING, task-12): keyed by the same SHA-256 of the data
    # section, stage from control word 0x714
    src = {}
    extra = a.extra if a.extra is not None else [d for d in [os.path.join(a.shaders, 'missing')] if os.path.isdir(d)]
    for d in extra:
        for f in sorted(os.listdir(d)):
            if not f.endswith('.data'):
                continue
            k = f[:-5]
            st = translate.nvn_stage(open(os.path.join(d, k + '.ctrl'), 'rb').read())
            if st and k not in stages:
                stages[k] = st
                src[k] = d
    keys = sorted(stages)
    if a.keys:
        want = [x.strip() for x in open(a.keys) if x.strip()]
        keys = [k for k in want if k in stages]
    if a.limit:
        keys = keys[:a.limit]
    t0 = time.time()
    recs = []
    with mp.Pool(a.jobs, _init, (corpus, out, not a.no_val)) as pool:
        for k, rec in enumerate(pool.imap_unordered(work, [(k, stages[k], src.get(k)) for k in keys], chunksize=8)):
            recs.append(rec)
            if (k + 1) % 2000 == 0:
                print('%d/%d %.0fs' % (k + 1, len(keys), time.time() - t0), flush=True)
    recs.sort(key=lambda r: r['key'])
    full = not a.limit and not a.keys
    idx_name = 'index.jsonl' if full else 'index.partial.jsonl'
    with open(os.path.join(out, idx_name), 'w') as f:
        for r in recs:
            line = {k: r.get(k) for k in ('key', 'stage', 'status', 'reason', 'spv', 'meta', 'insns', 'translator')}
            if r['key'] in src:
                line['source'] = 'nvn_dump'
            f.write(json.dumps(line) + '\n')
    if full:
        shutil.copyfile(os.path.join(a.shaders, 'variants.jsonl'), os.path.join(out, 'variants.jsonl'))
    by_stage = collections.Counter((r['stage'], r['status']) for r in recs)
    reasons = collections.Counter(reason_class(r['reason']) for r in recs if r['status'] == 'failed')
    examples = {}
    for r in recs:
        if r['status'] == 'failed':
            examples.setdefault(reason_class(r['reason']), r['key'])
    # variant-level view: a Unity variant is usable when all its stage programs translated
    okset = {r['key'] for r in recs if r['status'] == 'ok'}
    vtot = vok = 0
    with open(os.path.join(a.shaders, 'variants.jsonl')) as f:
        for line in f:
            v = json.loads(line)
            st = v.get('stages') or {}
            if not st:
                continue
            if not full and not all(k in stages and k in set(keys) for k in st.values()):
                continue
            vtot += 1
            vok += all(k in okset for k in st.values())
    report = {
        'translator': translate.TRANSLATOR, 'programs': len(recs),
        'ok': sum(r['status'] == 'ok' for r in recs),
        'failed': sum(r['status'] == 'failed' for r in recs),
        'by_stage': {'%s_%s' % k: n for k, n in sorted(by_stage.items())},
        'cf_mode': dict(collections.Counter(r.get('cf_mode') for r in recs if r['status'] == 'ok')),
        'spirv_val': {'validated': 0 if a.no_val else sum('spirv_val' in r for r in recs),
                      'pass': sum(r.get('spirv_val') == 'ok' for r in recs),
                      'fail': sum(r.get('spirv_val') == 'fail' for r in recs),
                      'tool': translate.SPIRV_VAL, 'target_env': 'vulkan1.3'},
        'variants': {'total': vtot, 'all_stages_ok': vok},
        'nvn_dump_programs': {'total': sum(r['key'] in src for r in recs),
                              'ok': sum(r['key'] in src and r['status'] == 'ok' for r in recs)},
        'failure_reasons': [{'reason': k, 'count': n, 'example_key': examples[k]} for k, n in reasons.most_common()],
        'seconds': round(time.time() - t0, 1), 'jobs': a.jobs, 'full_run': full,
    }
    rep_name = 'report.json' if full else 'report.partial.json'
    with open(os.path.join(out, rep_name), 'w') as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(out, 'failures.jsonl' if full else 'failures.partial.jsonl'), 'w') as f:
        for r in recs:
            if r['status'] == 'failed':
                f.write(json.dumps(r) + '\n')
    print(json.dumps({k: report[k] for k in ('programs', 'ok', 'failed', 'cf_mode', 'spirv_val', 'variants', 'nvn_dump_programs', 'seconds')}))
    for x in report['failure_reasons'][:25]:
        print('%6d  %s  %s' % (x['count'], x['reason'], x['example_key'][:16]))


if __name__ == '__main__':
    main()
