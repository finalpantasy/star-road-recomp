r"""Texture manifest exporter (task-11). Reads the owner's Unity bundles (via tools/survey/unityfs.py, read only) and
writes one TSV row per Texture2D: where its bytes live, its format and the 92-byte Switch platform blob.
Output goes to A:\AI\Recomps\smrpg-work\textures\manifest.tsv (never into the repo). Inline images (no .resS) are
written under textures\inline\ and referenced by file. Runs at BelowNormal priority.
Usage: python -I export_manifest.py [--aa ROOT] [--out DIR] [--limit N]"""
import os, sys, ctypes, argparse, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'survey'))
from unityfs import Bundle, Serialized, read_tree

ap = argparse.ArgumentParser()
ap.add_argument('--aa', default=r'A:\AI\Recomps\smrpg-work\v101\romfs\Data\StreamingAssets\aa\Switch')
ap.add_argument('--classes', default=r'A:\AI\Recomps\smrpg-work\analysis\survey\bundle_classes.tsv')
ap.add_argument('--out', default=r'A:\AI\Recomps\smrpg-work\textures')
ap.add_argument('--limit', type=int, default=0)
a = ap.parse_args()
try:
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
except Exception:
    pass
os.makedirs(os.path.join(a.out, 'inline'), exist_ok=True)
rows = [l.rstrip('\n').split('\t') for l in open(a.classes, encoding='utf-8')][1:]
bundles = [r[0] for r in rows if 'Texture2D=' in r[3]]
if a.limit: bundles = bundles[:a.limit]
out = open(os.path.join(a.out, 'manifest.tsv'), 'w', encoding='utf-8', newline='\n')
out.write('id\tbundle\tname\tunity_fmt\twidth\theight\tmips\tcolorspace\tcomplete_size\tres_node_offset\tstream_offset\tstream_size\tinline_file\tblob_hex\timage_count\tdimension\tmips_stripped\n')
n = 0; t0 = time.time()
for bn in bundles:
    b = Bundle(os.path.join(a.aa, bn))
    nm, o, sz, fl = b.nodes[0]
    d = b.read(o, sz); s = Serialized(d)
    resnodes = {x[0]: x for x in b.nodes[1:]}
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid != 28: continue
        v = read_tree(s.types[ti][3], d, s.data_offset + bo)
        pb = v['m_PlatformBlob']; pb = pb['Array'] if isinstance(pb, dict) else pb
        sd = v.get('m_StreamData') or {}
        path = sd.get('path', '') or ''
        res_off = -1; inline = ''
        img = v.get('image data', b'') or b''
        if sd.get('size', 0) and path:
            key = path.split('/')[-1]
            rn = resnodes.get(key)
            if rn is None:
                raise SystemExit('missing resS node %s in %s' % (key, bn))
            res_off = rn[1]
        elif len(img):
            inline = os.path.join(a.out, 'inline', 'tex_%05d.bin' % n)
            open(inline, 'wb').write(bytes(img))
        out.write('\t'.join(str(x) for x in [n, bn.replace(chr(92), '/'), v['m_Name'].replace('\t', ' '), v['m_TextureFormat'], v['m_Width'], v['m_Height'],
            v['m_MipCount'], v.get('m_ColorSpace', 0), v['m_CompleteImageSize'], res_off, sd.get('offset', 0), sd.get('size', 0), inline,
            bytes(pb).hex(), v.get('m_ImageCount', 1), v.get('m_TextureDimension', 2), v.get('m_MipsStripped', 0)]) + '\n')
        n += 1
out.close()
print('textures', n, 'bundles', len(bundles), 'secs', round(time.time() - t0, 1))
