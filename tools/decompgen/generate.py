r"""Bulk generator for exact trivial-function ports (task-45). Our own code, stdlib only.

Scans every function of the main module (the lifter's inventory and decoder), matches the closed pattern set in
patterns.py, ranks the matches by direct call sites and writes, for the chosen ones:

  gen.rs         Rust, one function per match (included by decomp/smrpg_decomp through build.rs)
  gen.def        manifest rows (LEAF, GLEAF, NLEAF, SLEAF) appended to runtime/decomp/leaves.def at configure time
  gen.index.tsv  address, pattern, variant, symbol, chunk, call sites, name (read by check_log.py --summary)
  gen.report.txt match counts per pattern over the whole binary

The output is derived from the game binary (addresses, field offsets, method names), so it never enters Git: the
default output directory is on A: next to the lifted chunks and the build pulls it from there.

  python -I tools/decompgen/generate.py                 # top 300 by direct call sites, at most 12 lifted chunks
  python -I tools/decompgen/generate.py --limit 1000 --max-chunks 40
  python -I tools/decompgen/generate.py --stats-only    # counts only, writes only the report
"""
import argparse
import array
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "lifter"))
import dec  # noqa: E402  the lifter's decoder
import inventory as inv  # noqa: E402  the lifter's function inventory (one row per function start)
import patterns  # noqa: E402

WORK = r"A:\AI\Recomps\smrpg-work"
IMG = os.path.join(WORK, "build", "img")
METHODS = os.path.join(WORK, "analysis", "il2cpp", "methods.tsv")
CDIR = os.path.join(WORK, "build", "c")
DEFAULT_OUT = os.path.join(WORK, "decompgen")
BASE = 0x7100000000      # main module base, same as tools/lifter/build.py BASES["main"]
CHUNK = 400              # functions per lifted chunk, same as tools/lifter/build.py CHUNK
MAX_INSNS = 5


def load_text():
    meta = json.load(open(os.path.join(IMG, "main.json")))
    img = open(os.path.join(IMG, "main.img"), "rb").read()
    words = array.array("I")
    words.frombytes(img[:meta["text_size"]])
    return words


def call_sites(words, starts):
    """Direct bl and b instructions (from other code) per function start: {address: count}."""
    counts = collections.Counter()
    n = len(words)
    for i in range(n):
        w = words[i]
        top = w >> 26
        if top == 0x25 or top == 0x05:          # bl, b
            off = w & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            t = BASE + ((i + off) << 2)
            if t in starts and t != BASE + (i << 2):
                counts[t] += 1
    return counts


def method_names():
    """{address: 'Type.method'} from the IL2CPP method map; shared code lists the first method and a count."""
    names = {}
    shared = collections.Counter()
    try:
        f = open(METHODS, encoding="utf-8")
    except OSError:
        return names
    with f:
        header = f.readline().rstrip("\n").split("\t")
        col = {n: i for i, n in enumerate(header)}
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) <= col["rva"] or not p[col["rva"]]:
                continue
            addr = BASE + int(p[col["rva"]], 16)
            shared[addr] += 1
            names.setdefault(addr, "%s.%s" % (p[col["type"]], p[col["name"]]))
    return {a: n if shared[a] == 1 else "%s (+%d shared)" % (n, shared[a] - 1) for a, n in names.items()}


