"""AC2 of task-13: memory ordering and condition variable tests (tests/runtime/test_sync.c).

Builds the runtime (without main.c and without a game) plus the test, runs it and turns its TEST lines into a JSON report.
Under two minutes. Run: python tests/test_sync.py --report <path>
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "runtime"
WORK = Path(os.environ.get("SMRPG_TEST_DIR", r"A:\AI\Recomps\smrpg-work\build\test_sync"))
GEN = Path(os.environ.get("SMRPG_GEN_DIR", r"A:\AI\Recomps\smrpg-work\build\gen_game"))
TEST_PROCESS_FLAGS = subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", required=True)
    a = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    clang = shutil.which("clang")
    if not clang:
        raise SystemExit("clang not on PATH")
    exe = WORK / "test_sync.exe"
    srcs = [SRC / f for f in ("rt_core.c", "loader.c", "svc.c", "hle_nn.c", "rt_weak.c")] + sorted((SRC / "hle").glob("*.c")) + sorted((SRC / "eh").glob("*.c")) + sorted((SRC / "gc").glob("*.c")) + sorted((SRC / "mods").glob("*.c")) + [SRC / "platform" / "overlay" / "options.c", ROOT / "tools" / "datatbl" / "nrbf.c"]
    cmd = [clang, "-std=gnu11", "-O2", "-g", "-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-D_WIN32_WINNT=0x0A00",
           "-I", str(SRC), "-I", str(GEN)] + [str(s) for s in srcs] + [str(ROOT / "tests" / "runtime" / "test_sync.c"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=TEST_PROCESS_FLAGS)
    tests = []
    if r.returncode:
        tests.append({"name": "build test_sync", "status": "failed", "detail": (r.stderr or r.stdout)[-1500:]})
    else:
        try:
            p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=400, creationflags=TEST_PROCESS_FLAGS)
            out = p.stdout
            if p.returncode not in (0, 1):
                tests.append({"name": "test_sync process", "status": "failed", "detail": "exit %d\n%s" % (p.returncode, (out + p.stderr)[-800:])})
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            tests.append({"name": "test_sync process", "status": "failed", "detail": "timed out after 400 s"})
        for line in out.splitlines():
            m = re.match(r"TEST (.+) (passed|failed|skipped) (.*)$", line)
            if m:
                tests.append({"name": m.group(1), "status": m.group(2), "detail": m.group(3)})
        if not any(t["status"] == "passed" for t in tests):
            tests.append({"name": "test_sync produced results", "status": "failed", "detail": out[-500:]})
    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(json.dumps({"tests": tests}, indent=1), encoding="utf-8")
    bad = [t["name"] for t in tests if t["status"] == "failed"]
    for t in tests:
        print("%-8s %s: %s" % (t["status"], t["name"], t["detail"][:120]))
    print("sync tests: %s" % ("PASS" if not bad else "FAIL " + "; ".join(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
