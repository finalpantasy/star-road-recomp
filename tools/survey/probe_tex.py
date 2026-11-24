import sys, os, collections, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
root = sys.argv[1]
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[2], encoding='utf-8')][1:]
blob = collections.Counter(); seen = 0
def lin(fmt, w, h, mips):
    bw, bb = {'ASTC_6x6': (6, 16), 'ASTC_5x5': (5, 16), 'ASTC_4x4': (4, 16), 'DXT5': (4, 16), 'DXT1': (4, 8), 'BC7': (4, 16)}.get(fmt, (None, None))
    if not bw: return None
    t = 0
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m); t += math.ceil(mw / bw) * math.ceil(mh / bw) * bb
    return t
bycount = collections.Counter(); diffs = collections.Counter(); ex = []
for r in rows:
    if r[2] not in ('ASTC_6x6', 'DXT5', 'ASTC_5x5', 'RGBA32') : continue
    if seen > 400: break
    k = r[0]
    if bycount[k] > 3: continue
    b = Bundle(os.path.join(root, k)); nm, o, sz, fl = b.nodes[0]; d = b.read(o, sz); s = Serialized(d)
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid != 28: continue
        v = read_tree(s.types[ti][3], d, s.data_offset + bo)
        if bycount[k] > 3: break
        bycount[k] += 1; seen += 1
        pb = v['m_PlatformBlob']; pbb = pb['Array'] if isinstance(pb, dict) else pb
        blob[(len(pbb), bytes(pbb).hex())] += 1
        fm = {63:'R8'}.get(v['m_TextureFormat'])
        fmn = {48:'ASTC_4x4',49:'ASTC_5x5',50:'ASTC_6x6',12:'DXT5',10:'DXT1',25:'BC7'}.get(v['m_TextureFormat'])
        l = lin(fmn, v['m_Width'], v['m_Height'], v['m_MipCount'])
        if l:
            diffs[(fmn, v['m_CompleteImageSize'] == l)] += 1
            if len(ex) < 6: ex.append((fmn, v['m_Width'], v['m_Height'], v['m_MipCount'], v['m_CompleteImageSize'], l, v['m_MipsStripped'], bytes(pbb).hex()))
print(blob.most_common(12)); print(diffs); [print(e) for e in ex]
