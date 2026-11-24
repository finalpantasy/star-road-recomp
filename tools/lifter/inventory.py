r"""Function inventory per module for the lifter (task-5). Our own code.

main:    the task-2 inventory (analysis/il2cpp/functions.tsv, FDEs plus gap split), used as is (AC1 baseline).
sdk, subsdk0: derived here the same way: every FDE of .eh_frame_hdr, plus the FDE-less code gaps split by branch
targets, defined FUNC symbols and RELATIVE relocation addends that land in text.

Output: A:\AI\Recomps\smrpg-work\build\inv\<module>.tsv   start_rva  end_rva  source  (rva = module-relative)

  python -I tools/lifter/inventory.py            # all three
  python -I tools/lifter/inventory.py sdk
"""
import array
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "il2cpp"))
from ehframe import parse_fde_table  # noqa: E402
from functions import find_code_gaps, split_gap_functions  # noqa: E402

IMG = r"A:\AI\Recomps\smrpg-work\build\img"
INV = r"A:\AI\Recomps\smrpg-work\build\inv"
TASK2 = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp\functions.tsv"
NOP = 0xD503201F


def load_sym(path):
    b = open(path, "rb").read()
    cnt, strsz = struct.unpack_from("<II", b, 4)
    ents = []
    base = 16
    strs = base + cnt * 24
    for i in range(cnt):
        no, defined, bind, typ, _pad, val, size = struct.unpack_from("<IBBBBQQ", b, base + i * 24)
        e = strs + no
        name = b[e:b.index(b"\0", e)].decode("latin1")
        ents.append(dict(name=name, defined=defined, bind=bind, type=typ, value=val, size=size))
    return ents


def load_rel(path):
    b = open(path, "rb").read()
    cnt = struct.unpack_from("<I", b, 4)[0]
    return [struct.unpack_from("<QIIq", b, 16 + i * 24) for i in range(cnt)]


def derive(mod):
    j = json.load(open(os.path.join(IMG, mod + ".json")))
    tsz = j["text_size"]
    img = open(os.path.join(IMG, mod + ".img"), "rb").read()
    words = array.array("I")
    words.frombytes(img[:tsz])
    h0, h1 = j["mod0"]["eh_frame_hdr"]
    fdes, _info = parse_fde_table(img, h0, h1)
    fdes = [f for f in fdes if 0 <= f["start"] < tsz]
    fdes.sort(key=lambda f: f["start"])
    src = {}
    for f in fdes:
        src[f["start"]] = ("fde", f["start"] + f["size"])
    syms = load_sym(os.path.join(IMG, mod + ".sym"))
    sym_starts = {s["value"] for s in syms if s["defined"] and s["type"] == 2 and 0 < s["value"] < tsz}
    rel_starts = set()
    for off, typ, symi, addend in load_rel(os.path.join(IMG, mod + ".rel")):
        if typ == 1027 and 0 < addend < tsz and (addend & 3) == 0:
            rel_starts.add(addend)
    bl = set()
    for i, w in enumerate(words):
        t = w & 0xFC000000
        if t == 0x94000000 or t == 0x14000000:
            off = w & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            tg = (i + off) * 4
            if 0 <= tg < tsz:
                bl.add(tg)
    gaps = find_code_gaps(fdes, tsz, words)
    anchors = sym_starts | rel_starts
    for a, b in gaps:
        for s in split_gap_functions(a, b, words, bl, anchors):
            src[s] = ("gap", None)
    starts = sorted(src)
    out = []
    for k, s in enumerate(starts):
        kind, e = src[s]
        nxt = starts[k + 1] if k + 1 < len(starts) else tsz
        if e is None or e > nxt:
            e = nxt
        out.append((s, e, kind))
    stats = dict(fdes=len(fdes), gaps=len(gaps), gap_functions=sum(1 for o in out if o[2] == "gap"),
                 sym_starts_not_in_inventory=len(sym_starts - set(starts)),
                 rel_starts_not_in_inventory=len(rel_starts - set(starts)),
                 bl_targets_not_in_inventory=len(bl - set(starts)))
    return out, stats


def from_task2():
    out = []
    with open(TASK2) as f:
        for ln in f:
            if ln.startswith("rva"):
                continue
            p = ln.rstrip("\n").split("\t")
            if len(p) < 6 or not p[0].startswith("0x"):
                continue
            out.append((int(p[0], 16), int(p[1], 16), "task2:" + p[5]))
    out.sort()
    return out


def write(mod, rows):
    os.makedirs(INV, exist_ok=True)
    with open(os.path.join(INV, mod + ".tsv"), "w") as f:
        for s, e, k in rows:
            f.write("%x\t%x\t%s\n" % (s, e, k))


def load(mod):
    best = {}   # one row per start address; the longest wins when the sources list a start twice
    with open(os.path.join(INV, mod + ".tsv")) as f:
        for ln in f:
            s, e, k = ln.rstrip("\n").split("\t")
            s, e = int(s, 16), int(e, 16)
            if s not in best or e > best[s][0]:
                best[s] = (e, k)
    return [(s, best[s][0], best[s][1]) for s in sorted(best)]


if __name__ == "__main__":
    mods = sys.argv[1:] or ["main", "sdk", "subsdk0"]
    for m in mods:
        if m == "main":
            rows = from_task2()
            write(m, rows)
            d, st = derive(m)
            t2 = {r[0] for r in rows}
            mine = {r[0] for r in d}
            print("main: task-2 inventory %d functions; own derivation %d; only-in-task2 %d only-in-own %d; %s"
                  % (len(rows), len(d), len(t2 - mine), len(mine - t2), st))
        else:
            rows, st = derive(m)
            write(m, rows)
            print("%s: %d functions %s" % (m, len(rows), st))
