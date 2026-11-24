"""Lifter check (task-5), for tools/game_check.py. Our own code. Under 10 minutes, no game data needed except for the
optional lift smoke (skipped when the prepared images are absent).

  1. runtime boot test with a mock game library (tests/test_runtime_boot.py)
  2. instruction oracle, small run: interpreter ops vs lifted C vs Unicorn on real instruction words (AC2)
  3. lift smoke: lift the first chunks of sdk and compile one of them with clang -fsyntax-only

Run: python tests/test_lifter.py --report <path>
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV_PY = Path(r"A:\AI\Recomps\tools-ext\venv-unicorn\Scripts\python.exe")
IMG = Path(r"A:\AI\Recomps\smrpg-work\build\img")
REPORT_DIR = Path(r"A:\AI\Recomps\smrpg-work\build\tests")
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000


def run(argv, timeout):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, creationflags=BELOW_NORMAL | NO_WINDOW,
                       cwd=str(ROOT))
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", required=True)
    ap.add_argument("--per-op", type=int, default=12)
    ap.add_argument("--states", type=int, default=4)
    a = ap.parse_args()
    tests = []

    def record(name, status, detail=""):
        tests.append({"name": name, "status": status, "detail": detail[-400:]})

    for _attempt in range(2):   # the runtime reserves a large address range; another heavy process can make it fail once
        rc, out = run([sys.executable, "-I", str(ROOT / "tests" / "test_runtime_boot.py")], 300)
        if rc == 0:
            break
    record("runtime boot test (mock game)", "passed" if rc == 0 else "failed", out)

    if not VENV_PY.exists() or not (IMG / "main.text.bin").exists():
        record("instruction oracle vs Unicorn", "skipped", "Unicorn venv or prepared images missing")
        record("lift and compile smoke", "skipped", "prepared images missing")
    else:
        rc, out = run([str(VENV_PY), str(ROOT / "tests" / "oracle" / "oracle.py"), "--per-op", str(a.per_op), "--states",
                       str(a.states), "--tag", "check"], 540)
        rep = REPORT_DIR / "report_check.json"
        detail = out
        status = "failed"
        if rc == 0 and rep.exists():
            j = json.loads(rep.read_text())
            detail = "compared %d mismatches %d skipped by unicorn %d" % (j["compared"], j["mismatches"], j["skipped_by_unicorn"])
            status = "passed" if j["mismatches"] == 0 and j["compared"] > 1000 else "failed"
        record("instruction oracle vs Unicorn", status, detail)

        code = (
            "import sys,os;sys.path.insert(0,r'%s');import build;"
            "build.write_prelude();"
            "r=build.lift_chunk(('sdk',0));print(r['lifted'],len(r['failed']),r['path'])" % (ROOT / "tools" / "lifter")
        )
        rc, out = run([sys.executable, "-I", "-c", code], 120)
        path = out.strip().split()[-1] if rc == 0 and out.strip() else ""
        if rc != 0 or not path.endswith(".c"):
            record("lift and compile smoke", "failed", out)
        else:
            gen = Path(r"A:\AI\Recomps\smrpg-work\build\gen_game")
            rc, out2 = run(["clang", "-fsyntax-only", "-w", "-std=gnu11", "-march=x86-64-v3", "-I", str(ROOT / "runtime"),
                            "-I", str(gen), path], 300)
            record("lift and compile smoke", "passed" if rc == 0 else "failed", out.strip() + " | " + out2)

    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(json.dumps({"tests": tests}, indent=1), encoding="utf-8")
    bad = [t for t in tests if t["status"] == "failed"]
    for t in tests:
        print(t["status"].upper(), t["name"], "-", t["detail"][:120].replace("\n", " "))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
