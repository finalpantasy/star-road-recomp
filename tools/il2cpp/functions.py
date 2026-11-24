"""Function inventory and per-function features for the NSO .text segment.

Function set = every FDE in .eh_frame_hdr  +  split of the few FDE-less code regions
(by branch targets, dynsym/method-pointer anchors and prologue patterns).

Features per function (all derived from the code and data, nothing guessed):
  size, has_fde, lsda, string refs (ADRP+ADD/LDR into rodata that land on a C string),
  GOT import refs (ADRP+LDR into a relocated import slot), direct BL/B targets.
"""
import array
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ehframe import parse_fde_table  # noqa: E402

NOP = 0xD503201F


def text_words(img, size):
    a = array.array("I")
    a.frombytes(bytes(img[:size]))
    if sys.byteorder != "little":
        a.byteswap()
    return a


def find_code_gaps(fdes, text_size, words):
    """Uncovered regions of .text that contain something other than zero/nop words."""
    gaps = []
    prev = 0
    for f in fdes:
        if f["start"] > prev:
            gaps.append((prev, f["start"]))
        prev = max(prev, f["start"] + f["size"])
    if prev < text_size:
        gaps.append((prev, text_size))
    out = []
    for a, b in gaps:
        seg = words[a // 4:b // 4]
        if any(w not in (0, NOP) for w in seg):
            out.append((a, b))
    return out


def is_prologue(w, w2):
    # stp x29,x30,[sp,#-N]!  (pre-index)  or  sub sp,sp,#imm  or  stp ..,[sp,#-N]!
    if (w & 0xFFC003E0) == 0xA98003E0:  # stp xA,xB,[sp,#imm]!
        return True
    if (w & 0xFF0003FF) == 0xD10003FF:  # sub sp,sp,#imm
        return True
    return False


def split_gap_functions(a, b, words, bl_targets, anchors):
    """Split an FDE-less code region into function starts."""
    starts = {a}
    for t in bl_targets:
        if a <= t < b:
            starts.add(t)
    for t in anchors:
        if a <= t < b:
            starts.add(t)
    # a start also follows a `ret`/unconditional branch plus nop padding
    i = a // 4
    end = b // 4
    prev_term = False
    while i < end:
        w = words[i]
        if w == NOP or w == 0:
            i += 1
            continue
        if prev_term:
            starts.add(i * 4)
        prev_term = (w == 0xD65F03C0) or ((w & 0xFC000000) == 0x14000000) or ((w & 0xFFFFFC1F) == 0xD61F0000)
        i += 1
    return sorted(s for s in starts if a <= s < b and words[s // 4] not in (0, NOP))


def scan_text(words, text_size, rodata_rng, ro_img, got_map, func_starts):
    """One pass over the code: branch targets and ADRP-based data references.

    Returns dict with:
      bl: list of (src_index, target) for BL; b: list of (src_index, target) for unconditional B
      refs: dict func_start -> {'str': set(addr), 'got': set(slot)}
    """
    n = len(words)
    bl = []
    bb = []
    adrp_idx = []
    for i in range(n):
        w = words[i]
        t = w & 0xFC000000
        if t == 0x94000000:
            off = w & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            bl.append((i, (i + off) * 4))
        elif t == 0x14000000:
            off = w & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            bb.append((i, (i + off) * 4))
        elif (w & 0x9F000000) == 0x90000000:
            adrp_idx.append(i)
    return bl, bb, adrp_idx


def adrp_targets(words, adrp_idx, window=6):
    """Yield (instr_index, kind, address) for ADRP + ADD/LDR/STR/LDRB.. pairs.

    kind: 'add' (address formed) or 'ld' (data load from page+offset, scaled)."""
    n = len(words)
    for i in adrp_idx:
        w = words[i]
        rd = w & 31
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000:
            imm -= 0x200000
        page = ((i * 4) & ~0xFFF) + (imm << 12)
        # look at following instructions for use of rd as base
        for j in range(i + 1, min(i + 1 + window, n)):
            x = words[j]
            # add xd, xn, #imm12 (64-bit, no shift flag set)
            if (x & 0xFFC00000) in (0x91000000, 0x91400000) and ((x >> 5) & 31) == rd:
                imm12 = (x >> 10) & 0xFFF
                if (x & 0xFFC00000) == 0x91400000:
                    imm12 <<= 12
                yield i, "add", page + imm12
                break
            # ldr xt, [xn, #imm12*8]
            if (x & 0xFFC00000) == 0xF9400000 and ((x >> 5) & 31) == rd:
                yield i, "ld", page + ((x >> 10) & 0xFFF) * 8
                break
            # ldr/str other widths with base rd -> treat as data ref in the page
            if (x & 0x3B000000) == 0x39000000 and ((x >> 5) & 31) == rd:
                sz = (x >> 30) & 3
                yield i, "ld", page + (((x >> 10) & 0xFFF) << sz)
                break
            if (x & 31) == rd and (x & 0xFC000000) not in (0x94000000,):
                # rd overwritten by a plain register write (approximate); stop scanning
                if (x & 0x7F800000) in (0x52800000, 0x12800000, 0x72800000):
                    break


def printable_cstr(ro, addr, ro_base, ro_size, minlen=3, maxlen=400):
    """Return the C string at rodata address if it looks like text, else None."""
    off = addr - ro_base
    if not (0 <= off < ro_size):
        return None
    end = ro.find(b"\0", off, off + maxlen)
    if end < 0 or end - off < minlen:
        return None
    b = ro[off:end]
    # printable ASCII plus common whitespace and UTF-8 continuation bytes
    for c in b:
        if c < 9 or (13 < c < 32) or c == 127:
            return None
    return b.decode("utf-8", "replace")
