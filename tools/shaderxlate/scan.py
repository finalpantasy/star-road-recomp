"""Scan the owner's romfs for every NVN shader variant and write a deduplicated corpus.

Usage: python tools/shaderxlate/scan.py --romfs A:/AI/Recomps/smrpg-work/v101/romfs --out A:/AI/Recomps/smrpg-work/shaders

Writes (all on A:, never in Git):
  <out>/corpus/<key>.data   NVN data section (SPH + code + constants), key = stage_key()
  <out>/corpus/<key>.ctrl   glslc control section
  <out>/variants.jsonl      one line per Unity variant: shader, pass, keywords, stage keys
  <out>/scan_summary.json   counts
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import container  # noqa: E402
import unityfs  # noqa: E402


def has_shader_class(path):
    """Cheap check: does any serialized node in this bundle declare class 48 in its type list."""
    b = unityfs.Bundle(path)
    for i, (nm, o, sz, fl) in enumerate(b.nodes):
        if nm.endswith('.resS') or nm.endswith('.resource'):
            continue
        n = 0x10000
        while True:
            head = b.read(o, min(n, sz))
            try:
                s = unityfs.Serialized(head)
                break
            except (struct.error, IndexError, ValueError, UnicodeDecodeError):
                if n >= sz:
                    raise
                n *= 4
        if any(t[0] == 48 for t in s.types):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    container.below_normal()
    t0 = time.time()
    corpus = os.path.join(a.out, 'corpus')
    os.makedirs(corpus, exist_ok=True)
    seen = {}
    nvariants = 0
    nshaders = 0
    failures = []
    files_scanned = 0
    with open(os.path.join(a.out, 'variants.jsonl'), 'w', encoding='utf8') as vf:
        srcs = list(container.find_serialized(a.romfs))
        srcs.sort(key=lambda x: x[1] != 'bundle')   # bundles first: they carry the Shader type tree
        for rel, kind, path in srcs:
            files_scanned += 1
            if kind == 'bundle' and not has_shader_class(path):
                continue
            for where, data in container.iter_serialized_blobs(rel, kind, path):
                for pid, v in container.iter_shaders(data, where):
                    nshaders += 1
                    name, kw, raw, variants = container.decode_shader(v)
                    ents = container.entries(raw) if raw is not None else []
                    for var in variants:
                        nvariants += 1
                        rec = {'file': where, 'path_id': pid, 'shader': name, **var}
                        try:
                            if raw is None:
                                raise ValueError('no NVN platform blob')
                            stages = container.split_program(ents[var['blob']])
                            rec['stages'] = {s.stage: s.key for s in stages}
                            for s in stages:
                                if s.key not in seen:
                                    seen[s.key] = s.stage
                                    with open(os.path.join(corpus, s.key + '.data'), 'wb') as fh:
                                        fh.write(s.data)
                                    with open(os.path.join(corpus, s.key + '.ctrl'), 'wb') as fh:
                                        fh.write(s.control)
                        except Exception as e:  # recorded, never silently dropped
                            rec['error'] = '%s: %s' % (type(e).__name__, e)
                            failures.append(rec)
                        vf.write(json.dumps(rec) + '\n')
    by_stage = {}
    for k, st in seen.items():
        by_stage[st] = by_stage.get(st, 0) + 1
    summ = {'files_scanned': files_scanned, 'shaders': nshaders, 'variants': nvariants,
            'variant_failures': len(failures), 'unique_stage_programs': len(seen), 'by_stage': by_stage,
            'seconds': round(time.time() - t0, 1)}
    with open(os.path.join(a.out, 'scan_summary.json'), 'w') as fh:
        json.dump(summ, fh, indent=1)
    print(json.dumps(summ))
    for f in failures[:20]:
        print('FAIL', f['shader'], f['blob'], f['error'])


if __name__ == '__main__':
    main()
