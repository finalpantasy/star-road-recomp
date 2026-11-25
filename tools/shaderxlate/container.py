"""Unity (Switch, NVN platform 19) shader container reader.

Layout facts measured on the owner's 1.0.1 romfs (task-10):
- A Unity Shader object (class 48) holds per platform one or more LZ4 chunks (segments).
  Chunk 0 starts with u32 n and n x (offset, length, segment) triples; offset is relative to
  the start of that segment.
- m_ParsedForm.m_SubShaders[].m_Passes[].progVertex.m_PlayerSubPrograms[tier][] lists the
  variants: m_BlobIndex (program entry), m_KeywordIndices, m_GpuProgramType.
  m_ParameterBlobIndices[tier][] gives the matching reflection entry. On NVN the vertex
  sub-program carries the whole linked program (every stage), progFragment is empty.
- Program entry: Unity header, then u32 nvn_size, then an NVN block of nvn_size bytes:
  a 0x60 byte stage table (4 arrays of 6 u32, stage order VS, FS, GS, TCS, TES, CS;
  array 1 = program offset after the table, array 2 = control size, array 3 = data size,
  0xFFFFFFFF = stage absent), then per stage a glslc control section and a data section.
- Control section (magic 0x98761234): +0x6F8 program size (SPH + code), +0x6FC constant
  size, +0x700 constant offset in data, +0x704 data size, +0x708 SPH offset in data (0x30).
"""
import os
import struct
import sys
import hashlib

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'survey'))
import unityfs  # noqa: E402  (repo tool, task-3)


def _fast_lz4(b, n):
    import lz4.block   # only needed when reading bundles (scan.py), not for translation
    return lz4.block.decompress(bytes(b), uncompressed_size=n)


unityfs.lz4_block = _fast_lz4

CTRL_MAGIC = 0x98761234
STAGES = ('vs', 'fs', 'gs', 'tcs', 'tes', 'cs')
NVN_PLATFORM = 19


class NvnStage:
    __slots__ = ('stage', 'control', 'data', 'prog_size', 'const_size', 'const_off', 'sph_off', 'key')

    def __init__(self, stage, control, data):
        self.stage = stage
        self.control = control
        self.data = data
        (self.prog_size, self.const_size, self.const_off, dsz, self.sph_off) = struct.unpack_from('<5I', control, 0x6F8)
        if dsz != len(data):
            raise ValueError('data size mismatch %x vs %x' % (dsz, len(data)))
        self.key = stage_key(stage, data)

    @property
    def code(self):
        """Code bytes after the SPH (graphics) or from sph_off (compute)."""
        start = self.sph_off + (0 if self.stage == 'cs' else 0x50)
        end = self.sph_off + self.prog_size
        return self.data[start:end]

    @property
    def sph(self):
        return self.data[self.sph_off:self.sph_off + 0x50] if self.stage != 'cs' else b''

    @property
    def consts(self):
        return self.data[self.const_off:self.const_off + self.const_size]


def stage_key(stage, data):
    """Cache key (docs/SHADER_CACHE.md section 1): sha256 of the data section bytes only."""
    return hashlib.sha256(data).hexdigest()


def split_program(entry):
    """Return list of NvnStage from one Unity program entry (raises on a malformed entry)."""
    mpos = -1
    p = 0
    while True:
        p = entry.find(b'\x34\x12\x76\x98', p)
        if p < 0:
            break
        if p >= 0x64 and p % 4 == 0:
            mpos = p
            break
        p += 1
    if mpos < 0:
        raise ValueError('no NVN control section')
    hdr = mpos - 0x60
    nvn_size = struct.unpack_from('<I', entry, hdr - 4)[0]
    tbl = struct.unpack_from('<24I', entry, hdr)
    offs, csz, dsz = tbl[6:12], tbl[12:18], tbl[18:24]
    out = []
    for i, st in enumerate(STAGES):
        if offs[i] == 0xFFFFFFFF:
            continue
        a = mpos + offs[i]
        ctrl = entry[a:a + csz[i]]
        if struct.unpack_from('<I', ctrl, 0)[0] != CTRL_MAGIC:
            raise ValueError('stage %s: bad control magic' % st)
        data = entry[a + csz[i]:a + csz[i] + dsz[i]]
        out.append(NvnStage(st, ctrl, data))
    if not out:
        raise ValueError('empty stage table')
    end = max(offs[i] + csz[i] + dsz[i] for i in range(6) if offs[i] != 0xFFFFFFFF)
    if end + 0x60 != nvn_size:
        raise ValueError('nvn size mismatch')
    return out