def scan():
    words = load_text()
    rows = inv.load("main")
    found = []
    for index, (s, e, _kind) in enumerate(rows):
        count = (e - s) // 4
        if count < 1 or count > MAX_INSNS or (e - s) % 4:
            continue
        pc = BASE + s
        insns = []
        for k in range(count):
            try:
                insns.append(dec.decode(words[s // 4 + k], pc + 4 * k))
            except dec.Unsupported:
                break
        if len(insns) != count:
            continue
        spec = patterns.match(insns, pc)
        if spec:
            found.append((pc, index // CHUNK, spec))
    starts = {BASE + s for s, e, k in rows}
    return words, found, starts


def pick(found, sites, limit, max_chunks):
    """Highest call-site count first, but only functions of the best `max_chunks` lifted chunks: every chunk with
    a swapped function is recompiled by the build (about 3 MB of C each)."""
    if max_chunks:
        weight = collections.Counter()
        for pc, chunk, spec in sorted(found, key=lambda f: -sites.get(f[0], 0))[:limit]:
            weight[chunk] += sites.get(pc, 0) + 1
        keep = {c for c, _w in weight.most_common(max_chunks)}
        found = [f for f in found if f[1] in keep]
    ranked = sorted(found, key=lambda f: (-sites.get(f[0], 0), f[0]))
    return ranked[:limit]


def used(body, name):
    return re.search(r"\b%s\b" % name, body) is not None


def rust_function(pc, spec, name):
    sym = symbol(pc, spec)
    params = ", ".join("%s: u64" % c if used(spec.rust_body, c) else "_: u64" for c in "abc")
    if spec.kind == "SLEAF":
        params += ", delta: u64"
    unsafe = "unsafe " if spec.kind in ("GLEAF", "NLEAF", "SLEAF") else ""
    note = "%s (%s %s)" % (name or "unnamed", spec.pattern, spec.variant)
    body = spec.rust_body
    if "\n" in body:
        body = "\n    ".join(body.split("\n"))
    return "/// 0x%x: %s\n#[unsafe(no_mangle)]\npub %sextern \"C\" fn %s(%s) -> u64 {\n    %s\n}\n" % (
        pc, note, unsafe, sym, params, body)


def symbol(pc, spec):
    return "smrpg_g_%s_%x" % (spec.pattern, pc)


def manifest_row(pc, chunk, spec):
    sym = symbol(pc, spec)
    head = "%s(main_%04d, %x, %s" % (spec.kind, chunk, pc, sym)
    if spec.kind in ("GLEAF", "NLEAF", "SLEAF"):
        head += ", %d, %d, %d" % (spec.base_reg, spec.offset, spec.nbytes)
    return head + ")"


def hand_written():
    """Addresses already swapped by hand in runtime/decomp/leaves.def (a function must not be declared twice)."""
    text = open(os.path.join(HERE, "..", "..", "runtime", "decomp", "leaves.def")).read()
    return {int(x, 16) for x in re.findall(r"[SCGN]?LEAF\(\w+, ([0-9a-f]+),", text)}


def chunk_has_definition(pc, chunk, cache):
    """The wrapper needs exactly one definition of the function in its lifted chunk file."""
    path = os.path.join(CDIR, "main_%04d.c" % chunk)
    if path not in cache:
        try:
            cache[path] = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            cache[path] = None
    text = cache[path]
    if text is None:
        return True      # no lifted chunk on disk (stats or unit-test machine): trust the inventory
    return text.count("void f_%x(Ctx *restrict c) {" % pc) == 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--limit", type=int, default=300, help="functions to generate, by direct call sites")
    ap.add_argument("--max-chunks", type=int, default=12, help="at most this many lifted chunks (0 = no cap)")
    ap.add_argument("--stats-only", action="store_true")
    a = ap.parse_args()

    words, found, starts = scan()
    sites = call_sites(words, starts)
    names = method_names()
    totals = collections.Counter(s.pattern for _p, _c, s in found)
    variants = collections.Counter((s.pattern, s.variant) for _p, _c, s in found)
    cache = {}
    manual = hand_written()
    ok = [f for f in found if f[0] not in manual and chunk_has_definition(f[0], f[1], cache)]
    chosen = pick(ok, sites, a.limit, a.max_chunks)
    chosen_counts = collections.Counter(s.pattern for _p, _c, s in chosen)
    chunks = sorted({c for _p, c, _s in chosen})

    report = ["main module: %d functions of at most %d instructions matched a pattern (%d already hand-written)" % (
                  len(found), MAX_INSNS, sum(1 for f in found if f[0] in manual)),
              "with a direct call site: %d" % sum(1 for p, _c, _s in found if sites.get(p)),
              "%-14s %8s %10s %9s" % ("pattern", "matches", "with sites", "generated")]
    for fam in patterns.FAMILIES:
        withs = sum(1 for p, _c, s in found if s.pattern == fam and sites.get(p))
        report.append("%-14s %8d %10d %9d" % (fam, totals[fam], withs, chosen_counts[fam]))
    report.append("variants:")
    for (fam, var), n in sorted(variants.items()):
        report.append("  %-14s %-10s %d" % (fam, var, n))
    report.append("generated %d functions in %d lifted chunks: %s" % (len(chosen), len(chunks), " ".join("main_%04d" % c for c in chunks)))
    os.makedirs(a.out, exist_ok=True)
    text = "\n".join(report) + "\n"
    open(os.path.join(a.out, "gen.report.txt"), "w", newline="\n").write(text)
    print(text, end="")
    if a.stats_only:
        return 0

    chosen.sort(key=lambda f: f[0])
    rs = ["// @generated by tools/decompgen/generate.py. Do not edit; it is rebuilt from the owner's own dump.",
          "// Derived from the game binary, so it lives outside Git (see docs/DECOMP.md).",
          "use super::*;", ""]
    rows, index = [], []
    for pc, chunk, spec in chosen:
        rs.append(rust_function(pc, spec, names.get(pc)))
        rows.append(manifest_row(pc, chunk, spec))
        index.append("\t".join(map(str, ("0x%x" % pc, spec.pattern, spec.variant, symbol(pc, spec), "main_%04d" % chunk,
                                         sites.get(pc, 0), names.get(pc, "")))))
    open(os.path.join(a.out, "gen.rs"), "w", newline="\n").write("\n".join(rs))
    open(os.path.join(a.out, "gen.def"), "w", newline="\n").write("\n".join(rows) + "\n")
    open(os.path.join(a.out, "gen.index.tsv"), "w", newline="\n", encoding="utf-8").write("\n".join(index) + "\n")
    print("wrote", os.path.join(a.out, "gen.rs"), len(chosen), "functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
