r"""Manifest rows for Texture2D objects that live outside the addressable bundles (task-11): the plain serialized files
globalgamemanagers.assets, resources.assets, "unity default resources" and level0 (no type tree, so fields are
parsed by hand for Unity 2021.3, serialized version 22), plus every Cubemap (class 89) inside the addressable
bundles (6 faces per object; same field layout as Texture2D). Reads the owner's dump read only. Pixel bytes go to
textures\inline_extra\ and the rows to textures\manifest_extra.tsv (never into the repo). Ids continue after the
bundle manifest so a combined manifest has unique ids.
Usage: python -I export_manifest_extra.py [--data DATA_DIR] [--out DIR] [--first-id N]"""
import os, sys, struct, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'survey'))
from unityfs import Serialized, Bundle, read_tree

ap = argparse.ArgumentParser()
ap.add_argument('--data', default=r'A:\AI\Recomps\smrpg-work\v101\romfs\Data')
ap.add_argument('--out', default=r'A:\AI\Recomps\smrpg-work\textures')
ap.add_argument('--aa', default=r'A:\AI\Recomps\smrpg-work\v101\romfs\Data\StreamingAssets\aa\Switch')
ap.add_argument('--classes', default=r'A:\AI\Recomps\smrpg-work\analysis\survey\bundle_classes.tsv')
ap.add_argument('--first-id', type=int, default=13883)
a = ap.parse_args()
FILES = [('globalgamemanagers.assets', 'globalgamemanagers.assets.resS'), ('resources.assets', 'resources.assets.resS'),
         (os.path.join('Resources', 'unity default resources'), None), ('level0', 'level0.resS')]


class P:
    def __init__(s, d, p): s.d = d; s.p = p
    def u32(s): v = struct.unpack_from('<I', s.d, s.p)[0]; s.p += 4; return v
    def i32(s): v = struct.unpack_from('<i', s.d, s.p)[0]; s.p += 4; return v
    def u64(s): v = struct.unpack_from('<Q', s.d, s.p)[0]; s.p += 8; return v
    def u8(s): v = s.d[s.p]; s.p += 1; return v
    def al(s): s.p = (s.p + 3) & ~3
    def string(s):
        n = s.u32(); v = s.d[s.p:s.p + n].decode('utf-8', 'replace'); s.p += n; s.al(); return v
    def bytes_(s):
        n = s.u32(); v = s.d[s.p:s.p + n]; s.p += n; s.al(); return v


def parse(raw):
    p = P(raw, 0)
    name = p.string()
    p.i32()                       # m_ForcedFallbackFormat
    p.u8(); p.u8(); p.al()        # m_DownscaleFallback, m_IsAlphaChannelOptional
    w = p.i32(); h = p.i32(); csize = p.u32(); stripped = p.i32(); fmt = p.i32(); mips = p.i32()
    p.u8(); p.u8(); p.u8(); p.u8(); p.al()   # readable, preprocessed, ignore master limit, streaming mipmaps
    p.i32()                       # m_StreamingMipmapsPriority
    imgc = p.i32(); dim = p.i32()
    p.raw = p.d[p.p:p.p + 24]; p.p += 24     # m_TextureSettings
    p.i32()                       # m_LightmapFormat
    cs = p.i32()
    blob = p.bytes_()
    img = p.bytes_()
    soff = p.u64(); ssize = p.u32(); spath = p.string()
    return dict(name=name, w=w, h=h, csize=csize, stripped=stripped, fmt=fmt, mips=mips, imgc=imgc, dim=dim, cs=cs, blob=bytes(blob), img=bytes(img), soff=soff, ssize=ssize, spath=spath, end=p.p, total=len(raw))


os.makedirs(os.path.join(a.out, 'inline_extra'), exist_ok=True)
out = open(os.path.join(a.out, 'manifest_extra.tsv'), 'w', encoding='utf-8', newline='\n')
out.write('id\tbundle\tname\tunity_fmt\twidth\theight\tmips\tcolorspace\tcomplete_size\tres_node_offset\tstream_offset\tstream_size\tinline_file\tblob_hex\timage_count\tdimension\tmips_stripped\n')
n = a.first_id; skipped = []
for fname, ress in FILES:
    path = os.path.join(a.data, fname)
    d = open(path, 'rb').read()
    s = Serialized(d)
    rs = open(os.path.join(a.data, ress), 'rb').read() if ress else b''
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid not in (28, 89): continue
        raw = d[s.data_offset + bo: s.data_offset + bo + bs]
        try:
            t = parse(raw)
        except Exception as e:
            skipped.append((fname, pid, 'parse error %s' % e)); continue
        if t['end'] != t['total'] and not (cid == 89 and t['end'] < t['total']):
            skipped.append((fname, pid, 'trailing bytes %d of %d (%s)' % (t['total'] - t['end'], t['total'], t['name']))); continue
        data = t['img']
        if t['ssize'] and t['spath']:
            data = rs[t['soff']:t['soff'] + t['ssize']]
        inline = ''
        if len(data):
            inline = os.path.join(a.out, 'inline_extra', 'tex_%05d.bin' % n)
            open(inline, 'wb').write(data)
        out.write('\t'.join(str(x) for x in [n, fname.replace(chr(92), '/'), t['name'].replace('\t', ' '), t['fmt'], t['w'], t['h'], t['mips'], t['cs'], t['csize'],
                  -1, 0, 0, inline, t['blob'].hex(), t['imgc'], t['dim'], t['stripped']]) + '\n')
        n += 1
# Cubemaps inside the addressable bundles (type tree present, so read_tree works)
rows = [l.rstrip('\n').split('\t') for l in open(a.classes, encoding='utf-8')][1:]
for r in rows:
    if len(r) < 4 or 'Cubemap=' not in r[3]: continue
    b = Bundle(os.path.join(a.aa, r[0]))
    nm, o, sz, fl = b.nodes[0]
    d = b.read(o, sz); s = Serialized(d)
    resnodes = {x[0]: x for x in b.nodes[1:]}
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid != 89: continue
        v = read_tree(s.types[ti][3], d, s.data_offset + bo)
        pb = v['m_PlatformBlob']; pb = pb['Array'] if isinstance(pb, dict) else pb
        sd = v.get('m_StreamData') or {}
        data = bytes(v.get('image data', b'') or b'')
        if sd.get('size', 0) and sd.get('path'):
            rn = resnodes.get(sd['path'].split('/')[-1])
            if rn is None: raise SystemExit('missing resS node in %s' % r[0])
            data = b.read(rn[1] + sd.get('offset', 0), sd['size'])
        inline = ''
        if len(data):
            inline = os.path.join(a.out, 'inline_extra', 'tex_%05d.bin' % n)
            open(inline, 'wb').write(data)
        out.write('\t'.join(str(x) for x in [n, r[0].replace(chr(92), '/'), v['m_Name'].replace('\t', ' '), v['m_TextureFormat'], v['m_Width'], v['m_Height'],
                  v['m_MipCount'], v.get('m_ColorSpace', 0), v['m_CompleteImageSize'], -1, 0, 0, inline, bytes(pb).hex(), v.get('m_ImageCount', 1),
                  v.get('m_TextureDimension', 2), v.get('m_MipsStripped', 0)]) + '\n')
        n += 1
out.close()
print('extra textures', n - a.first_id, 'skipped', len(skipped))
for x in skipped: print('  skipped', x)
