r"""Prepare module images for the runtime loader (task-5). Our own code.

For each of main, sdk, subsdk0 writes into OUT (default A:\AI\Recomps\smrpg-work\build\img):
  <m>.img   decompressed NSO image (text, rodata, data, bss zero), UNRELOCATED
  <m>.rel   "REL0", u32 count, u32 0, u32 0, then count x 24 bytes:
            u64 offset, u32 type (257 ABS64, 1025 GLOB_DAT, 1026 JUMP_SLOT, 1027 RELATIVE), u32 symindex, i64 addend
  <m>.sym   "SYM0", u32 count, u32 strsz, u32 0, then count x 24 bytes:
            u32 name_off, u8 defined, u8 bind, u8 type, u8 pad, u64 value, u64 size ; then the string table
  <m>.json  segment layout, init/fini arrays, dynamic info, MOD0 info
All offsets are module-relative (text at 0); the runtime adds the module base.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "il2cpp"))
from nso import NSO  # noqa: E402

EXEFS = r"A:\AI\Recomps\smrpg-work\v101\exefs"
OUT = r"A:\AI\Recomps\smrpg-work\build\img"
MODULES = ("main", "sdk", "subsdk0")
RELTYPES = (257, 1025, 1026, 1027)


def prep(name, exefs, out):
    n = NSO(os.path.join(exefs, name))
    with open(os.path.join(out, name + ".img"), "wb") as f:
        f.write(n.image)
    rels, other = [], {}
    for off, t, si, add in n.relocs():
        if t in RELTYPES:
            rels.append((off, t, si, add))
        else:
            other[t] = other.get(t, 0) + 1
    with open(os.path.join(out, name + ".rel"), "wb") as f:
        f.write(struct.pack("<4sIII", b"REL0", len(rels), 0, 0))
        for r in rels:
            f.write(struct.pack("<QIIq", *r))
    syms = n.dynsyms()
    strtab = bytearray(b"\0")
    recs = bytearray()
    for nm, val, sz, info, shndx in syms:
        no = 0
        if nm:
            no = len(strtab)
            strtab += nm.encode() + b"\0"
        recs += struct.pack("<IBBBBQQ", no, 1 if shndx else 0, info >> 4, info & 15, 0, val, sz)
    with open(os.path.join(out, name + ".sym"), "wb") as f:
        f.write(struct.pack("<4sIII", b"SYM0", len(syms), len(strtab), 0))
        f.write(recs)
        f.write(strtab)
    segs = [dict(name=s.name, mem_off=s.mem_off, size=s.size) for s in n.segments]
    info = dict(
        module=name, sha256=n.raw_sha256, build_id=n.build_id,
        image_size=n.image_size, text_size=n.text.size,
        rodata_off=n.rodata.mem_off, rodata_size=n.rodata.size, data_off=n.data_seg.mem_off, data_size=n.data_seg.size, bss_off=n.data_seg.mem_off + n.data_seg.size, bss_size=n.bss_size,
        segments=segs,
        init_array=dict(off=n.init_array[0], size=n.init_array[1]),
        fini_array=dict(off=n.fini_array[0], size=n.fini_array[1]),
        dynamic_off=n.dynamic_off,
        rela=dict(off=n.rela[0], size=n.rela[1]), jmprel=dict(off=n.jmprel[0], size=n.jmprel[1]),
        symtab=n.symtab, strtab=n.strtab, strsz=n.strsz, needed=n.needed,
        mod0=dict(off=n.mod0_off, eh_frame_hdr=[n.eh_frame_hdr_start, n.eh_frame_hdr_end],
                  bss=[n.bss_start, n.bss_end], module_object=n.module_object),
        nreloc=len(rels), nreloc_other=other, nsym=len(syms),
        ndefined=sum(1 for s in syms if s[4]),
        init_entries=[n.u64(n.init_array[0] + 8 * i) for i in range(n.init_array[1] // 8)] if n.init_array[1] else [],
    )
    with open(os.path.join(out, name + ".json"), "w") as f:
        json.dump(info, f, indent=1)
    return info


def main():
    exefs = os.environ.get("SMRPG_EXEFS", EXEFS)
    out = os.environ.get("SMRPG_IMG", OUT)
    os.makedirs(out, exist_ok=True)
    for m in MODULES:
        i = prep(m, exefs, out)
        print("%-8s image=%#x relocs=%d other=%s syms=%d defined=%d init=%d" % (
            m, i["image_size"], i["nreloc"], i["nreloc_other"], i["nsym"], i["ndefined"], len(i["init_entries"])))


if __name__ == "__main__":
    main()