def entries(chunks):
    """Split a platform blob into entries. Chunk 0 starts with u32 n, then n x (offset, length,
    segment) u32 triples; offset is relative to the start of chunk `segment`."""
    head = chunks[0]
    n = struct.unpack_from('<I', head, 0)[0]
    tri = struct.unpack_from('<%dI' % (3 * n), head, 4)
    return [chunks[tri[3 * i + 2]][tri[3 * i]:tri[3 * i] + tri[3 * i + 1]] for i in range(n)]


SHADER_FILES = None


def find_serialized(romfs):
    """Yield (relpath, bytes) for every serialized file in the romfs that may hold Shader objects."""
    data_dir = os.path.join(romfs, 'Data')
    for dp, dn, fn in os.walk(data_dir):
        dn.sort()
        for f in sorted(fn):
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, romfs).replace('\\', '/')
            if f.endswith('.resS') or f.endswith('.resource') or f.endswith('.json') or f.endswith('.dll'):
                continue
            with open(path, 'rb') as fh:
                head = fh.read(8)
            if head.startswith(b'UnityFS'):
                yield rel, 'bundle', path
            elif f in ('globalgamemanagers', 'unity default resources', 'unity_builtin_extra') or \
                    f.endswith('.assets') or f.startswith('level'):
                yield rel, 'serialized', path


def iter_serialized_blobs(rel, kind, path):
    if kind == 'bundle':
        b = unityfs.Bundle(path)
        for i, (nm, o, sz, fl) in enumerate(b.nodes):
            if nm.endswith('.resS') or nm.endswith('.resource'):
                continue
            yield '%s#%s' % (rel, nm), b.read(o, sz)
    else:
        with open(path, 'rb') as fh:
            yield rel, fh.read()


SHADER_TREE = [None]   # type tree taken from a bundle; player files are stripped of trees


def iter_shaders(data, where):
    """Yield dicts for every Shader object in one serialized file."""
    try:
        s = unityfs.Serialized(data)
    except Exception:
        return
    for (pid, bo, bs, cid, ti) in s.objects:
        if cid != 48:
            continue
        tree = s.types[ti][3]
        if tree is None:
            tree = SHADER_TREE[0]
            if tree is None:
                raise ValueError('%s: Shader without type tree' % where)
        elif SHADER_TREE[0] is None:
            SHADER_TREE[0] = tree
        v = unityfs.read_tree(tree, data, s.data_offset + bo)
        yield pid, v


def decode_shader(v):
    """Return (name, keyword_names, platform_raw_or_None, variants) for one Shader typetree value."""
    pf = v['m_ParsedForm']
    name = pf.get('m_Name')
    kw = pf['m_KeywordNames']['Array']
    plats = v['platforms']['Array']
    raw = None
    if NVN_PLATFORM in plats:
        k = plats.index(NVN_PLATFORM)
        # Large shaders split the platform blob into several LZ4 chunks (segments).
        offs = v['offsets']['Array'][k]['Array']
        cls = v['compressedLengths']['Array'][k]['Array']
        dls = v['decompressedLengths']['Array'][k]['Array']
        blob = v['compressedBlob']['Array']
        raw = [_fast_lz4(blob[o:o + c], d) for o, c, d in zip(offs, cls, dls)]
    variants = []
    for si, ss in enumerate(pf['m_SubShaders']['Array']):
        for pi, ps in enumerate(ss['m_Passes']['Array']):
            pname = ps.get('m_UseName') or ps.get('m_Name') or ''
            for stname in ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain'):
                pv = ps.get(stname)
                if not pv:
                    continue
                tiers = pv['m_PlayerSubPrograms']['Array']
                pbis = pv['m_ParameterBlobIndices']['Array']
                for tier, lst in enumerate(tiers):
                    pb = pbis[tier]['Array'] if tier < len(pbis) else []
                    for j, sp in enumerate(lst['Array']):
                        variants.append({
                            'subshader': si, 'pass': pi, 'pass_name': pname, 'prog': stname, 'tier': tier,
                            'blob': sp['m_BlobIndex'], 'param_blob': pb[j] if j < len(pb) else -1,
                            'keywords': [kw[x] if x < len(kw) else '#%d' % x for x in sp['m_KeywordIndices']['Array']],
                            'gpu_program_type': sp['m_GpuProgramType'],
                        })
    return name, kw, raw, variants


def below_normal():
    """Run this process at BelowNormal priority (project rule for heavy processes)."""
    if os.name == 'nt':
        import ctypes
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)
