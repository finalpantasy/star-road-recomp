r"""Build and run task-25 codec and scaling tests against the owner's A: table fixtures."""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATA = r"A:\AI\Recomps\smrpg-work\v101\romfs\Data\StreamingAssets\data_tbl"
OUT = r"A:\AI\Recomps\smrpg-work\build\datatbl_tests"


def main():
    if len(sys.argv) > 1:
        data = os.path.abspath(sys.argv[1])
    else:
        data = DATA
    if not os.path.isdir(data) or not any(n.lower().endswith(".tbl") for n in os.listdir(data)):
        print("NOT_RUN: A: data_tbl fixtures are unavailable")
        return 3
    if not os.path.isfile(os.path.join(ROOT, "tools", "datatbl", "nrbf.c")):
        print("RED: tools/datatbl/nrbf.c is not implemented")
        return 1
    clang = shutil.which("clang")
    if not clang:
        print("NOT_RUN: clang is unavailable")
        return 3
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "test_difficulty_plus.exe")
    cmd = [clang, "-O1", "-g", "-Wall", "-Wextra", "-std=gnu11",
           os.path.join(ROOT, "tests", "mods", "test_difficulty_plus.c"),
           os.path.join(ROOT, "tools", "datatbl", "nrbf.c"),
           os.path.join(ROOT, "runtime", "mods", "datatbl_overlay.c"),
           os.path.join(ROOT, "runtime", "platform", "overlay", "options.c"), "-o", exe]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode:
        print("BUILD FAILED (difficulty_plus tests)\n" + p.stdout[-4000:])
        return 1
    p = subprocess.run([exe, data, OUT], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(p.stdout, end="")
    return p.returncode


if __name__ == "__main__":
    raise SystemExit(main())
