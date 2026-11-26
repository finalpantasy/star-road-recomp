"""Check randomized and live evidence from a single hidden game run."""
from pathlib import Path
import re
import sys


def check(log):
    manifest = Path(__file__).with_name("leaves.def").read_text()
    addresses = re.findall(r"S?LEAF\(\w+, ([0-9a-f]+),", manifest)
    if not addresses or "decomp mismatch" in log:
        raise ValueError("empty manifest or a recorded mismatch")
    if not re.search(r"EXIT code=(?:0\b|4 \(watchdog timeout\))", log):
        raise ValueError("missing successful exit or timed watchdog stop")
    for address in addresses:
        test = rf"decomp selftest addr=0x{address} random=10000 edge=(\d+) calls=(\d+) differences=0$"
        live = rf"decomp live addr=0x{address} calls=([1-9]\d*) differences=0$"
        hits = re.findall(live, log, re.MULTILINE)
        tests = re.findall(test, log, re.MULTILINE)
        if len(tests) != 1 or int(tests[0][1]) != 10000 + int(tests[0][0]) or len(hits) != 1:
            raise ValueError(f"incomplete randomized or live coverage for 0x{address}")
        print(f"0x{address} random=10000 edge={tests[0][0]} live={hits[0]} differences=0")


if __name__ == "__main__":
    check(Path(sys.argv[1]).read_text(errors="replace"))
