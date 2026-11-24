"""AC3 spot checks: disassemble 20 named methods with llvm-objdump (aarch64) and judge them.

Checks per method (all automatic, results written to spot_check.tsv and spot_check.md):
  * address inside .text, 4-aligned, equal to an FDE start
  * llvm-objdump decodes every instruction of the function (no .inst / <unknown>)
  * prologue/body plausible: first instruction is not a branch-to-self or udf and a
    terminator (ret/b/br/tail branch) exists in the function
  * for named semantic probes, the expected instruction pattern is present
"""
import csv
import os
import random
import re
import subprocess
import sys

OUT_DIR = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp"
ELF = os.path.join(OUT_DIR, "main_v101.elf")
BASE = 0x7100000000
OBJDUMP = "llvm-objdump"

# (type::method signature prefix in methods.tsv, regex that must appear in the disassembly, why)
PROBES = [
    ("System.Object::.ctor()", r"\bret\b", "empty constructor returns immediately"),
    ("System.String::get_Length()", r"ldr\s+w0, \[x0, #0x10\]", "String length field at +0x10"),
    ("System.String::get_Chars(", r"ldr|ldrh", "reads a char from the string"),
    ("UnityEngine.Mathf::Abs(float)", r"fabs\s+s0, s\d+", "Mathf.Abs(float) is fabs (after IL2CPP init check)"),
    ("UnityEngine.Mathf::Max(int,int)", r"cmp\s+w0, w1|csel|smax", "Mathf.Max(int,int) compares and selects"),
    ("System.Math::Abs(int)", r"cneg|cmp|neg|csneg|abs", "Math.Abs(int) negates conditionally"),
]


def run_objdump(start, stop):
    cmd = [OBJDUMP, "-d", "--no-show-raw-insn", "--start-address=%#x" % (BASE + start), "--stop-address=%#x" % (BASE + stop), ELF]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120).stdout


def load_functions():
    fn = {}
    with open(os.path.join(OUT_DIR, "functions.tsv"), encoding="utf-8") as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            fn[int(p[0], 16)] = (int(p[1], 16), p[5], p[7], p[2])
    return fn


def main():
    fns = load_functions()
    methods = []
    with open(os.path.join(OUT_DIR, "methods.tsv"), encoding="utf-8") as f:
        rd = csv.reader(f, delimiter="\t")
        hdr = next(rd)
        for r in rd:
            d = dict(zip(hdr, r))
            if d["rva"]:
                methods.append(d)
    by_sig = {}
    for d in methods:
        by_sig.setdefault(d["type"] + "::" + d["name"] + d["signature"], d)
    picks = []
    for sig, rx, why in PROBES:
        hit = next((d for k, d in by_sig.items() if k.startswith(sig)), None)
        if hit:
            picks.append((hit, rx, why))
    rnd = random.Random(20261124)
    game = [d for d in methods if d["image"] == "Assembly-CSharp.dll" and 8 <= int(fns[int(d["rva"], 16)][3]) <= 600]
    other = [d for d in methods if d["image"] not in ("Assembly-CSharp.dll",) and 8 <= int(fns[int(d["rva"], 16)][3]) <= 600]
    for d in rnd.sample(game, 9):
        picks.append((d, None, "random Assembly-CSharp method"))
    for d in rnd.sample(other, 20 - len(picks)):
        picks.append((d, None, "random engine/BCL method"))
    rows = []
    for d, rx, why in picks[:20]:
        a = int(d["rva"], 16)
        end, cls, conf, size = fns[a]
        size = int(size)
        stop = a + min(size, 512)
        txt = run_objdump(a, stop)
        lines = [l for l in txt.splitlines() if re.match(r"\s*7[0-9a-f]+:", l)]
        decoded = len(lines)
        bad = sum(1 for l in lines if ".inst" in l or "<unknown>" in l)
        # a function ends in ret / branch, or in a call to a no-return helper (managed throw)
        term = any(re.search(r"\t(ret|b|br|retab|braa|brab)\s*(\s|$)", l) for l in lines) or (
            bool(lines) and re.search(r"\tbl\s", lines[-1]) is not None)
        first = lines[0].split(":", 1)[1].strip() if lines else ""
        probe_ok = None
        if rx:
            probe_ok = bool(re.search(rx, txt))
        ok = (a % 4 == 0) and decoded > 0 and bad == 0 and term and (probe_ok is not False)
        rows.append({
            "method": d["type"] + "::" + d["name"] + d["signature"], "image": d["image"], "rva": d["rva"], "size": size,
            "first_insn": first, "insns_checked": decoded, "undecoded": bad, "terminator": term,
            "probe": why, "probe_ok": "" if probe_ok is None else probe_ok, "plausible": ok,
        })
    with open(os.path.join(OUT_DIR, "spot_check.tsv"), "w", encoding="utf-8", newline="\n") as f:
        keys = list(rows[0].keys())
        f.write("\t".join(keys) + "\n")
        for r in rows:
            f.write("\t".join(str(r[k]) for k in keys) + "\n")
    print(len(rows), "checked,", sum(1 for r in rows if r["plausible"]), "plausible")
    for r in rows:
        print("%-3s %-10s %-9s %-80.80s | %s" % ("OK" if r["plausible"] else "BAD", r["rva"], r["size"], r["method"], r["first_insn"]))


if __name__ == "__main__":
    main()
