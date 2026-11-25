r"""Upscale pass and present hook tests (task-23). Our own code.

  python -I tests/upscale/run_upscale_tests.py [--seconds 45] [--skip-game]

Needs a built game (python -I tools/rtprep/build_game.py). Every run is hidden and muted (tools/rtprep/run_game.py), one at a time, BelowNormal.
  1. fetch pin: tools/thirdparty/fetch_fsr.py reports the FSR files present and matching their sha256 (no download when they match).
  2. AC1 faithful default: upscale off, the presented image hashed at several frames; every hash line says ref-blit=equal (today's plain
     blit redone into a scratch image in the same command buffer, compared byte for byte), none says DIFFERENT.
  3. AC2 FSR 1: 1920x1080 game image to a 3840x2160 target with the validation layer on: zero validation errors or warnings, a PNG pair
     (game image in, pass output out) with the right sizes, and the scaled blit onto the 1280x720 hidden swapchain.
  4. AC3 cost: the pass logs its GPU time (timestamp queries) every 120 frames; the last line is reported.
Output: console plus tests/upscale/reports/UPSCALE_TEST_REPORT.md. Captures: A:\AI\Recomps\smrpg-work\captures\task23\test.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BUILD = r"A:\AI\Recomps\smrpg-work\build"
CAP = r"A:\AI\Recomps\smrpg-work\captures\task23\test"
LAYERS = r"A:\AI\Recomps\new-pokemon-snap\tools\vulkan_sdk\Bin"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + (" : " + detail if detail else ""))


def busy():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return [n for n in ("smrpg", "nvn_", "vkdiff") if n in out]


def run_game(tag, seconds, env):
    args = [sys.executable, "-I", os.path.join(ROOT, "tools", "rtprep", "run_game.py"), "--seconds", str(seconds), "--log", "t23test_" + tag,
            "--grep", "FATAL|EXIT"]
    for k, v in env.items():
        args += ["--env", "%s=%s" % (k, v)]
    subprocess.run(args, capture_output=True, text=True)
    p = os.path.join(BUILD, "logs", "t23test_" + tag + ".log.err")
    return open(p, errors="replace").read() if os.path.exists(p) else ""


def png_size(path):
    with open(path, "rb") as f:
        d = f.read(24)
    return struct.unpack(">II", d[16:24]) if d[:8] == b"\x89PNG\r\n\x1a\n" else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--skip-game", action="store_true")
    a = ap.parse_args()
    os.makedirs(CAP, exist_ok=True)
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "thirdparty", "fetch_fsr.py")], capture_output=True, text=True)
    check("fetch_fsr pin: files present and matching", r.returncode == 0 and "matching" in r.stdout, (r.stdout or r.stderr).strip()[:120])
    if not a.skip_game:
        if not os.path.exists(os.path.join(BUILD, "bin", "smrpg.exe")):
            print("no game build; run tools/rtprep/build_game.py first")
            return 2
        b = busy()
        if b:
            print("another heavy process is running:", b, "(one at a time machine-wide); try again later")
            return 3
        log = run_game("off", a.seconds, {"SMRPG_UPSCALE": "off", "SMRPG_PRESENT_HASH": "every:300"})
        eq = len(re.findall(r"present-hash: .*mode off .*ref-blit=equal", log)); df = len(re.findall(r"ref-blit=DIFFERENT", log))
        check("AC1 off: presented image equals today's blit at every hashed frame", eq >= 5 and df == 0, "%d equal, %d different" % (eq, df))
        env = {"SMRPG_UPSCALE": "fsr1", "SMRPG_UPSCALE_TARGET": "3840x2160", "NVN_VALIDATION": "1", "NVN_LAYER_PATH": LAYERS, "VK_LAYER_PATH": LAYERS,
               "SMRPG_UPSCALE_CAPTURE_DIR": CAP, "SMRPG_UPSCALE_CAPTURE_FRAMES": "900"}
        for f in os.listdir(CAP):
            if f.startswith("upscale_000900"):
                os.remove(os.path.join(CAP, f))
        log = run_game("fsr1", a.seconds, env)
        bad = re.findall(r"VALIDATION ERROR|validation warning", log)
        check("AC2 fsr1 4K: validation layer on, no errors or warnings", "validation=1" in log and not bad, "%d messages" % len(bad))
        check("AC2 fsr1 4K: pass ran at 1920x1080 -> 3840x2160", "game image 1920x1080" in log and "target 3840x2160" in log)
        pin, pout = os.path.join(CAP, "upscale_000900_in.png"), os.path.join(CAP, "upscale_000900_out.png")
        sizes = (png_size(pin) if os.path.exists(pin) else None, png_size(pout) if os.path.exists(pout) else None)
        check("AC2 fsr1 4K: PNG pair on A:", sizes == ((1920, 1080), (3840, 2160)), "%s %s" % (pin, sizes))
        t = re.findall(r"gpu time over \d+ frames, (\S+) -> (\S+): easu ([\d.]+) ms, rcas ([\d.]+) ms, copy/blit ([\d.]+) ms, total ([\d.]+) ms \(worst ([\d.]+) ms\)", log)
        check("AC3 fsr1 4K: GPU time logged", bool(t), "easu %s, rcas %s, copy/blit %s, total %s ms (worst %s)" % t[-1][2:] if t else "none")
        log = run_game("fsr2", 12, {"SMRPG_UPSCALE": "fsr2"})
        check("fsr2 requested: falls back to the plain blit with a reason", "fsr2 needs per-pixel motion vectors" in log)
    ok = all(r[1] for r in results)
    rep = ["# Upscale test report (task-23)", "", "Run %s. Result: %s." % (time.strftime("%Y-%m-%d %H:%M"), "PASS" if ok else "FAIL"), "", "| Check | Result | Detail |", "|---|---|---|"]
    for n, k, d in results:
        rep.append("| %s | %s | %s |" % (n, "PASS" if k else "FAIL", d.replace("|", "/")))
    os.makedirs(os.path.join(ROOT, "tests", "upscale", "reports"), exist_ok=True)
    with open(os.path.join(ROOT, "tests", "upscale", "reports", "UPSCALE_TEST_REPORT.md"), "w") as f:
        f.write("\n".join(rep) + "\n")
    print("%d checks, %d failed" % (len(results), sum(1 for r in results if not r[1])))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
