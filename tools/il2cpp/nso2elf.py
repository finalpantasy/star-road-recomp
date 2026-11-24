"""Convert the decompressed NSO image to a plain AArch64 ELF64 (ET_EXEC) for inspection tools.

Segments are placed at BASE + RVA with relocations applied, so pointers in .data/.rodata are
absolute and valid at BASE. Function symbols come from ghidra_names.tsv (our generated names).
Output is derived from the owner's dump and stays on A:, never in Git.

Usage: python -I tools/il2cpp/nso2elf.py [out.elf] [base_hex]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import NSO  # noqa: E402

OUT_DIR = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp"
DEFAULT_BASE = 0x7100000000
PF_X, PF_W, PF_R = 1, 2, 4


def load_names(path):
    out = []
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            rva, size, cls, name = line.rstrip("\n").split("\t")[:4]
            out.append((int(rva, 16), int(size), name))
    return out


def load_class_names(path):
    """Unnamed functions get <class>_<rva> so inspection tools show their origin."""
    out = []
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            rva, size, cls, conf = line.rstrip("\n").split("\t")
            out.append((int(rva, 16), int(size), "%s_%x" % (cls, int(rva, 16))))
    return out


def build_elf(nso, names, base):
    img, _ = nso.relocated_image(base=base)
    segs = [(".text", nso.text, PF_R | PF_X, 0x6), (".rodata", nso.rodata, PF_R, 0x2), (".data", nso.data_seg, PF_R | PF_W, 0x3)]
    # section name table
    shstr = b"\0"
    sh_names = {}
    for n in (".text", ".rodata", ".data", ".bss", ".symtab", ".strtab", ".shstrtab"):
        sh_names[n] = len(shstr)
        shstr += n.encode() + b"\0"
    # symbols
    strtab = b"\0"
    syms = [struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)]
    for rva, size, name in sorted(names):
        nm = name.encode()
        syms.append(struct.pack("<IBBHQQ", len(strtab), 0x12, 0, 1, base + rva, size))  # GLOBAL FUNC, shndx=1 (.text)
        strtab += nm + b"\0"
    symtab = b"".join(syms)
    ehsize, phsize, shsize = 64, 56, 64
    nph = 4
    off = 0x1000
    file_parts = []
    seg_info = []
    for name, seg, flags, shflags in segs:
        data = bytes(img[seg.mem_off:seg.mem_off + seg.size])
        seg_info.append((name, seg, flags, shflags, off, len(data)))
        file_parts.append((off, data))
        off = (off + len(data) + 0xFFF) & ~0xFFF
    bss_start = nso.data_seg.mem_off + nso.data_seg.size
    bss_size = nso.image_size - bss_start
    symtab_off = off
    off += len(symtab)
    strtab_off = off
    off += len(strtab)
    shstr_off = off
    off += len(shstr)
    off = (off + 7) & ~7
    sh_off = off
    # program headers
    ph = b""
    for name, seg, flags, shflags, foff, flen in seg_info:
        memsz = flen
        if name == ".data":
            memsz = flen + bss_size
        ph += struct.pack("<IIQQQQQQ", 1, flags, foff, base + seg.mem_off, base + seg.mem_off, flen, memsz, 0x1000)
    ph += struct.pack("<IIQQQQQQ", 0x6474E551, PF_R | PF_W, 0, 0, 0, 0, 0, 16)  # GNU_STACK
    # section headers: null, .text, .rodata, .data, .bss, .symtab, .strtab, .shstrtab
    sh = [b"\0" * 64]
    for name, seg, flags, shflags, foff, flen in seg_info:
        sh.append(struct.pack("<IIQQQQIIQQ", sh_names[name], 1, shflags, base + seg.mem_off, foff, flen, 0, 0, 0x10, 0))
    sh.append(struct.pack("<IIQQQQIIQQ", sh_names[".bss"], 8, 0x3, base + bss_start, off, bss_size, 0, 0, 0x10, 0))
    sh.append(struct.pack("<IIQQQQIIQQ", sh_names[".symtab"], 2, 0, 0, symtab_off, len(symtab), 6, 1, 8, 24))
    sh.append(struct.pack("<IIQQQQIIQQ", sh_names[".strtab"], 3, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0))
    sh.append(struct.pack("<IIQQQQIIQQ", sh_names[".shstrtab"], 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    eh = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    eh += struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, base, ehsize, sh_off, 0, ehsize, phsize, nph, shsize, len(sh), 7)
    out = bytearray(eh + ph)
    out += b"\0" * (0x1000 - len(out))
    for foff, data in file_parts:
        out += b"\0" * (foff - len(out))
        out += data
    out += b"\0" * (symtab_off - len(out))
    out += symtab + strtab + shstr
    out += b"\0" * (sh_off - len(out))
    out += b"".join(sh)
    return out


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT_DIR, "main_v101.elf")
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else DEFAULT_BASE
    nso = NSO()
    names = load_names(os.path.join(OUT_DIR, "ghidra_names.tsv"))
    names += load_class_names(os.path.join(OUT_DIR, "ghidra_classes.tsv"))
    elf = build_elf(nso, names, base)
    with open(out, "wb") as f:
        f.write(elf)
    print("wrote", out, len(elf), "bytes,", len(names), "symbols, base %#x" % base)


if __name__ == "__main__":
    main()
