"""Minimal read-only UnityFS bundle + SerializedFile (metadata, typetree) parser. Pure Python, no deps."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import lz4_block


class R:
    def __init__(s, b, be=True):
        s.b = b; s.p = 0; s.be = be

    def u(s, fmt, n):
        v = struct.unpack_from(('>' if s.be else '<') + fmt, s.b, s.p); s.p += n; return v[0]

    def u8(s): return s.u('B', 1)
    def u16(s): return s.u('H', 2)
    def u32(s): return s.u('I', 4)
    def i32(s): return s.u('i', 4)
    def i64(s): return s.u('q', 8)

    def cstr(s):
        e = s.b.index(b'\0', s.p); v = s.b[s.p:e].decode('utf8', 'replace'); s.p = e + 1; return v

    def align(s, n=4): s.p = (s.p + n - 1) // n * n
    def raw(s, n):
        v = s.b[s.p:s.p + n]; s.p += n; return v


class Bundle:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0x200)
        r = R(h)
        assert r.cstr() == 'UnityFS'
        self.version = r.u32(); self.player = r.cstr(); self.engine = r.cstr()
        self.size = r.i64(); csz = r.u32(); usz = r.u32(); self.flags = r.u32()
        hdr_end = r.p
        if self.version >= 7:
            hdr_end = (hdr_end + 15) // 16 * 16
        self.comp_type = self.flags & 0x3f
        if self.flags & 0x80:
            self.f.seek(0, 2); self.f.seek(self.f.tell() - csz); bi = self.f.read(csz); data_start = hdr_end
        else:
            self.f.seek(hdr_end); bi = self.f.read(csz); data_start = hdr_end + csz
            if self.flags & 0x200: data_start = (data_start + 15) // 16 * 16
        bi = self._dec(bi, usz, self.comp_type)
        q = R(bi); q.p = 16
        self.blocks = []
        for _ in range(q.i32()):
            self.blocks.append((q.u32(), q.u32(), q.u16()))
        self.nodes = []
        for _ in range(q.i32()):
            o = q.i64(); sz = q.i64(); fl = q.u32(); nm = q.cstr(); self.nodes.append((nm, o, sz, fl))
        self.data_start = data_start
        self.boff = []; co = data_start; uo = 0
        for u_, c_, f_ in self.blocks:
            self.boff.append((uo, co)); uo += u_; co += c_
        self._cache = {}

    @staticmethod
    def _dec(b, usz, ct):
        if ct == 0: return b
        if ct in (2, 3): return lz4_block(b, usz)
        raise NotImplementedError('compression %d' % ct)

    def block(self, i):
        if i not in self._cache:
            u_, c_, f_ = self.blocks[i]
            self.f.seek(self.boff[i][1]); raw = self.f.read(c_)
            self._cache[i] = self._dec(raw, u_, f_ & 0x3f)
        return self._cache[i]

    def read(self, off, size):
        out = bytearray(); pos = off; end = off + size
        for i, (u_, c_, f_) in enumerate(self.blocks):
            bs = self.boff[i][0]; be_ = bs + u_
            if be_ <= pos or bs >= end: continue
            blk = self.block(i)
            out += blk[max(pos, bs) - bs: min(end, be_) - bs]
            pos = min(end, be_)
        return bytes(out)

    def node(self, i):
        nm, o, sz, fl = self.nodes[i]; return self.read(o, sz)

    def head(self, i, n=0x4000):
        nm, o, sz, fl = self.nodes[i]; return self.read(o, min(n, sz))


COMMON = {}


def _init_common():
    s = ("AABB\0AnimationClip\0AnimationCurve\0AnimationState\0Array\0Base\0BitField\0bitset\0bool\0char\0ColorRGBA\0"
         "Component\0data\0deque\0double\0dynamic_array\0FastPropertyName\0first\0float\0Font\0GameObject\0Generic Mono\0"
         "GradientNEW\0GUID\0GUIStyle\0int\0list\0long long\0map\0Matrix4x4f\0MdFour\0MonoBehaviour\0MonoScript\0"
         "m_ByteSize\0m_Curve\0m_EditorClassIdentifier\0m_EditorHideFlags\0m_Enabled\0m_ExtensionPtr\0m_GameObject\0"
         "m_Index\0m_IsArray\0m_IsStatic\0m_MetaFlag\0m_Name\0m_ObjectHideFlags\0m_PrefabInternal\0m_PrefabParentObject\0"
         "m_Script\0m_StaticEditorFlags\0m_Type\0m_Version\0Object\0pair\0PPtr<Component>\0PPtr<GameObject>\0"
         "PPtr<Material>\0PPtr<MonoBehaviour>\0PPtr<MonoScript>\0PPtr<Object>\0PPtr<Prefab>\0PPtr<Sprite>\0"
         "PPtr<TextAsset>\0PPtr<Texture>\0PPtr<Texture2D>\0PPtr<Transform>\0Prefab\0Quaternionf\0Rectf\0RectInt\0"
         "RectOffset\0second\0set\0short\0size\0SInt16\0SInt32\0SInt64\0SInt8\0staticvector\0string\0TextAsset\0"
         "TextMesh\0Texture\0Texture2D\0Transform\0TypelessData\0UInt16\0UInt32\0UInt64\0UInt8\0unsigned int\0"
         "unsigned long long\0unsigned short\0vector\0Vector2f\0Vector3f\0Vector4f\0m_ScriptingClassIdentifier\0"
         "Gradient\0Type*\0int2_storage\0int3_storage\0BoundsInt\0m_CorrespondingSourceObject\0m_PrefabInstance\0"
         "m_PrefabAsset\0FileSize\0Hash128\0RenderingLayerMask\0").split('\0')
    off = 0
    for t in s[:-1]:
        COMMON[off] = t; off += len(t) + 1


_init_common()

CLASS = {1: 'GameObject', 4: 'Transform', 20: 'Camera', 21: 'Material', 23: 'MeshRenderer', 28: 'Texture2D',
         33: 'MeshFilter', 43: 'Mesh', 48: 'Shader', 49: 'TextAsset', 74: 'AnimationClip', 83: 'AudioClip',
         84: 'RenderTexture', 89: 'Cubemap', 90: 'Avatar', 91: 'AnimatorController', 95: 'Animator',
         114: 'MonoBehaviour', 115: 'MonoScript', 128: 'Font', 137: 'SkinnedMeshRenderer', 142: 'AssetBundle',
         198: 'ParticleSystem', 199: 'ParticleSystemRenderer', 212: 'SpriteRenderer', 213: 'Sprite',
         222: 'CanvasRenderer', 223: 'Canvas', 224: 'RectTransform', 329: 'VideoClip', 1001: 'PrefabInstance',
         30: 'GraphicsSettings', 47: 'QualitySettings', 5: 'TimeManager', 126: 'NavMesh?', 156: 'TerrainData',
         43.5: 'x'}


class Serialized:
    """Parse metadata only (data must hold at least the metadata section)."""

    def __init__(self, data):
        r = R(data)
        self.meta_size = r.u32(); self.file_size = r.u32(); self.version = r.u32(); self.data_offset = r.u32()
        self.big = r.u8(); r.raw(3)
        if self.version >= 22:
            self.meta_size = r.u32(); self.file_size = r.i64(); self.data_offset = r.i64(); r.i64()
        r.be = bool(self.big)
        self.unity = r.cstr()
        r.be = False
        self.platform = r.u32()
        self.has_tree = bool(r.u8())
        self.types = []
        ntypes = r.u32()
        for _ in range(ntypes):
            cid = r.i32(); stripped = r.u8(); sidx = r.u16()
            if cid == 114: r.raw(16)
            r.raw(16)
            tree = None
            if self.has_tree:
                n = r.u32(); sbs = r.u32()
                nsz = 32 if self.version >= 19 else 24
                nb = r.raw(n * nsz); sb = r.raw(sbs)

                def st(off, sb=sb):
                    if off & 0x80000000:
                        return COMMON.get(off & 0x7fffffff, '?%d' % (off & 0x7fffffff))
                    return sb[off:sb.index(b'\0', off)].decode()
                nodes = []
                for k in range(n):
                    ver, lvl, arr, to, no, bsz, idx, meta = struct.unpack_from('<HBBIIiii', nb, k * nsz)
                    nodes.append((lvl, st(to), st(no), bsz, arr, meta))
                tree = nodes
                if self.version >= 21:
                    dn = r.u32(); r.raw(4 * dn)
            self.types.append((cid, stripped, sidx, tree))
        nobj = r.u32()
        self.objects = []
        for _ in range(nobj):
            r.align(4); pid = r.i64(); bo = r.i64() if self.version >= 22 else r.u32(); bs = r.u32(); ti = r.i32()
            self.objects.append((pid, bo, bs, self.types[ti][0], ti))
        self.meta_end = r.p


def read_tree(nodes, data, base=0, be=False):
    """Generic typetree reader. nodes = list of (level,type,name,bytesize,isarray,meta). Returns dict for root."""
    pos = [base]
    fmt = {'SInt8': 'b', 'UInt8': 'B', 'char': 'B', 'bool': '?', 'SInt16': 'h', 'short': 'h', 'UInt16': 'H',
           'unsigned short': 'H', 'SInt32': 'i', 'int': 'i', 'UInt32': 'I', 'unsigned int': 'I', 'SInt64': 'q',
           'long long': 'q', 'UInt64': 'Q', 'unsigned long long': 'Q', 'float': 'f', 'double': 'd'}
    e = '>' if be else '<'

    def rd(i):
        lvl, typ, name, bsz, arr, meta = nodes[i]
        j = i + 1
        while j < len(nodes) and nodes[j][0] > lvl: j += 1
        end = j
        if typ in fmt:
            f = e + fmt[typ]; sz = struct.calcsize(f)
            v = struct.unpack_from(f, data, pos[0])[0]; pos[0] += sz
        elif typ == 'string':
            n = struct.unpack_from(e + 'I', data, pos[0])[0]; pos[0] += 4
            v = bytes(data[pos[0]:pos[0] + n]).decode('utf8', 'replace'); pos[0] += n
            pos[0] = (pos[0] + 3) // 4 * 4
            return v, end
        elif arr:
            # node i is the array holder: children are size + data
            ch_size = i + 1; ch_data = i + 2
            n = struct.unpack_from(e + 'i', data, pos[0])[0]; pos[0] += 4
            lvl2, typ2, name2, bsz2, arr2, meta2 = nodes[ch_data]
            if typ2 in ('UInt8', 'SInt8', 'char') and True:
                v = bytes(data[pos[0]:pos[0] + n]); pos[0] += n
            else:
                v = []
                for _ in range(n):
                    x, _e = rd(ch_data); v.append(x)
            if meta & 0x4000: pos[0] = (pos[0] + 3) // 4 * 4
            return v, end
        else:
            d = {}
            k = i + 1
            while k < end:
                nm = nodes[k][2]
                x, k2 = rd(k); d[nm] = x; k = k2
            v = d
            return v, end
        if meta & 0x4000: pos[0] = (pos[0] + 3) // 4 * 4
        return v, end
    return rd(0)[0]
