"""NSO loader and dynsym/import survey. Read-only on inputs. Usage: python -I nso.py <nso> <outdir>"""
import struct, sys, os, json

def lz4_block(src, out_size):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        t = src[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out += src[i:i+lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i+1] << 8); i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        s = len(out) - off
        if off >= ml:
            out += out[s:s+ml]
        else:
            for k in range(ml):
                out.append(out[s+k])
    assert len(out) == out_size, (len(out), out_size)
    return bytes(out)

def load_nso(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 0xC)[0]
    segs = []
    for i in range(3):
        fo, mo, sz = struct.unpack_from('<III', d, 0x10 + i*0x10)
        segs.append((fo, mo, sz))
    bss = struct.unpack_from('<I', d, 0x38)[0]
    build_id = d[0x40:0x60].hex()
    csz = struct.unpack_from('<III', d, 0x60)
    out = []
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo+csz[i]]
        out.append(lz4_block(raw, sz) if flags & (1 << i) else raw)
    return dict(segs=segs, bss=bss, build_id=build_id, flags=flags, data=out, csz=csz)

def image(nso):
    segs = nso['segs']; data = nso['data']
    end = segs[2][1] + segs[2][2] + nso['bss']
    img = bytearray(end)
    for (fo, mo, sz), dat in zip(segs, data):
        img[mo:mo+len(dat)] = dat
    return img

def cstr(buf, off):
    e = buf.index(b'\0', off)
    return buf[off:e].decode('latin1')

def dyn(img):
    mod0 = struct.unpack_from('<I', img, 4)[0]
    assert img[mod0:mod0+4] == b'MOD0'
    dyn_off = mod0 + struct.unpack_from('<i', img, mod0+4)[0]
    tags = {}
    p = dyn_off
    while True:
        tag, val = struct.unpack_from('<qQ', img, p); p += 16
        if tag == 0: break
        tags.setdefault(tag, val)
    return tags

def dynsyms(img):
    t = dyn(img)
    symtab, strtab = t[6], t[5]
    # symbol count: hash tables
    if 4 in t:
        nsym = struct.unpack_from('<I', img, t[4] + 4)[0]
    else:
        # DT_GNU_HASH (0x6ffffef5)
        gh = t[0x6ffffef5]
        nb, symoff, bloom, shift = struct.unpack_from('<IIII', img, gh)
        buckets = struct.unpack_from('<%dI' % nb, img, gh + 16 + bloom*8)
        chains = gh + 16 + bloom*8 + nb*4
        mx = max(buckets)
        nsym = mx
        if mx >= symoff:
            while not (struct.unpack_from('<I', img, chains + (nsym - symoff)*4)[0] & 1):
                nsym += 1
            nsym += 1
    syms = []
    for i in range(nsym):
        nm, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', img, symtab + i*24)
        syms.append((i, cstr(img, strtab + nm) if nm else '', info, shndx, val, size))
    return syms, t

if __name__ == '__main__':
    p, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    name = os.path.basename(p)
    n = load_nso(p)
    img = image(n)
    syms, tags = dynsyms(img)
    imps = [s for s in syms[1:] if s[3] == 0 and s[1]]
    exps = [s for s in syms[1:] if s[3] != 0]
    meta = dict(name=name, build_id=n['build_id'], segs=n['segs'], bss=n['bss'], nsym=len(syms), imports=len(imps), exports=len(exps),
                dynamic_tags={hex(k): v for k, v in tags.items()})
    json.dump(meta, open(os.path.join(outdir, name + '.meta.json'), 'w'), indent=1)
    with open(os.path.join(outdir, name + '.imports.tsv'), 'w') as f:
        f.write('index\tname\n')
        for s in imps: f.write('%d\t%s\n' % (s[0], s[1]))
    with open(os.path.join(outdir, name + '.exports.tsv'), 'w') as f:
        f.write('index\tname\tvalue\tsize\n')
        for s in exps: f.write('%d\t%s\t0x%x\t%d\n' % (s[0], s[1], s[4], s[5]))
    # rodata dump for string scans (kept on A:)
    open(os.path.join(outdir, name + '.image.bin'), 'wb').write(img)
    print(name, meta['build_id'], 'nsym', len(syms), 'imports', len(imps), 'exports', len(exps))
