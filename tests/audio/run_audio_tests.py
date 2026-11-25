r"""Star Road Recomp audio tests (task-27). Our own code.

usage: python -I tests/audio/run_audio_tests.py
Builds and runs tests/audio/test_hwopus.c (hwopus decode through libopus, PCM dump and stats helpers) and, when present, the audren mixer
tests. Clang, no SDL; BelowNormal and hidden. Exit 0 pass, 1 fail, 3 not run (no clang). libopus-0.dll comes from SMRPG_OPUS_DLL or the
MSYS2 ucrt64 bin (see runtime/audio/hwopus_dec.h).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = r"A:\AI\Recomps\smrpg-work\build\audio_tests"
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000
TESTS = ["test_hwopus", "test_audren_mix"]


def main():
    clang = shutil.which("clang")
    if not clang:
        print("NOT_RUN: clang not found")
        return 3
    os.makedirs(OUT, exist_ok=True)
    rc = 0
    for name in TESTS:
        src = os.path.join(ROOT, "tests", "audio", name + ".c")
        if not os.path.exists(src):
            continue
        exe = os.path.join(OUT, name + ".exe")
        cmd = [clang, "-O1", "-g", "-Wall", "-Wextra", "-std=gnu11", src, "-o", exe]
        p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW)
        if p.returncode:
            print(p.stdout + p.stderr)
            print("FAIL: build", name)
            rc = 1
            continue
        if p.stderr.strip():
            print(p.stderr.strip())
        r = subprocess.run([exe], capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW, timeout=120)
        print(r.stdout.strip())
        if r.returncode:
            print("FAIL:", name, "exit", r.returncode)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
