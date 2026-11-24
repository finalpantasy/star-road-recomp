"""NSO0 loader: header, LZ4 segments, MOD0, .dynamic, dynsym, relocations.

Our own implementation. Guest addresses are module-relative offsets (text at 0).
Image layout in memory: text, rodata, data, bss at the offsets in the header.
"""
import struct
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lz4block  # noqa: E402

MAIN_DEFAULT = r"A:\AI\Recomps\smrpg-work\v101\exefs\main"


class Segment:
    def __init__(self, name, file_off, mem_off, size, comp_size, compressed, hash_):
        self.name = name
        self.file_off = file_off
        self.mem_off = mem_off
        self.size = size
        self.comp_size = comp_size
        self.compressed = compressed
        self.hash = hash_
        self.data = b""
        self.hash_ok = None


class NSO:
    def __init__(self, path=MAIN_DEFAULT):
        self.path = path
        raw = open(path, "rb").read()
        self.raw_sha256 = hashlib.sha256(raw).hexdigest()
        magic, ver, _res, flags = struct.unpack_from("<4sIII", raw, 0)
        if magic != b"NSO0":
            raise ValueError("not an NSO")
        self.flags = flags
        self.build_id = raw[0x40:0x60].hex()
        segs = []
        for i, name in enumerate(("text", "rodata", "data")):
            fo, mo, sz, extra = struct.unpack_from("<IIII", raw, 0x10 + 0x10 * i)
            csz = struct.unpack_from("<I", raw, 0x60 + 4 * i)[0]
            h = raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i]
            s = Segment(name, fo, mo, sz, csz, bool(flags & (1 << i)), h)
            s.extra = extra
            segs.append(s)
        self.bss_size = segs[2].extra
        for i, s in enumerate(segs):
            blob = raw[s.file_off:s.file_off + s.comp_size]
            s.data = lz4block.decompress(blob, s.size) if s.compressed else blob
            if flags & (1 << (3 + i)):
                s.hash_ok = hashlib.sha256(s.data).digest() == s.hash
        self.text, self.rodata, self.data_seg = segs
        self.segments = segs
        self.api_info = struct.unpack_from("<II", raw, 0x88)
        self.dynstr_ext = struct.unpack_from("<II", raw, 0x90)
        self.dynsym_ext = struct.unpack_from("<II", raw, 0x98)
        self.image_size = self.data_seg.mem_off + self.data_seg.size + self.bss_size
        img = bytearray(self.image_size)
        for s in segs:
            img[s.mem_off:s.mem_off + s.size] = s.data
        self.image = img  # raw (unrelocated) image
        self._parse_mod0()
        self._parse_dynamic()

    # ---- helpers
    def u32(self, a):
        return struct.unpack_from("<I", self.image, a)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.image, a)[0]

    def seg_of(self, addr):
        for s in self.segments:
            if s.mem_off <= addr < s.mem_off + s.size:
                return s.name
        if self.data_seg.mem_off + self.data_seg.size <= addr < self.image_size:
            return "bss"
        return None

    def cstr(self, a):
        e = self.image.find(b"\0", a)
        return bytes(self.image[a:e]).decode("utf-8", "replace")

    # ---- MOD0
    def _parse_mod0(self):
        mod_off = self.u32(4)
        self.mod0_off = mod_off
        if self.image[mod_off:mod_off + 4] != b"MOD0":
            raise ValueError("MOD0 not at %x" % mod_off)
        (dyn, bss_s, bss_e, eh_s, eh_e, rt) = struct.unpack_from("<6i", self.image, mod_off + 4)
        self.dynamic_off = mod_off + dyn
        self.bss_start = mod_off + bss_s
        self.bss_end = mod_off + bss_e
        self.eh_frame_hdr_start = mod_off + eh_s
        self.eh_frame_hdr_end = mod_off + eh_e
        self.module_object = mod_off + rt

    # ---- .dynamic
    def _parse_dynamic(self):
        DT = {}
        a = self.dynamic_off
        while True:
            tag, val = struct.unpack_from("<qQ", self.image, a)
            a += 16
            if tag == 0:
                break
            DT.setdefault(tag, []).append(val)
        self.dt = DT

        def g(t, d=0):
            return DT.get(t, [d])[0]
        self.rela = (g(7), g(8), g(9, 24))
        self.jmprel = (g(23), g(2))
        self.symtab, self.strtab = g(6), g(5)
        self.strsz = g(10)
        self.init_array = (g(25), g(27))
        self.fini_array = (g(26), g(28))
        self.hash_tab, self.gnu_hash = g(4), g(0x6ffffef5)
        self.needed = [self.cstr(self.strtab + v) for v in DT.get(1, [])]

    # ---- dynamic symbols
    def dynsyms(self):
        """Return list of (name, value, size, info, shndx)."""
        n = self._sym_count()
        out = []
        for i in range(n):
            nm, info, other, shndx, val, sz = struct.unpack_from("<IBBHQQ", self.image, self.symtab + 24 * i)
            out.append((self.cstr(self.strtab + nm) if nm else "", val, sz, info, shndx))
        return out

    def _sym_count(self):
        if self.hash_tab:  # DT_HASH: nchain = number of symbols
            return struct.unpack_from("<II", self.image, self.hash_tab)[1]
        gh = self.gnu_hash
        if gh:
            nb, symoff, bloom_n, shift = struct.unpack_from("<IIII", self.image, gh)
            bloom_off = gh + 16
            buckets = bloom_off + 8 * bloom_n
            chains = buckets + 4 * nb
            mx = max(struct.unpack_from("<%dI" % nb, self.image, buckets))
            if mx < symoff:
                return symoff
            i = mx - symoff
            while not (struct.unpack_from("<I", self.image, chains + 4 * i)[0] & 1):
                i += 1
            return symoff + i + 1
        return (self.strtab - self.symtab) // 24

    def relocs(self):
        """Yield (offset, type, sym_index, addend) from RELA and JMPREL."""
        for (off, size) in ((self.rela[0], self.rela[1]), (self.jmprel[0], self.jmprel[1])):
            if not off or not size:
                continue
            for i in range(size // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", self.image, off + 24 * i)
                yield r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add

    def relocated_image(self, syms=None, base=0):
        """Image with RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT applied for in-module targets.

        Pointer values are RVAs plus `base` (use base=0 for RVAs, 0x7100000000 for Ghidra).
        Returns (bytearray, stats). Unresolved external symbol slots stay zero.
        """
        img = bytearray(self.image)
        syms = syms if syms is not None else self.dynsyms()
        stats = {"RELATIVE": 0, "ABS64": 0, "GLOB_DAT": 0, "JUMP_SLOT": 0, "undef_sym": 0, "other": 0}
        for r_off, rtype, si, add in self.relocs():
            if rtype == 1027:  # R_AARCH64_RELATIVE
                struct.pack_into("<Q", img, r_off, (add + base) & 0xFFFFFFFFFFFFFFFF)
                stats["RELATIVE"] += 1
            elif rtype in (257, 1025, 1026):  # ABS64, GLOB_DAT, JUMP_SLOT
                key = {257: "ABS64", 1025: "GLOB_DAT", 1026: "JUMP_SLOT"}[rtype]
                nm, val, sz, info, shndx = syms[si]
                if shndx != 0:
                    struct.pack_into("<Q", img, r_off, (val + add + base) & 0xFFFFFFFFFFFFFFFF)
                    stats[key] += 1
                else:
                    stats["undef_sym"] += 1
            else:
                stats["other"] += 1
        return img, stats


if __name__ == "__main__":
    n = NSO(sys.argv[1] if len(sys.argv) > 1 else MAIN_DEFAULT)
    print("sha256", n.raw_sha256, "build_id", n.build_id)
    for s in n.segments:
        print(s.name, "file_off=%#x mem=%#x size=%#x comp=%#x hash_ok=%s" % (s.file_off, s.mem_off, s.size, s.comp_size, s.hash_ok))
    print("bss=%#x image=%#x" % (n.bss_size, n.image_size))
    print("MOD0 @%#x dynamic=%#x eh_hdr=%#x-%#x" % (n.mod0_off, n.dynamic_off, n.eh_frame_hdr_start, n.eh_frame_hdr_end))
    print("rela", [hex(x) for x in n.rela], "jmprel", [hex(x) for x in n.jmprel], "init_array", [hex(x) for x in n.init_array], "needed", n.needed)
    s = n.dynsyms()
    print("dynsyms", len(s), "defined", sum(1 for x in s if x[4]), "named", sum(1 for x in s if x[0]))
    img, st = n.relocated_image(s)
    print(st)
