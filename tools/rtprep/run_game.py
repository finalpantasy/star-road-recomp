r"""Run the game executable hidden at BelowNormal with a hard time limit and print the interesting log lines (task-13). Our own code.

  python -I tools/rtprep/run_game.py [--seconds 60] [--log NAME] [--grep REGEX] [-- extra smrpg.exe args]
Log: A:\AI\Recomps\smrpg-work\build\logs\<NAME>.log (default hle_boot)
Takes the machine-wide heavy slot first (tools/rtprep/heavy_lock.py); SMRPG_HEAVY_LOCK=0 skips it for nested calls.
"""
import argparse
import os
import re
import subprocess
import sys

BUILD = r"A:\AI\Recomps\smrpg-work\build"


def run(a):
    log = os.path.join(BUILD, "logs", a.log + ".log")
    exe = os.path.join(BUILD, "bin", "smrpg.exe")
    env = dict(os.environ)
    if "--audio" not in a.rest:     # task-22: every background run is muted (owner rule): SDL audio driver dummy, and the runtime's audout stays a null sink
        env.setdefault("SDL_AUDIO_DRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    cmd = [exe, "--max-seconds", str(a.seconds), "--log", log, "--img", os.path.join(BUILD, "img")] + a.rest
    errf = open(log + ".err", "wb")          # the NVN layer and other host libraries print to stderr
    p = subprocess.Popen(cmd, creationflags=0x4000 | 0x08000000, stdout=subprocess.DEVNULL, stderr=errf, env=env)
    try:
        p.wait(timeout=a.seconds + 30)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
        print("(killed after timeout)")
    rx = re.compile(a.grep)
    n = 0
    for line in open(log, errors="replace"):
        if rx.search(line):
            print(line.rstrip()[:300])
            n += 1
            if n > 150:
                print("... (more in log)")
                break
    print("exit code", p.returncode, "log", log)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--log", default="hle_boot")
    ap.add_argument("--grep", default=r"IPC|sm:|EXIT|FATAL|svcBreak|UNIMPL|BOOT|UNKNOWN|C\+\+ throw|guest thread|crash|Exception")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("rest", nargs="*")
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # python -I does not add the script directory
    from heavy_lock import heavy_slot, HeavyLockTimeout, EX_TEMPFAIL
    try:
        with heavy_slot("run_game.py"):                              # one heavy process at a time across both leads (task-27)
            return run(a)
    except HeavyLockTimeout as e:
        print("[heavy-lock]", e)
        return EX_TEMPFAIL


if __name__ == "__main__":
    sys.exit(main())
