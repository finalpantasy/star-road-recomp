"""Build the function list and per-function features; cache to A: as a pickle.

Usage: python -I tools/il2cpp/build_features.py
Writes <outdir>/features.pkl (derived data, never committed).
"""
import bisect
import os
import pickle
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_tables import Model, OUT_DEFAULT  # noqa: E402
from ehframe import parse_fde_table  # noqa: E402
import functions as fx  # noqa: E402


def build(M):
    nso, reg = M.nso, M.reg
    img = M.img
    tsize = nso.text.size
    words = fx.text_words(img, tsize)
    fdes, info = parse_fde_table(img, nso.eh_frame_hdr_start, nso.eh_frame_hdr_end)
    syms = nso.dynsyms()
    # import slots: GOT/JUMP_SLOT relocs against undefined symbols -> slot address -> name
    got_map = {}
    for r_off, rtype, si, add in nso.relocs():
        if rtype in (1025, 1026, 257) and syms[si][4] == 0:
            got_map[r_off] = syms[si][0]
    t = time.time()
    bl, bb, adrp_idx = fx.scan_text(words, tsize, None, None, got_map, None)
    print("scan %.1fs: bl=%d b=%d adrp=%d" % (time.time() - t, len(bl), len(bb), len(adrp_idx)))
    bl_targets = {tg for _, tg in bl}
    anchors = set()
    for p in M.method_ptr:
        if p:
            anchors.add(p)
    for s in syms:
        if s[4] and s[1] < tsize:
            anchors.add(s[1])
    gaps = fx.find_code_gaps(fdes, tsize, words)
    starts = {f["start"] for f in fdes}
    gap_funcs = []
    for a, b in gaps:
        for s in fx.split_gap_functions(a, b, words, bl_targets, anchors):
            gap_funcs.append((s, b))
    # build function table: (start, end, has_fde, lsda)
    funcs = []
    for f in fdes:
        funcs.append([f["start"], f["start"] + f["size"], 1, 1 if f["lsda"] else 0])
    gs = sorted(gap_funcs)
    for k, (s, bend) in enumerate(gs):
        e = gs[k + 1][0] if k + 1 < len(gs) and gs[k + 1][0] < bend else bend
        # trim trailing nop/zero padding
        j = e // 4
        while j > s // 4 + 1 and words[j - 1] in (0, fx.NOP):
            j -= 1
        funcs.append([s, j * 4, 0, 0])
    funcs.sort()
    fstarts = [f[0] for f in funcs]

    def func_of(addr):
        k = bisect.bisect_right(fstarts, addr) - 1
        if k >= 0 and addr < funcs[k][1]:
            return k
        return -1

    ro = nso.rodata
    ro_img = img[ro.mem_off:ro.mem_off + ro.size]
    data_lo = nso.data_seg.mem_off
    data_hi = nso.data_seg.mem_off + nso.data_seg.size
    nf = len(funcs)
    strs = [None] * nf  # list of rodata string addrs per function
    gots = [None] * nf
    dataref = [None] * nf
    t = time.time()
    str_cache = {}
    for i, kind, addr in fx.adrp_targets(words, adrp_idx):
        k = func_of(i * 4)
        if k < 0:
            continue
        if ro.mem_off <= addr < ro.mem_off + ro.size:
            if addr not in str_cache:
                str_cache[addr] = fx.printable_cstr(ro_img, addr, ro.mem_off, ro.size)
            if str_cache[addr] is not None:
                if strs[k] is None:
                    strs[k] = []
                strs[k].append(addr)
        elif data_lo <= addr < data_hi + nso.bss_size:
            if kind == "ld" and addr in got_map:
                if gots[k] is None:
                    gots[k] = []
                gots[k].append(got_map[addr])
            else:
                if dataref[k] is None:
                    dataref[k] = []
                dataref[k].append(addr)
    print("refs %.1fs: funcs=%d, with strings=%d, with got=%d" % (
        time.time() - t, nf, sum(1 for s in strs if s), sum(1 for s in gots if s)))
    calls = [None] * nf
    for i, tg in bl:
        k = func_of(i * 4)
        c = func_of(tg)
        if k >= 0 and c >= 0:
            if calls[k] is None:
                calls[k] = set()
            calls[k].add(c)
    tails = [None] * nf  # tail-call (B) to another function
    for i, tg in bb:
        k = func_of(i * 4)
        c = func_of(tg)
        if k >= 0 and c >= 0 and c != k:
            if tails[k] is None:
                tails[k] = set()
            tails[k].add(c)
    strings = {a: s for a, s in str_cache.items() if s is not None}
    return {
        "funcs": funcs, "gaps": gaps, "strs": strs, "gots": gots, "dataref": dataref, "calls": calls, "tails": tails,
        "strings": strings, "n_bl": len(bl), "n_b": len(bb), "n_adrp": len(adrp_idx), "got_map": got_map,
        "fde_info": info,
    }


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT
    t0 = time.time()
    M = Model()
    feats = build(M)
    with open(os.path.join(out, "features.pkl"), "wb") as f:
        pickle.dump(feats, f, protocol=4)
    print("done %.1fs" % (time.time() - t0), len(feats["funcs"]))
