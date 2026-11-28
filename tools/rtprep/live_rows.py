r"""Print the per-row 'decomp live' table of one or more A/B logs, with row names from runtime/decomp/leaves.def (task-68). Our own code.

  python -I tools/rtprep/live_rows.py LOG [LOG ...]      (LOG = path or a name under A:\AI\Recomps\smrpg-work\build\logs)
Only named rows (QLEAF, ELEAF, LEAF; the ported ones) are listed, plus a count of probe rows (PLEAF) that were called.
Also prints the counts of 'decomp mismatch', 'decomp raced', IPC and interpreter-miss lines.
"""
import os
import re
import sys

LOGS = r"A:\AI\Recomps\smrpg-work\build\logs"
HERE = os.path.dirname(os.path.abspath(__file__))
DEF = os.path.join(HERE, "..", "..", "runtime", "decomp", "leaves.def")


def names():
    out, kind = {}, {}
    for line in open(DEF, encoding="utf-8"):
        m = re.match(r"\s*([A-Z]*LEAF)\(\s*\w+,\s*([0-9a-fA-F]+)\s*(?:,\s*(\w+))?", line)
        if m:
            addr = "0x" + m.group(2).lower()
            kind[addr] = m.group(1)
            if m.group(3) and not m.group(3).isdigit():
                out[addr] = m.group(3)
    return out, kind


def parse(path):
    calls, diffs, deferred = {}, {}, {}
    mism = raced = 0
    extra = []
    for line in open(path, errors="replace"):
        m = re.search(r"decomp live addr=(0x[0-9a-f]+) calls=(\d+) differences=(\d+)", line)
        if m:
            calls[m.group(1)] = int(m.group(2))
            diffs[m.group(1)] = int(m.group(3))
            continue
        m = re.search(r"decomp live addr=(0x[0-9a-f]+) deferred=(\d+)", line)
        if m:
            deferred[m.group(1)] = int(m.group(2))
            continue
        if "decomp mismatch" in line:
            mism += 1
        if "decomp raced total" in line or "raced total" in line:
            extra.append(line.strip()[:120])
        if "IPC:" in line and "distinct" in line:
            extra.append(line.strip()[:120])
        if "interpreter misses:" in line:
            extra.append(line.strip()[:120])
    return calls, diffs, deferred, mism, extra


def main():
    nm, kind = names()
    logs = [a if os.path.exists(a) else os.path.join(LOGS, a + ".log") for a in sys.argv[1:]]
    data = [parse(p) for p in logs]
    print("row".ljust(44), "addr".ljust(14), *[os.path.basename(p)[:-4].rjust(14) for p in logs])
    for addr, name in sorted(nm.items(), key=lambda kv: kv[1]):
        cells = []
        for calls, diffs, deferred, _, _ in data:
            c = calls.get(addr)
            if c is None:
                cells.append("-".rjust(14))
            else:
                s = f"{c}/{diffs[addr]}" + (f"+{deferred[addr]}d" if addr in deferred else "")
                cells.append(s.rjust(14))
        print(name.ljust(44), addr.ljust(14), *cells)
    for p, (calls, diffs, deferred, mism, extra) in zip(logs, data):
        probe = [a for a, c in calls.items() if kind.get(a) == "PLEAF" and c > 0]
        print(f"{os.path.basename(p)}: mismatch lines {mism}; probe rows called {len(probe)}; named rows with diffs "
              f"{[a for a in nm if diffs.get(a, 0)]}")
        for e in extra:
            print("   ", e)


if __name__ == "__main__":
    main()
