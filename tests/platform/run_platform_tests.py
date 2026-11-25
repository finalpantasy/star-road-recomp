r"""Star Road Recomp platform layer tests (task-22). Our own code.

usage: python -I tests/platform/run_platform_tests.py [--game] [--game-seconds 30]
  default   build and run tests/platform/test_platform.c (hid layout, input mapping, key map, script, audio) against SDL3
  --game    also run the game hidden and muted for --game-seconds, poll Windows for every top-level window the process owns, and check the
            log: the window must exist, never be visible, and no audio device may be opened (needs a built smrpg.exe, build_game.py)
Exit 0 all pass, 1 a test failed, 3 not run (no toolchain or no SDL3).
Heavy-process rules: BelowNormal priority, hidden, SDL_AUDIO_DRIVER=dummy, killed by PID only, never --window or --audio.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SDL = os.environ.get("SMRPG_SDL3_DIR", r"A:\msys64\ucrt64")
BUILD = r"A:\AI\Recomps\smrpg-work\build"
OUT = os.path.join(BUILD, "platform_tests")
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000
REPORT = os.path.join(ROOT, "tests", "platform", "reports", "PLATFORM_TEST_REPORT.md")


def muted_env():
    env = dict(os.environ)
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def build_unit():
    clang = shutil.which("clang")
    if not clang or not os.path.exists(os.path.join(SDL, "include", "SDL3", "SDL.h")):
        print("NOT_RUN: clang or SDL3 headers missing (SMRPG_SDL3_DIR=%s)" % SDL)
        return None
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "test_platform.exe")
    src = os.path.join(ROOT, "tests", "platform", "test_platform.c")
    plat = os.path.join(ROOT, "runtime", "platform")
    cmd = [clang, "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-std=gnu11", "-idirafter", os.path.join(SDL, "include"), src,
           os.path.join(plat, "pad.c"), os.path.join(plat, "plat_audio.c"), os.path.join(plat, "hidshm.c"), os.path.join(SDL, "lib", "libSDL3.dll.a"),      # the import library by path: -L would also pull MSYS2's own CRT objects
           
           "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW)
    if p.returncode:
        print("BUILD FAILED\n" + (p.stdout + p.stderr)[-3000:])
        return False
    for dll in ("SDL3.dll", "libiconv-2.dll"):
        shutil.copyfile(os.path.join(SDL, "bin", dll), os.path.join(OUT, dll))
    return exe


def build_options_test():
    """task-16: the options store (runtime/platform/overlay/options.c) is plain C with no SDL: its own small test."""
    clang = shutil.which("clang")
    if not clang:
        return None
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "test_options.exe")
    ovl = os.path.join(ROOT, "runtime", "platform", "overlay")
    cmd = [clang, "-O1", "-g", "-Wall", "-Wextra", "-std=gnu11", os.path.join(ROOT, "tests", "platform", "test_options.c"), os.path.join(ovl, "options.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW)
    if p.returncode:
        print("BUILD FAILED (test_options)" + chr(10) + (p.stdout + p.stderr)[-3000:])
        return False
    return exe


def run_unit(exe):
    p = subprocess.Popen([exe, OUT, ROOT], cwd=OUT, env=muted_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         creationflags=BELOW_NORMAL | NO_WINDOW)
    try:
        out, _ = p.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
        out, _ = p.communicate()
        out += "\n(killed after 60 s)"
        p.returncode = 124
    return p.returncode, out


user32 = ctypes.WinDLL("user32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows_of(pid):
    """Every top-level window owned by pid: (hwnd, visible, title)."""
    found = []

    def cb(h, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(h, buf, 256)
            found.append((h, bool(user32.IsWindowVisible(h)), buf.value))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def run_game(seconds):
    exe = os.path.join(BUILD, "bin", "smrpg.exe")
    if not os.path.exists(exe):
        print("NOT_RUN: no", exe, "(python -I tools/rtprep/build_game.py)")
        return None
    log = os.path.join(BUILD, "logs", "platform_hidden.log")
    env = muted_env()
    env["SMRPG_NVDEC_OK"] = "1"
    p = subprocess.Popen([exe, "--max-seconds", str(seconds), "--log", log, "--img", os.path.join(BUILD, "img")], env=env,
                         stdout=subprocess.DEVNULL, stderr=open(log + ".err", "wb"), creationflags=BELOW_NORMAL | NO_WINDOW)
    seen, ever_visible, polls = {}, False, 0
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < seconds + 30:
        for h, vis, title in windows_of(p.pid):
            seen[h] = title
            ever_visible = ever_visible or vis
        polls += 1
        time.sleep(0.25)
    if p.poll() is None:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
    text = open(log, errors="replace").read()
    return {"polls": polls, "windows": seen, "ever_visible": ever_visible, "log": text, "path": log, "pid": p.pid}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", action="store_true")
    ap.add_argument("--game-seconds", type=int, default=30)
    a = ap.parse_args()
    lines = ["# Platform test report (task-22)", "", "Generated by tests/platform/run_platform_tests.py.", ""]
    code = 0
    exe = build_unit()
    if exe is None:
        return 3
    if exe is False:
        return 1
    rc, out = run_unit(exe)
    print(out)
    lines += ["## Unit test (test_platform.c)", "", "```", out.strip()[-3500:], "```", "", "Exit code %d." % rc, ""]
    if rc:
        code = 1
    oexe = build_options_test()
    if oexe is False:
        code = 1
    elif oexe:
        rc2, out2 = run_unit(oexe)
        print(out2)
        lines += ["## Options store (test_options.c, task-16)", "", "```", out2.strip()[-2000:], "```", "", "Exit code %d." % rc2, ""]
        if rc2:
            code = 1
    if a.game:
        r = run_game(a.game_seconds)
        if r is None:
            return 3
        txt = r["log"]
        checks = []
        checks.append(("the SDL window exists (Windows reports it for the process)", len(r["windows"]) > 0))
        checks.append(("no window of the process was ever visible (%d polls of EnumWindows)" % r["polls"], not r["ever_visible"]))
        checks.append(("log: window created hidden (IsWindowVisible=0, sdl_hidden=1)", bool(re.search(r"window state \(created\).*IsWindowVisible=0.*sdl_hidden=1", txt))))
        checks.append(("log: audio off, null sink", "platform: audio off: null sink, no device" in txt))
        checks.append(("log: no audio device opened", "audio device open" not in txt))
        checks.append(("log: Vulkan surface came from the hidden SDL window", "Vulkan surface created from the SDL window (hidden)" in txt))
        checks.append(("log: pad samples are being published", "pad samples published" in txt))
        lines += ["## Hidden run (%d s, muted, pid %d)" % (a.game_seconds, r["pid"]), "", "Windows seen: %s" % (r["windows"] or "none"), ""]
        for name, ok in checks:
            print(("PASS " if ok else "FAIL ") + name)
            lines.append("- %s %s" % ("PASS" if ok else "FAIL", name))
            if not ok:
                code = 1
        lines += ["", "Log: %s" % r["path"], ""]
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("report", REPORT, "exit", code)
    return code


if __name__ == "__main__":
    sys.exit(main())
