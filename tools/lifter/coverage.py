r"""Decode coverage of a module's .text: which words the lifter cannot lower, grouped by encoding class (task-5).

  python -I tools/lifter/coverage.py main            # histogram of failures
  python -I tools/lifter/coverage.py main --mask 0xFFE0FC00 --top 40
  python -I tools/lifter/coverage.py main --ext simd_a   # only the named extension modules loaded (SMRPG_EXT)

Words come from A:\AI\Recomps\smrpg-work\build\img\<module>.text.bin (flat .text, base = module base).
"""
import argparse
import collections
import os
import struct
import sys

ap = argparse.ArgumentParser()
ap.add_argument("module", nargs="?", default="main")
ap.add_argument("--mask", type=lambda x: int(x, 0), default=0xFFE0FC00)
ap.add_argument("--top", type=int, default=30)
ap.add_argument("--ext")
a = ap.parse_args()
if a.ext is not None:
    os.environ["SMRPG_EXT"] = a.ext
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dec  # noqa: E402
import lower as lw  # noqa: E402

BASES = {"main": 0x7100000000, "sdk": 0x7110000000, "subsdk0": 0x7120000000}
b = open(r"A:\AI\Recomps\smrpg-work\build\img\%s.text.bin" % a.module, "rb").read()
n = len(b) // 4
ok = 0
cnt = collections.Counter()
ex = {}
for i in range(n):
    w = struct.unpack_from("<I", b, i * 4)[0]
    try:
        lw.decode_any(w, BASES[a.module] + i * 4)
        ok += 1
    except dec.Unsupported:
        k = w & a.mask
        cnt[k] += 1
        ex.setdefault(k, w)
print("%s: %d words, lowered %d, unlowered %d" % (a.module, n, ok, n - ok))
for k, v in cnt.most_common(a.top):
    print("%8d  key %08x  e.g. %08x" % (v, k, ex[k]))
