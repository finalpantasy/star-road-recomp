"""Check randomized and live evidence from a single hidden game run.

  python -I runtime/decomp/check_log.py LOG            strict: every row needs selftest and live calls (hand-written rows)
  python -I runtime/decomp/check_log.py --summary LOG  every row needs a clean selftest; live calls may be 0 (reported)

The strict mode fails on a function the run never called. The summary mode (task-45) keeps every other check and
counts live against selftest-only functions, per row source (hand-written, generated) and per generated pattern.
"""
from pathlib import Path
import collections
import re
import sys

ROW = r"(?<!O)[SCGNVD]?LEAF\(\w+, ([0-9a-f]+),"
GEN_DIR = Path(r"A:\AI\Recomps\smrpg-work\decompgen")


def addresses():
    manual = re.findall(ROW, Path(__file__).with_name("leaves.def").read_text())
    generated = []
    if (GEN_DIR / "gen.def").exists():
        generated = re.findall(ROW, (GEN_DIR / "gen.def").read_text())
    return manual, generated


def check_header(log):
    if "decomp mismatch" in log:
        raise ValueError("a recorded mismatch")
    if not re.search(r"EXIT code=(?:0\b|4 \(watchdog timeout\))", log):
        raise ValueError("missing successful exit or timed watchdog stop")


def evidence(log, address):
    test = rf"decomp selftest addr=0x{address} random=10000 edge=(\d+) calls=(\d+) differences=0$"
    live = rf"decomp live addr=0x{address} calls=(\d+) differences=(\d+)$"
    tests = re.findall(test, log, re.MULTILINE)
    hits = re.findall(live, log, re.MULTILINE)
    if len(tests) != 1 or int(tests[0][1]) != 10000 + int(tests[0][0]) or len(hits) != 1:
        raise ValueError(f"incomplete randomized or live coverage for 0x{address}")
    return tests[0], int(hits[0][0]), int(hits[0][1])


def check(log):
    manual, generated = addresses()
    if not manual:
        raise ValueError("empty manifest")
    check_header(log)
    for address in manual + generated:
        tests, calls, differences = evidence(log, address)
        if calls < 1 or differences:
            raise ValueError(f"incomplete randomized or live coverage for 0x{address}")
        print(f"0x{address} random=10000 edge={tests[0]} live={calls} differences=0")


def summary(log):
    manual, generated = addresses()
    if not manual:
        raise ValueError("empty manifest")
    check_header(log)
    pattern = {}
    index = GEN_DIR / "gen.index.tsv"
    if index.exists():
        for line in index.read_text(encoding="utf-8").splitlines():
            f = line.split("\t")
            pattern[f[0][2:]] = f[1]
    groups = collections.defaultdict(lambda: [0, 0, 0, 0])   # rows, live, selftest only, live calls
    differences_total = 0
    for source, rows in (("hand-written", manual), ("generated", generated)):
        for address in rows:
            _tests, calls, differences = evidence(log, address)   # raises on a missing or failed selftest
            differences_total += differences
            keys = [source] + ([f"  {pattern.get(address, '?')}"] if source == "generated" else [])
            for key in keys:
                g = groups[key]
                g[0] += 1
                g[1] += calls > 0
                g[2] += calls == 0
                g[3] += calls
    print(f"{'group':<18} {'rows':>5} {'live':>5} {'selftest-only':>14} {'live calls':>12}")
    for key in sorted(groups, key=lambda k: (not k.startswith("hand"), k.startswith("  "), k)):
        r = groups[key]
        print(f"{key:<18} {r[0]:>5} {r[1]:>5} {r[2]:>14} {r[3]:>12}")
    raced = re.findall(r"decomp raced addr=0x([0-9a-f]+)", log)
    print(f"selftest clean for {len(manual) + len(generated)} rows; live differences: {differences_total}; "
          f"raced loads (retry agreed): {len(raced)}" + (" at " + " ".join(sorted(set(raced))) if raced else ""))
    if differences_total:
        raise ValueError("live differences")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--summary":
        summary(Path(args[1]).read_text(errors="replace"))
    else:
        check(Path(args[0]).read_text(errors="replace"))
