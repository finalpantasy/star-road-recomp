"""Texture path tests (task-11): builds runtime/gfx/texture with CMake (LLVM-MinGW clang) and runs texture_unit.

No game data is used: texture_unit generates every input. The corpus checks (survey, round trip, ASTC oracle)
need the owner's dump and run through texture_check; see backlog/tasks/task-11.md for their commands.
Under one minute. Run: python tests/texture/test_texture.py [--report out.json]
Env: SMRPG_TEST_DIR build directory (default A:\\AI\\Recomps\\smrpg-work\\build\\test_tex), SMRPG_CMAKE cmake path.
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "runtime" / "gfx" / "texture"
BUILD = Path(os.environ.get("SMRPG_TEST_DIR", r"A:\AI\Recomps\smrpg-work\build\test_tex"))
CMAKE = os.environ.get("SMRPG_CMAKE") or shutil.which("cmake") or r"A:\AI\Recomps\tools-ext\cmake-4.5.1-windows-x86_64\bin\cmake.exe"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    report = None
    if "--report" in sys.argv:
        report = Path(sys.argv[sys.argv.index("--report") + 1])
    BUILD.mkdir(parents=True, exist_ok=True)
    cfg = [CMAKE, "-S", str(SRC), "-B", str(BUILD), "-G", "MinGW Makefiles", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_BUILD_TYPE=Release"]
    r = run(cfg)
    if r.returncode:
        print(r.stdout[-2000:], r.stderr[-2000:])
        return 2
    r = run([CMAKE, "--build", str(BUILD), "--target", "texture_unit", "texture_check", "-j", "4"])
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        return 2
    exe = next(BUILD.glob("texture_unit*"), None)
    exe = BUILD / "texture_unit.exe" if (BUILD / "texture_unit.exe").exists() else exe
    r = run([str(exe)], timeout=600)
    print(r.stdout)
    passed = len(re.findall(r"^PASS ", r.stdout, re.M))
    failed = len(re.findall(r"^FAIL [a-z_0-9]+$", r.stdout, re.M))
    m = re.search(r"SUMMARY passed (\d+) failed (\d+)", r.stdout)
    ok = r.returncode == 0 and m is not None and failed == 0
    print("texture tests: %s (%d passed, %d failed)" % ("PASS" if ok else "FAIL", passed, failed))
    if report:
        report.write_text(json.dumps({"passed": passed, "failed": failed, "ok": ok, "exit": r.returncode}, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
