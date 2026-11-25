r"""Configure, build and install the game executable with CMake (task-13). Our own code.

  python -I tools/rtprep/build_game.py [--jobs N] [--clean]

Uses the lifted objects listed by `python -I tools/lifter/build.py link` (bin\link.rsp), builds runtime/ (core, hle/, NVN on Vulkan)
in A:\AI\Recomps\smrpg-work\build\rt-game at BelowNormal priority and copies smrpg.exe to build\bin\smrpg.exe.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
WORK = r"A:\AI\Recomps\smrpg-work"
BUILD = os.path.join(WORK, "build")
OUT = os.path.join(BUILD, "rt-game")
CMAKE = r"A:\AI\Recomps\tools-ext\cmake-4.5.1-windows-x86_64\bin\cmake.exe"
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000


def run(cmd):
    p = subprocess.run(cmd, creationflags=BELOW_NORMAL | NO_WINDOW, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    return p.returncode, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--clean", action="store_true")
    a = ap.parse_args()
    rsp = os.path.join(BUILD, "bin", "link.rsp")
    if not os.path.exists(rsp):
        print("missing", rsp, "(run tools/lifter/build.py link once)")
        return 2
    if a.clean and os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT, exist_ok=True)
    rc, out = run([CMAKE, "-S", os.path.join(ROOT, "runtime"), "-B", OUT, "-G", "MinGW Makefiles", "-DCMAKE_C_COMPILER=clang",
                   "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=Release", "-DSMRPG_GAME_OBJECTS=" + rsp.replace("\\", "/")])
    print(out[-1500:])
    if rc:
        return rc
    rc, out = run([CMAKE, "--build", OUT, "--", "-j", str(a.jobs)])
    print(out[-4000:])
    if rc:
        return rc
    exe = os.path.join(OUT, "smrpg.exe")
    dst = os.path.join(BUILD, "bin", "smrpg.exe")
    shutil.copyfile(exe, dst)
    print("installed", dst, os.path.getsize(dst))
    # task-22: the platform layer links SDL3 (MSYS2 ucrt64); SDL3.dll and the libiconv it needs sit beside the exe. Never committed.
    sdl_bin = os.path.join(os.environ.get("SMRPG_SDL3_DIR", r"A:\msys64\ucrt64"), "bin")
    for dll in ("SDL3.dll", "libiconv-2.dll"):
        src = os.path.join(sdl_bin, dll)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(BUILD, "bin", dll))
            print("installed", os.path.join(BUILD, "bin", dll))
        else:
            print("note: no", src, "(a build without the platform layer does not need it)")
    shutil.copyfile(os.path.join(ROOT, "config", "input.default.ini"), os.path.join(BUILD, "bin", "input.default.ini"))   # the default keyboard map
    shutil.copyfile(os.path.join(ROOT, "config", "options.default.ini"), os.path.join(BUILD, "bin", "options.default.ini"))   # task-16: documented defaults (options.ini is written by the overlay)
    return 0


if __name__ == "__main__":
    sys.exit(main())
