"""Checks the AArch64 crypto-extension helpers the interpreter uses (runtime/crypto_a64.c) against published AES, SHA-1, SHA-256 and
PMULL vectors (task-13). Run: python -I tests/test_crypto_a64.py --report <path>"""
import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(os.environ.get("SMRPG_TEST_DIR", "A:/AI/Recomps/smrpg-work/build/test_crypto"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", required=True)
    a = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    clang = shutil.which("clang")
    if not clang:
        raise SystemExit("clang not on PATH")
    exe = WORK / "test_crypto_a64.exe"
    r = subprocess.run([clang, "-O1", "-w", str(ROOT / "tests" / "runtime" / "test_crypto_a64.c"), str(ROOT / "runtime" / "crypto_a64.c"), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        tests = [{"name": "build test_crypto_a64", "status": "failed", "detail": (r.stderr or r.stdout)[-1500:]}]
    else:
        p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, creationflags=0x00004000)
        ok = p.returncode == 0 and "TEST crypto_a64 passed" in p.stdout
        tests = [{"name": "crypto_a64 vectors", "status": "passed" if ok else "failed", "detail": p.stdout.strip()[-400:]}]
    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(json.dumps({"tests": tests}, indent=1), encoding="utf-8")
    for t in tests:
        print("%-8s %s: %s" % (t["status"], t["name"], t["detail"][:120]))
    return 0 if all(t["status"] == "passed" for t in tests) else 1


if __name__ == "__main__":
    raise SystemExit(main())
