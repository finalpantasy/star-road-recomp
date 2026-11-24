"""Baseline input check: the owner's dump and extracted files match tools/inputs.json.

Writes a JSON report for tools/game_check.py. Full dump hashing is opt-in
(--full-dumps) because the XCI is 6.8 GB; by default dumps are checked by size.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', required=True)
    ap.add_argument('--full-dumps', action='store_true')
    args = ap.parse_args()

    spec = json.loads((ROOT / 'tools' / 'inputs.json').read_text(encoding='utf-8'))
    work = Path(os.environ.get('SMRPG_WORK', spec['work_root_default']))
    tests = []

    def record(name, ok, detail=''):
        tests.append({'name': name, 'status': 'passed' if ok else 'failed', 'detail': detail})

    for key, d in spec['dumps'].items():
        p = ROOT / d['path']
        if not p.is_file():
            record(f'dump {key} present', False, 'missing')
            continue
        size_ok = p.stat().st_size == d['size']
        record(f'dump {key} size', size_ok, str(p.stat().st_size))
        if args.full_dumps:
            record(f'dump {key} sha256', sha256(p) == d['sha256'])

    for rel, want in spec['extracted'].items():
        p = work / rel
        if not p.is_file():
            record(f'extracted {rel}', False, 'missing; run extraction (task-1)')
            continue
        got = sha256(p)
        record(f'extracted {rel}', got == want, got[:16])

    romfs = work / spec['romfs']['dir']
    count = total = 0
    if romfs.is_dir():
        for f in romfs.rglob('*'):
            if f.is_file():
                count += 1
                total += f.stat().st_size
    record('romfs file count', count == spec['romfs']['file_count'], str(count))
    record('romfs total bytes', total == spec['romfs']['total_bytes'], str(total))

    meta = romfs / 'Data' / 'Managed' / 'Metadata' / 'global-metadata.dat'
    ok = False
    if meta.is_file():
        head = meta.read_bytes()[:8]
        ok = head[:4] == bytes.fromhex('af1bb1fa') and int.from_bytes(head[4:8], 'little') == spec['title']['il2cpp_metadata_version']
    record('il2cpp metadata magic and version', ok)

    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps({'tests': tests}, indent=1), encoding='utf-8')
    failed = [t['name'] for t in tests if t['status'] != 'passed']
    print(f"{len(tests) - len(failed)}/{len(tests)} passed" + (f"; failed: {failed}" if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
