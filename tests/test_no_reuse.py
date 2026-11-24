"""Guard for D-009: no reused code is tracked by Git.

Fails if any tracked file sits under reuse/ or carries a reuse provenance marker.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKER = 'STAR-ROAD-REUSED'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', required=True)
    a = ap.parse_args()
    files = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT, capture_output=True, check=True).stdout.split(b'\0')
    files = [f.decode('utf-8', 'replace') for f in files if f]
    under = [f for f in files if f.startswith('reuse/')]
    marked = []
    for f in files:
        p = ROOT / f
        if f == 'tests/test_no_reuse.py' or not p.is_file() or p.stat().st_size > 4_000_000:
            continue
        try:
            if MARKER in p.read_text(encoding='utf-8', errors='ignore'):
                marked.append(f)
        except OSError:
            pass
    tests = [
        {'name': 'no tracked files under reuse/', 'status': 'failed' if under else 'passed', 'detail': ', '.join(under[:10])},
        {'name': f'no tracked file carries {MARKER}', 'status': 'failed' if marked else 'passed', 'detail': ', '.join(marked[:10])},
    ]
    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(json.dumps({'tests': tests}, indent=1), encoding='utf-8')
    bad = [t['name'] for t in tests if t['status'] != 'passed']
    print('no-reuse guard: ' + ('PASS' if not bad else 'FAIL ' + '; '.join(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
