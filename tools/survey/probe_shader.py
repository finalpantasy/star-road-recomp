import sys, os, glob, collections, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
root = sys.argv[1]
import csv
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[2])][1:]
sh = [r for r in rows if 'Shader=' in r[3]]
print(len(sh), 'bundles with Shader'); 
for r in sh[:5]: print(r[0], r[1], r[3][:100])
r = sh[0]
b = Bundle(os.path.join(root, r[0])); nm, o, sz, fl = b.nodes[0]
d = b.read(o, sz); s = Serialized(d)
print(s.meta_end, s.data_offset, len(d))
for (pid, bo, bs, cid, ti) in s.objects:
    if cid == 48:
        tree = s.types[ti][3]
        print('tree nodes', len(tree), [(n[1], n[2]) for n in tree if n[0] <= 1][:30])
        v = read_tree(tree, d, s.data_offset + bo)
        print({k: (x if not isinstance(x, (bytes, list, dict)) else (type(x).__name__, len(x))) for k, x in v.items()})
        break
print('---')
cnt = collections.Counter(); names = []
for (pid, bo, bs, cid, ti) in s.objects:
    if cid != 48: continue
    v = read_tree(s.types[ti][3], d, s.data_offset + bo)
    pf = v['platforms']['Array']; cl = v['compressedLengths']['Array']; dl = v['decompressedLengths']['Array']; blob = v['compressedBlob']['Array']
    cnt[tuple(pf)] += 1
    pn = v['m_ParsedForm'].get('m_Name'); names.append(pn)
    if len(names) <= 3:
        print(pn, pf, v['offsets']['Array'][:6], cl[:6], dl[:6], len(blob), blob[:16].hex(), 'stageCounts', v['stageCounts']['Array'][:6])
print(cnt)
import re
print([n for n in names if n][:40])
print(collections.Counter(n.split('/')[0] for n in names if n))
print('=== decompress')
from nso import lz4_block
for (pid, bo, bs, cid, ti) in s.objects:
    if cid != 48: continue
    v = read_tree(s.types[ti][3], d, s.data_offset + bo)
    if v['m_ParsedForm'].get('m_Name') != 'CriMana/SofdecPrimeYuv': continue
    blob = v['compressedBlob']['Array']; dl = v['decompressedLengths']['Array'][0]['Array'][0]
    raw = lz4_block(blob, dl)
    import struct
    n = struct.unpack_from('<I', raw, 0)[0]
    offs = struct.unpack_from('<%dI' % n, raw, 4); lens = struct.unpack_from('<%dI' % n, raw, 4 + 4*n)
    base = 4 + 8*n
    print('subprograms', n, list(zip(offs, lens)))
    for k in range(n):
        if lens[k] < 100: continue
        prog = raw[base+offs[k]: base+offs[k]+lens[k]]
        print(k, len(prog), prog[:64].hex())
        print(re.findall(rb'[ -~]{4,}', prog)[:12])
        open(os.environ.get('SURVEY_OUT','.')+'/shader_sample_%d.bin' % k,'wb').write(prog)
    break
