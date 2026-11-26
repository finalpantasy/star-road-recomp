"""Decode the A64 AES, SHA-1, SHA-256, PMULL and CRC instruction encodings."""


def decode_lower(w, pc):
    rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
    if w & 0x7FE0E000 == 0x1AC04000:
        size = (w >> 10) & 3
        if (w >> 31) == (size == 3):
            return "crc32", dict(rd=rd, rn=rn, rm=rm, size=1 << size, castagnoli=(w >> 12) & 1)
    if w & 0xFFFF0C00 == 0x4E280800:
        names = {4: "aese", 5: "aesd", 6: "aesmc", 7: "aesimc"}
        name = names.get((w >> 12) & 15)
        if name:
            return name, dict(rd=rd, rn=rn)
    if w & 0xFFFE0C00 == 0x5E280800:
        names = {0: "sha1h", 1: "sha1su1", 2: "sha256su0"}
        name = names.get((w >> 12) & 31)
        if name:
            return name, dict(rd=rd, rn=rn)
    if w & 0xFFE08C00 == 0x5E000000:
        names = ("sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1")
        kind = (w >> 12) & 7
        if kind < len(names):
            return names[kind], dict(rd=rd, rn=rn, rm=rm)
    if w & 0xBF20FC00 == 0x0E20E000:
        size = (w >> 22) & 3
        if size in (0, 3):
            return "pmull", dict(rd=rd, rn=rn, rm=rm, high=(w >> 30) & 1, wide=int(size == 3))
    return None
