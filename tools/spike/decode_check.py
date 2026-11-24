"""Task-4 spike: cross-check our decoder against llvm-objdump on sampled functions. Our own code.

Compares, per instruction, the decoded branch targets, ADRP/ADR values and a coarse mnemonic class.
usage: python -I tools/spike/decode_check.py --elf <main_v101.elf> --funcs <functions.tsv> --out <json> [--n 300]
"""
import argparse
import json
import os
import random
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a64 import Unsupported, decode  # noqa: E402
from lift import BASE, load_text, read_funcs  # noqa: E402

# our op -> llvm mnemonic prefixes it may print (aliases included)
CLASS = {
    "b": ("b",), "bl": ("bl",), "bcond": ("b.",), "cbz": ("cbz", "cbnz"), "tbz": ("tbz", "tbnz"), "br": ("br",),
    "blr": ("blr",), "ret": ("ret",), "adrp": ("adrp",), "adr": ("adr",), "svc": ("svc",), "brk": ("brk",),
    "ldp": ("ldp", "ldnp", "ldpsw"), "stp": ("stp", "stnp"), "ldr": ("ldr", "ldur", "ldtr"),
    "str": ("str", "stur", "sttr"), "ldr_lit": ("ldr", "ldrsw"), "ldxr": ("ldxr", "ldaxr"),
    "stxr": ("stxr", "stlxr"), "ldar": ("ldar",), "stlr": ("stlr",), "dmb": ("dmb", "dsb"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--funcs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=300)
    a = ap.parse_args()
    data, _, toff, _ = load_text(a.elf)
    funcs = random.Random(7).sample(read_funcs(a.funcs), a.n)
    checked = mism = 0
    bad = []
    for rva, end, _, _ in funcs:
        if end - rva > 4096:
            continue
        out = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", "--start-address=0x%x" % (BASE + rva),
                              "--stop-address=0x%x" % (BASE + end), a.elf], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
            if not m:
                continue
            pc, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
            w, = struct.unpack_from("<I", data, toff + pc - BASE)
            try:
                i = decode(w, pc)
            except Unsupported:
                continue
            checked += 1
            ok = True
            want = CLASS.get(i["op"])
            if want and not any(mn == p or (p.endswith(".") and mn.startswith(p)) or mn.startswith(p) for p in want):
                ok = False
            if "target" in i:
                t = re.findall(r"0x([0-9a-f]+)", ops.split("<")[0])  # last hex is the target (tbz prints the bit first)
                if not t or int(t[-1], 16) != i["target"]:
                    ok = False
            if i["op"] in ("adrp", "adr"):
                t = re.search(r"0x([0-9a-f]+)", ops)
                if not t or int(t.group(1), 16) != i["val"]:
                    ok = False
            if not ok:
                mism += 1
                if len(bad) < 20:
                    bad.append(dict(pc="%x" % pc, objdump="%s %s" % (mn, ops), ours=i))
    res = dict(functions=len(funcs), insns_checked=checked, mismatches=mism, examples=bad)
    json.dump(res, open(a.out, "w"), indent=1, default=str)
    print(json.dumps(dict(res, examples=bad[:5]), indent=1, default=str))


if __name__ == "__main__":
    main()
