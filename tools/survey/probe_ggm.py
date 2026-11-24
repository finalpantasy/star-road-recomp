import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
p = sys.argv[1]
d = open(p, 'rb').read()
s = Serialized(d)
print(p, 'ver', s.version, 'unity', s.unity, 'plat', s.platform, 'tree', s.has_tree, 'objs', len(s.objects), 'data_off', s.data_offset)
print(collections.Counter(CLASS.get(o[3], o[3]) for o in s.objects).most_common(60))
import struct
for (pid, bo, bs, cid, ti) in s.objects:
    nm = CLASS.get(cid, cid)
    if nm in ('TimeManager', 'GraphicsSettings', 'QualitySettings', 129):
        raw = d[s.data_offset + bo: s.data_offset + bo + bs]
        print('==', nm, pid, bs); 
        if nm == 'TimeManager': print(struct.unpack_from('<4f', raw, 0))
        else: print(raw[:min(bs, 400)].hex())
