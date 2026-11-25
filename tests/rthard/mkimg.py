"""Turn an AArch64 ELF shared object into the runtime's module files (task-14 test bed). Our own code.

  python -I tests/rthard/mkimg.py <in.so> <outdir> <module name>

Writes <name>.img (the loaded image, base 0), .json (the fields runtime/loader.c reads, plus mod0 and the function list
the test lifter uses), .sym (SYM0: the dynamic symbols), .rel (REL0: the dynamic relocations). Word 0 of the image
becomes `b +8` and word 1 the MOD0 offset (8), as in a Horizon NSO; MOD0 sits in the ELF header bytes, which the
runtime never reads. The test modules are ours (built from tests/rthard/guest), nothing from the game.
"""
import json
import os
import struct
import sys

PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME = 1, 2, 0x6474E550
DT = dict(RELA=7, RELASZ=8, JMPREL=23, PLTRELSZ=2, SYMTAB=6, STRTAB=5, STRSZ=10, INIT_ARRAY=25, INIT_ARRAYSZ=27,
          FINI_ARRAY=26, FINI_ARRAYSZ=28)


def main(src, out, name):
    b = open(src, "rb").read()
    assert b[:4] == b"\x7fELF" and b[4] == 2, "not ELF64"
    e_phoff, e_shoff = struct.unpack_from("<QQ", b, 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", b, 0x36)
    phs = [struct.unpack_from("<IIQQQQQQ", b, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    loads = [p for p in phs if p[0] == PT_LOAD]
    size = max(p[3] + p[6] for p in loads)
    size = (size + 0xFFF) & ~0xFFF
    img = bytearray(size)
    text_end = 0
    for typ, flags, off, va, _pa, fsz, msz, _al in loads:
        img[va:va + fsz] = b[off:off + fsz]
        if flags & 1:
            text_end = max(text_end, va + fsz)
    text_end = (text_end + 3) & ~3
    dyn = {}
    for p in phs:
        if p[0] == PT_DYNAMIC:
            for i in range(0, p[5], 16):
                tag, val = struct.unpack_from("<qQ", b, p[2] + i)
                if tag == 0:
                    break
                dyn[tag] = val
    eh = [p for p in phs if p[0] == PT_GNU_EH_FRAME]
    eh0, eh1 = (eh[0][3], eh[0][3] + eh[0][6]) if eh else (0, 0)
    # sections: function symbols from .symtab (the test lifter's inventory)
    shs = [struct.unpack_from("<IIQQQQIIQQ", b, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    funcs = []
    for sh in shs:
        if sh[1] == 2:  # SHT_SYMTAB
            strs = shs[sh[6]]
            for i in range(sh[5] // 24):
                st_name, st_info, _o, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", b, sh[4] + i * 24)
                if st_info & 0xF == 2 and st_shndx and st_size:
                    nm = b[strs[4] + st_name:b.index(b"\0", strs[4] + st_name)].decode()
                    funcs.append((st_value, st_value + st_size, nm))
    # PLT entries (16 bytes after the 32-byte header) are functions too: `bl` to an import lands on one
    shstr = shs[e_shstrndx]
    for sh in shs:
        nm = b[shstr[4] + sh[0]:b.index(b"\0", shstr[4] + sh[0])].decode()
        if nm == ".plt":
            for a in range(sh[3] + 32, sh[3] + sh[5], 16):
                funcs.append((a, a + 16, "plt"))
    funcs = sorted(set(funcs))
    # dynamic symbols
    symtab, strtab, strsz = dyn[DT["SYMTAB"]], dyn[DT["STRTAB"]], dyn[DT["STRSZ"]]
    nsym = (strtab - symtab) // 24 if strtab > symtab else 0
    syms = []
    for i in range(nsym):
        st_name, st_info, _o, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", img, symtab + i * 24)
        syms.append((st_name, 1 if st_shndx else 0, st_info >> 4, st_info & 0xF, st_value, st_size))
    sb = bytearray(b"SYM0" + struct.pack("<III", len(syms), strsz, 0))
    for st_name, d, bind, typ, val, sz in syms:
        sb += struct.pack("<IBBBBQQ", st_name, d, bind, typ, 0, val, sz)
    sb += img[strtab:strtab + strsz]
    rels = []
    for a, n in ((DT["RELA"], DT["RELASZ"]), (DT["JMPREL"], DT["PLTRELSZ"])):
        if a in dyn:
            for i in range(0, dyn[n], 24):
                off, info, add = struct.unpack_from("<QQq", img, dyn[a] + i)
                rels.append((off, info & 0xFFFFFFFF, info >> 32, add))
    rb = bytearray(b"REL0" + struct.pack("<III", len(rels), 0, 0))
    for off, typ, sym, add in rels:
        rb += struct.pack("<QIIq", off, typ, sym, add)
    # Horizon module header
    struct.pack_into("<II", img, 0, 0x14000002, 8)             # b +8 ; MOD0 offset
    struct.pack_into("<4siiiiii", img, 8, b"MOD0", 0, 0, 0, eh0 - 8, eh1 - 8, 0)
    js = dict(module=name, image_size=size, text_size=text_end, rodata_off=0, rodata_size=0, data_off=0, data_size=0,
              init_array=dict(off=dyn.get(DT["INIT_ARRAY"], 0), size=dyn.get(DT["INIT_ARRAYSZ"], 0)),
              fini_array=dict(off=dyn.get(DT["FINI_ARRAY"], 0), size=dyn.get(DT["FINI_ARRAYSZ"], 0)),
              mod0=dict(off=8, eh_frame_hdr=[eh0, eh1]), functions=[[s, e, n] for s, e, n in funcs])
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, name + ".img"), "wb").write(img)
    open(os.path.join(out, name + ".sym"), "wb").write(sb)
    open(os.path.join(out, name + ".rel"), "wb").write(rb)
    json.dump(js, open(os.path.join(out, name + ".json"), "w"), indent=1)
    return js


if __name__ == "__main__":
    j = main(sys.argv[1], sys.argv[2], sys.argv[3])
    print("%s: image %#x text %#x functions %d eh_frame_hdr %s" % (sys.argv[3], j["image_size"], j["text_size"],
                                                                   len(j["functions"]), j["mod0"]["eh_frame_hdr"]))
