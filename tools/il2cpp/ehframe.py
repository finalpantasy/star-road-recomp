"""eh_frame / eh_frame_hdr parsing: function ranges and LSDA presence per FDE.

Our own implementation of the public DWARF call-frame / .eh_frame_hdr encodings.
"""
import struct

DW_EH_PE_omit = 0xFF


def _uleb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, i


def _sleb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, i


def read_encoded(img, i, enc, base_for_rel=None, data_base=0):
    """Read a DW_EH_PE encoded value at image offset i. Returns (value, new_i).
    pcrel is relative to the address of the field itself (image offsets are module RVAs)."""
    if enc == DW_EH_PE_omit:
        return None, i
    fmt = enc & 0x0F
    app = enc & 0x70
    start = i
    if fmt == 0x00:
        v = struct.unpack_from("<Q", img, i)[0]; i += 8
    elif fmt == 0x01:
        v, i = _uleb(img, i)
    elif fmt == 0x02:
        v = struct.unpack_from("<H", img, i)[0]; i += 2
    elif fmt == 0x03:
        v = struct.unpack_from("<I", img, i)[0]; i += 4
    elif fmt == 0x04:
        v = struct.unpack_from("<Q", img, i)[0]; i += 8
    elif fmt == 0x09:
        v, i = _sleb(img, i)
    elif fmt == 0x0A:
        v = struct.unpack_from("<h", img, i)[0]; i += 2
    elif fmt == 0x0B:
        v = struct.unpack_from("<i", img, i)[0]; i += 4
    elif fmt == 0x0C:
        v = struct.unpack_from("<q", img, i)[0]; i += 8
    else:
        raise ValueError("unsupported pointer encoding %#x" % enc)
    if app == 0x10:  # pcrel
        v += start
    elif app == 0x30:  # datarel
        v += data_base
    elif app not in (0x00,):
        raise ValueError("unsupported pointer application %#x" % enc)
    return v & 0xFFFFFFFFFFFFFFFF, i


def parse_cie(img, off):
    """Return dict(aug, fde_enc, lsda_enc, personality, has_z)."""
    length = struct.unpack_from("<I", img, off)[0]
    i = off + 8  # past length and CIE id
    version = img[i]; i += 1
    e = img.find(b"\0", i)
    aug = bytes(img[i:e]).decode(); i = e + 1
    if version >= 4:
        i += 2
    _caf, i = _uleb(img, i)
    _daf, i = _sleb(img, i)
    if version == 1:
        i += 1
    else:
        _, i = _uleb(img, i)
    cie = {"aug": aug, "fde_enc": 0, "lsda_enc": DW_EH_PE_omit, "personality": None}
    if aug.startswith("z"):
        alen, i = _uleb(img, i)
        end = i + alen
        for ch in aug[1:]:
            if ch == "R":
                cie["fde_enc"] = img[i]; i += 1
            elif ch == "L":
                cie["lsda_enc"] = img[i]; i += 1
            elif ch == "P":
                penc = img[i]; i += 1
                cie["personality"], i = read_encoded(img, i, penc)
            elif ch == "S" or ch == "B":
                pass
        i = end
    return cie


def parse_fde_table(img, hdr_start, hdr_end):
    """Parse .eh_frame_hdr. Returns list of dicts sorted by start:
    {start, size, fde, lsda(bool), personality}. Also returns the header info."""
    ver, ptr_enc, cnt_enc, tab_enc = struct.unpack_from("<4B", img, hdr_start)
    i = hdr_start + 4
    eh_frame_ptr, i = read_encoded(img, i, ptr_enc, data_base=hdr_start)
    count, i = read_encoded(img, i, cnt_enc, data_base=hdr_start)
    out = []
    cies = {}
    for k in range(count):
        if tab_enc == 0x3B:  # datarel sdata4
            a, b = struct.unpack_from("<ii", img, i + 8 * k)
            start = hdr_start + a
            fde = hdr_start + b
        else:
            raise ValueError("unsupported table encoding %#x" % tab_enc)
        flen = struct.unpack_from("<I", img, fde)[0]
        cie_ptr = struct.unpack_from("<I", img, fde + 4)[0]
        cie_off = fde + 4 - cie_ptr
        cie = cies.get(cie_off)
        if cie is None:
            cie = cies[cie_off] = parse_cie(img, cie_off)
        j = fde + 8
        pc, j = read_encoded(img, j, cie["fde_enc"])
        rng, j = read_encoded(img, j, cie["fde_enc"] & 0x0F)
        lsda = None
        if cie["aug"].startswith("z"):
            alen, j = _uleb(img, j)
            if cie["lsda_enc"] != DW_EH_PE_omit and alen:
                lsda, _ = read_encoded(img, j, cie["lsda_enc"])
        out.append({"start": pc, "size": rng, "fde": fde, "lsda": lsda, "personality": cie["personality"],
                    "aug": cie["aug"], "fde_len": flen})
    out.sort(key=lambda r: r["start"])
    info = {"version": ver, "eh_frame_ptr": eh_frame_ptr, "count": count, "ncies": len(cies),
            "personalities": sorted({c["personality"] for c in cies.values() if c["personality"] is not None})}
    return out, info
