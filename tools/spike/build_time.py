"""Task-4 spike: compile the lifted chunks and measure time and object size. Our own code.

usage: python -I tools/spike/build_time.py --lift <dir from lift.py> --out <results dir> [--jobs 16]
Every clang runs at BelowNormal priority. Results go to <out>/build_time.json.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import re
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BELOW_NORMAL = 0x00004000
BASE_FLAGS = ["-c", "-march=x86-64-v3", "-fno-strict-aliasing", "-w", "-I", HERE]


def run(cmd):
    t = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL)
    dt = time.perf_counter() - t
    if p.returncode:
        raise RuntimeError("%s\n%s" % (" ".join(cmd), p.stderr[-3000:]))
    return dt


def text_bytes(objs):
    total = 0
    for k in range(0, len(objs), 200):
        out = subprocess.run(["llvm-size", "-A"] + objs[k:k + 200], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"^\.text\S*\s+(\d+)", line)
            if m:
                total += int(m.group(1))
    return total


def compile_set(srcs, outdir, flags, jobs, inc):
    os.makedirs(outdir, exist_ok=True)
    cmds = []
    for s in srcs:
        o = os.path.join(outdir, os.path.splitext(os.path.basename(s))[0] + ".o")
        cmds.append((["clang"] + BASE_FLAGS + ["-I", inc] + flags + [s, "-o", o], o))
    t = time.perf_counter()
    with cf.ThreadPoolExecutor(jobs) as ex:
        cpu = sum(ex.map(lambda c: run(c[0]), cmds))
    wall = time.perf_counter() - t
    objs = [o for _, o in cmds]
    return dict(files=len(srcs), wall_s=round(wall, 2), cpu_s=round(cpu, 2), obj_bytes=sum(os.path.getsize(o) for o in objs),
                text_bytes=text_bytes(objs)), objs


def split_functions(chunk_files, n, dst):
    os.makedirs(dst, exist_ok=True)
    out = []
    for cfile in chunk_files:
        text = open(cfile, encoding="ascii").read()
        head, *funcs = text.split("\nvoid f_")
        for k, f in enumerate(funcs):
            p = os.path.join(dst, "one_%05d.c" % len(out))
            open(p, "w", encoding="ascii").write(head + "\nvoid f_" + f)
            out.append(p)
            if len(out) >= n:
                return out
    return out


def merge(chunk_files, per, dst):
    os.makedirs(dst, exist_ok=True)
    out = []
    for k in range(0, len(chunk_files), per):
        p = os.path.join(dst, "big_%04d.c" % (k // per))
        with open(p, "w", encoding="ascii") as f:
            for cfile in chunk_files[k:k + per]:
                f.write(open(cfile, encoding="ascii").read() + "\n")
        out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lift", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=16)
    a = ap.parse_args()
    man = json.load(open(os.path.join(a.lift, "manifest.json")))
    res = dict(jobs=a.jobs, functions=man["lifted"], insns=man["insns"], runs={})

    def chunks(v):
        return sorted(glob.glob(os.path.join(a.lift, v, "chunk_*.c")))

    def rec(name, srcs, flags, inc_variant, nfuncs):
        r, objs = compile_set(srcs, os.path.join(a.out, "obj", name), flags, a.jobs, os.path.join(a.lift, inc_variant))
        r["functions"] = nfuncs
        r["cpu_s_per_1k_fn"] = round(r["cpu_s"] / nfuncs * 1000, 2)
        res["runs"][name] = r
        print(name, r, flush=True)
        return objs

    nf = man["lifted"]
    for v in ("ctx", "local_naive", "local", "local_exc", "local_poll"):
        rec(v + "_O2", chunks(v), ["-O2"], v, nf)
    rec("local_exc_O1", chunks("local_exc"), ["-O1"], "local_exc", nf)
    rec("local_exc_O0", chunks("local_exc"), ["-O0"], "local_exc", nf)

    # LLVM IR proxy: what a backend that writes .ll directly would pay (frontend removed).
    lldir = os.path.join(a.out, "ll")
    os.makedirs(lldir, exist_ok=True)
    lls = []
    t = time.perf_counter()
    cmds = []
    for s in chunks("local_exc"):
        ll = os.path.join(lldir, os.path.basename(s)[:-2] + ".ll")
        cmds.append(["clang", "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-march=x86-64-v3",
                     "-fno-strict-aliasing", "-w", "-I", HERE, "-I", os.path.join(a.lift, "local_exc"), s, "-o", ll])
        lls.append(ll)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        fe_cpu = sum(ex.map(run, cmds))
    res["runs"]["ll_frontend_only"] = dict(cpu_s=round(fe_cpu, 2), wall_s=round(time.perf_counter() - t, 2),
                                           ll_bytes=sum(os.path.getsize(p) for p in lls))
    rec("ll_to_obj_O2", lls, ["-O2"], "local_exc", nf)

    # chunking: one function per file (first 300) versus 100 and 400 per file
    ones = split_functions(chunks("local_exc"), 300, os.path.join(a.out, "split1"))
    rec("local_exc_O2_1perfile_300fn", ones, ["-O2"], "local_exc", len(ones))
    bigs = merge(chunks("local_exc"), 4, os.path.join(a.out, "merge400"))
    rec("local_exc_O2_400perfile", bigs, ["-O2"], "local_exc", nf)

    json.dump(res, open(os.path.join(a.out, "build_time.json"), "w"), indent=1)
    shutil.rmtree(os.path.join(a.out, "split1"), ignore_errors=True)


if __name__ == "__main__":
    main()
