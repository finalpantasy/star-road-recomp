"""Task-4 spike driver: sample real functions from main_v101.elf, lift them to C in several
variants, write chunked C files plus stubs under the work folder. Our own code.

usage: python -I tools/spike/lift.py --elf <main_v101.elf> --funcs <functions.tsv> --out <dir>
                                     [--sample 4000] [--seed 20261124] [--chunk 100]
Nothing written here contains game bytes in the repo: output goes to --out (on A:).
"""
import argparse
import collections
import json
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a64 import Unsupported, decode  # noqa: E402
from emit_c import Fn  # noqa: E402

BASE = 0x7100000000
VARIANTS = {"ctx": dict(model="ctx"), "local_naive": dict(model="local", abi_flush=False), "local": dict(model="local"), "local_exc": dict(model="local", exc=True),
            "local_poll": dict(model="local", poll=True)}


def load_text(elf):
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for k in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, shoff + k * shentsize)
        secs.append((name, addr, off, size))
    stroff = secs[shstrndx][2]
    for name, addr, off, size in secs:
        nm = data[stroff + name:data.index(b"\0", stroff + name)]
        if nm == b".text":
            return data, addr, off, size
    raise SystemExit("no .text")


def read_funcs(path):
    out = []
    with open(path, encoding="utf-8") as f:
        hdr = f.readline().rstrip("\n").split("\t")
        for line in f:
            r = dict(zip(hdr, line.rstrip("\n").split("\t")))
            out.append((int(r["rva"], 16), int(r["end_rva"], 16), r["class"], r["has_lsda"] == "1"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--funcs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sample", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=20261124)
    ap.add_argument("--chunk", type=int, default=100)
    ap.add_argument("--max-bytes", type=int, default=65536)
    a = ap.parse_args()

    data, taddr, toff, tsize = load_text(a.elf)
    assert taddr == BASE
    funcs = read_funcs(a.funcs)
    rng = random.Random(a.seed)
    sample = sorted(rng.sample(funcs, a.sample))
    known = {BASE + s for s, _, _, _ in funcs}

    t0 = time.perf_counter()
    decoded, fails = [], collections.Counter()
    by_class = collections.defaultdict(lambda: [0, 0, 0, 0])  # sampled, lifted, bytes sampled, bytes lifted
    insn_count = 0
    for rva, end, cls, lsda in sample:
        size = end - rva
        by_class[cls][0] += 1
        by_class[cls][2] += size
        if size <= 0 or size > a.max_bytes or size % 4:
            fails["size"] += 1
            continue
        words = struct.unpack_from("<%dI" % (size // 4), data, toff + rva)
        try:
            ins = [(BASE + rva + 4 * k, decode(w, BASE + rva + 4 * k)) for k, w in enumerate(words)]
        except Unsupported as e:
            fails[str(e).split(" ")[0]] += 1
            continue
        decoded.append((rva, end, cls, lsda, ins))
        by_class[cls][1] += 1
        by_class[cls][3] += size
        insn_count += len(ins)
    t_decode = time.perf_counter() - t0

    manifest = dict(sample=a.sample, seed=a.seed, lifted=len(decoded), insns=insn_count, decode_s=t_decode,
                    fail_reasons=fails.most_common(25), by_class={k: v for k, v in sorted(by_class.items())},
                    variants={})
    leaf = []
    for vname, opts in VARIANTS.items():
        vdir = os.path.join(a.out, vname)
        os.makedirs(vdir, exist_ok=True)
        t1 = time.perf_counter()
        bodies, callees, lifted = [], set(), []
        for rva, end, cls, lsda, ins in decoded:
            fn = Fn(BASE + rva, BASE + end, None, known=known, **opts)
            try:
                body = fn.emit(ins)
            except Unsupported as e:
                fails["emit " + str(e).split(" ")[0]] += 1
                continue
            bodies.append(body)
            callees |= fn.callees
            lifted.append(BASE + rva)
            if vname == "local":
                has_sys = any(i["op"] in ("svc", "brk") for _, i in ins)
                if not fn.callees and not fn.uses_dispatch and not has_sys:
                    leaf.append(dict(addr=BASE + rva, cls=cls, insns=len(ins)))
        t_emit = time.perf_counter() - t1
        nchunks = 0
        total_c = 0
        for k in range(0, len(bodies), a.chunk):
            chunk = bodies[k:k + a.chunk]
            path = os.path.join(vdir, "chunk_%04d.c" % (k // a.chunk))
            text = '#include "spike_rt.h"\n#include "decls.h"\n\n' + "\n".join(chunk)
            with open(path, "w", encoding="ascii") as f:
                f.write(text)
            total_c += len(text)
            nchunks += 1
        all_syms = set(lifted) | callees
        with open(os.path.join(vdir, "decls.h"), "w", encoding="ascii") as f:
            f.write("#pragma once\n" + "".join("void f_%x(Ctx *restrict c);\n" % s for s in sorted(all_syms)))
        missing = sorted(callees - set(lifted))
        with open(os.path.join(vdir, "stubs.c"), "w", encoding="ascii") as f:
            f.write('#include "spike_rt.h"\n#include "decls.h"\nvoid rt_missing(Ctx *c, u64 a);\n')
            f.write("".join("void f_%x(Ctx *restrict c) { rt_missing(c, 0x%xULL); }\n" % (s, s) for s in missing))
            f.write("const u64 g_fn_addr[] = {%s};\n" % ",".join("0x%xULL" % s for s in lifted))
            f.write("void (*const g_fn_ptr[])(Ctx *restrict) = {%s};\n" % ",".join("f_%x" % s for s in lifted))
            f.write("const u32 g_fn_count = %d;\n" % len(lifted))
        manifest["variants"][vname] = dict(functions=len(lifted), emit_s=t_emit, chunks=nchunks, c_bytes=total_c,
                                           missing_callees=len(missing))
    manifest["leaf"] = leaf
    manifest["fail_reasons"] = fails.most_common(30)
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(json.dumps({k: v for k, v in manifest.items() if k not in ("leaf",)}, indent=1))
    print("leaf functions:", len(leaf))


if __name__ == "__main__":
    main()
