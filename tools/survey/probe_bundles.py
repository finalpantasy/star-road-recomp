import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glob, collections, time, sys
from unityfs import *
root = sys.argv[1]
fs = sorted(glob.glob(root + '/**/*.bundle', recursive=True))
print(len(fs))
for f in fs[:3] + fs[600:602]:
    t = time.time(); b = Bundle(f)
    print(f[-50:], b.version, b.engine, b.comp_type, hex(b.flags), len(b.blocks), [(n[0][-40:], n[2]) for n in b.nodes])
    d = b.head(0, 0x20000); s = Serialized(d)
    print(' ser v', s.version, 'plat', s.platform, 'tree', s.has_tree, collections.Counter(CLASS.get(o[3], o[3]) for o in s.objects).most_common(8), round(time.time() - t, 2))
