r"""Star Road Recomp unlock_postgame tests (task-24). Our own code.

usage: python -I tests/mods/run_postgame_tests.py
Builds tests/mods/test_postgame.c with runtime/mods/postgame.c (clang, no SDL), runs it with the save copies under
A:\AI\Recomps\smrpg-work\save\test_postgame\, BelowNormal and hidden. Exit 0 pass, 1 fail, 3 not run (no clang).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = r"A:\AI\Recomps\smrpg-work\build\postgame_tests"
SAVES = r"A:\AI\Recomps\smrpg-work\save\test_postgame"
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000


def main():
    clang = shutil.which("clang")
    if not clang:
        print("NOT_RUN: clang not found")
        return 3
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(SAVES, exist_ok=True)
    exe = os.path.join(OUT, "test_postgame.exe")
    cmd = [clang, "-O1", "-g", "-Wall", "-Wextra", "-std=gnu11", os.path.join(ROOT, "tests", "mods", "test_postgame.c"),
           os.path.join(ROOT, "runtime", "mods", "postgame.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW)
    if p.returncode:
        print(p.stdout + p.stderr)
        print("FAIL: build")
        return 1
    if p.stderr.strip():
        print(p.stderr.strip())
    r = subprocess.run([exe, SAVES], capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW, timeout=120)
    print(r.stdout.strip())
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
