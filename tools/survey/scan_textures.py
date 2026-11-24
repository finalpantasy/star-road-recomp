"""Pass 2: Texture2D + Material/Shader refs over all bundles. Output TSV + summary."""
import sys, os, glob, collections, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
root, out = sys.argv[1], sys.argv[2]
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[3])][1:]
tf = {1:'Alpha8',2:'ARGB4444',3:'RGB24',4:'RGBA32',5:'ARGB32',7:'RGB565',9:'R16',10:'DXT1',12:'DXT5',13:'RGBA4444',14:'BGRA32',15:'RHalf',16:'RGHalf',17:'RGBAHalf',18:'RFloat',19:'RGFloat',20:'RGBAFloat',24:'BC6H',25:'BC7',26:'BC4',27:'BC5',28:'DXT1Crunched',29:'DXT5Crunched',30:'PVRTC_RGB2',31:'PVRTC_RGBA2',32:'PVRTC_RGB4',33:'PVRTC_RGBA4',34:'ETC_RGB4',41:'EAC_R',42:'EAC_R_SIGNED',43:'EAC_RG',44:'EAC_RG_SIGNED',45:'ETC2_RGB',46:'ETC2_RGBA1',47:'ETC2_RGBA8',48:'ASTC_4x4',49:'ASTC_5x5',50:'ASTC_6x6',51:'ASTC_8x8',52:'ASTC_10x10',53:'ASTC_12x12',54:'ASTC_RGBA_4x4',55:'ASTC_RGBA_5x5',56:'ASTC_RGBA_6x6',57:'ASTC_RGBA_8x8',58:'ASTC_RGBA_10x10',59:'ASTC_RGBA_12x12',62:'RG16',63:'R8',64:'ETC_RGB4Crunched',65:'ETC2_RGBA8Crunched'}
fmt = collections.Counter(); sizes = collections.Counter(); w = open(out, 'w', encoding='utf-8'); t0 = time.time(); n = 0
w.write('bundle\tname\tformat\twidth\theight\tmips\timageCount\tdimension\tcolorSpace\tstreamSize\tinlineSize\tisReadable\n')
other = collections.Counter(); sample_fields = None
matshader = collections.Counter()
for r in rows:
    if 'Texture2D=' not in r[3]: continue
    b = Bundle(os.path.join(root, r[0])); nm, o, sz, fl = b.nodes[0]
    d = b.read(o, sz); s = Serialized(d)
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid != 28: continue
        v = read_tree(s.types[ti][3], d, s.data_offset + bo)
        if sample_fields is None: sample_fields = [k for k in v.keys()]
        f = tf.get(v['m_TextureFormat'], v['m_TextureFormat'])
        sd = v.get('m_StreamData') or {}
        fmt[f] += 1; sizes[(f, v['m_Width'], v['m_Height'])] += 1; n += 1
        w.write('%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%s\n' % (r[0], v['m_Name'], f, v['m_Width'], v['m_Height'], v['m_MipCount'], v.get('m_ImageCount', 1), v.get('m_TextureDimension'), v.get('m_ColorSpace'), sd.get('size', 0), len(v.get('image data', b'')), v.get('m_IsReadable')))
w.close()
print('textures', n, 'secs', round(time.time() - t0, 1)); print('fields', sample_fields)
print(fmt.most_common())
print('top sizes', [(k, c) for k, c in sizes.most_common(12)])
