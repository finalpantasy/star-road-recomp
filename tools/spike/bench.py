"""Task-4 spike: build and run the leaf-function bench in several configurations. Our own code.

usage: python -I tools/spike/bench.py --lift <lift dir> --elf <main_v101.elf> --funcs <functions.tsv> --out <dir>
Runs hidden (no console window), BelowNormal priority, one process at a time.
"""
import argparse
import glob
import json
import math
import os
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = dict(BELOW_NORMAL=0x00004000, NO_WINDOW=0x08000000)
CONFIGS = [  # name, lift variant, defines
    ("poll_screen", "local_poll", []),
    ("ctx", "ctx", []),
    ("local", "local", []),
    ("local_masked", "local", ["-DMEM_MASKED=1"]),
    ("local_relaxed", "local", ["-DACCESS_RELAXED=1"]),
    ("local_poll", "local_poll", []),
]


def leaf_source(lift, variant, leaf, dst):
    want = {"%x" % a for a in leaf}
    out, addrs = ['#include "spike_rt.h"\n#include "decls.h"\n'], []
    for cfile in sorted(glob.glob(os.path.join(lift, variant, "chunk_*.c"))):
        for f in open(cfile, encoding="ascii").read().split("\nvoid f_")[1:]:
            a = f[:f.index("(")]
            if a in want:
                out.append("void f_" + f)
                addrs.append(a)
    out.append("const u64 g_fn_addr[] = {%s};\n" % ",".join("0x%sULL" % a for a in addrs))
    out.append("void (*const g_fn_ptr[])(Ctx *restrict) = {%s};\n" % ",".join("f_" + a for a in addrs))
    out.append("const u32 g_fn_count = %d;\n" % len(addrs))
    open(dst, "w", encoding="ascii").write("\n".join(out))


def run(cmd, timeout):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          creationflags=FLAGS["BELOW_NORMAL"] | FLAGS["NO_WINDOW"])


def load(path):
    rows, extra = {}, []
    for line in open(path):
        r = json.loads(line)
        (rows.__setitem__(r["addr"], r) if "addr" in r else extra.append(r))
    return rows, extra


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lift", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--funcs", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    man = json.load(open(os.path.join(a.lift, "manifest.json")))
    leaf = [r["addr"] for r in man["leaf"]]
    with open(os.path.join(a.out, "leaf_all.txt"), "w") as f:
        f.write("\n".join("%x" % x for x in leaf))
    with open(os.path.join(a.out, "all_addrs.txt"), "w") as f:
        for line in list(open(a.funcs))[1:]:
            f.write("%x\n" % (0x7100000000 + int(line.split("\t")[0], 16)))
    exes = {}
    for name, variant, defs in CONFIGS:
        src = os.path.join(a.out, "leaf_%s.c" % variant)
        if not os.path.exists(src):
            leaf_source(a.lift, variant, leaf, src)
        exe = os.path.join(a.out, "bench_%s.exe" % name)
        cmd = ["clang", "-O2", "-march=x86-64-v3", "-fno-strict-aliasing", "-w", "-I", HERE, "-I",
               os.path.join(a.lift, variant)] + defs + [os.path.join(HERE, "bench_rt.c"), src, "-o", exe]
        p = run(cmd, 300)
        if p.returncode:
            raise SystemExit(p.stderr[-3000:])
        exes[name] = exe
    # 1. screen in the poll build: watchdog plus back-edge safepoints stop runaway loops; faults are caught.
    scr = os.path.join(a.out, "screen.jsonl")
    p = run([exes["poll_screen"], a.elf, "screen", os.path.join(a.out, "leaf_all.txt"), scr], 600)
    rows, _ = load(scr)
    ok = [k for k, r in rows.items() if r["ok"]]
    with open(os.path.join(a.out, "leaf_ok.txt"), "w") as f:
        f.write("\n".join(ok))
    res = dict(leaf=len(leaf), screened_ok=len(ok), screened_fault=len(rows) - len(ok), configs={})
    # 2. time each configuration on the survivors and compare end-state hashes.
    results = {}
    for rep in range(3):  # interleave configurations; keep the per-function minimum
        for name, _, _ in CONFIGS[1:]:
            outp = os.path.join(a.out, "time_%s_%d.jsonl" % (name, rep))
            extra_args = [os.path.join(a.out, "all_addrs.txt")] if name == "local" and rep == 0 else []
            run([exes[name], a.elf, "time", os.path.join(a.out, "leaf_ok.txt"), outp] + extra_args, 600)
            rws, extra = load(outp)
            if name not in results:
                results[name] = (rws, extra)
                continue
            old, oextra = results[name]
            for k, r in rws.items():
                if k in old and r["ok"] and old[k]["ok"] and 0 <= r["ns"] < old[k]["ns"]:
                    old[k]["ns"] = r["ns"]
                elif k in old and not r["ok"]:
                    old[k]["ok"] = 0
    base_rows = results["local"][0]
    common = [k for k in ok if all(k in results[n][0] and results[n][0][k]["ok"] for n in results)]
    res["timed_common"] = len(common)
    for name, (rws, extra) in results.items():
        ns = [rws[k]["ns"] for k in common]
        big = [k for k in common if base_rows[k]["ns"] >= 2.0]
        ratios = [max(rws[k]["ns"], 0.05) / max(base_rows[k]["ns"], 0.05) for k in big]
        mism = sum(1 for k in common if rws[k]["hash"] != base_rows[k]["hash"])
        res["configs"][name] = dict(sum_ns=round(sum(ns), 1), median_ns=round(statistics.median(ns), 3),
                                    geomean_vs_local_2ns_plus=round(math.exp(sum(map(math.log, ratios)) / len(ratios)), 3),
                                    n_2ns_plus=len(big), sum_ns_2ns_plus=round(sum(rws[k]["ns"] for k in big), 1),
                                    hash_mismatch_vs_local=mism,
                                    extra=extra)
    json.dump(res, open(os.path.join(a.out, "bench.json"), "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
