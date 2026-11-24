r"""Run the linked game library's boot at BelowNormal priority, hidden, with a hard time limit (task-5).

  python -I tools/lifter/run_boot.py [--seconds 120] [-- extra smrpg.exe args]
Log: A:\AI\Recomps\smrpg-work\build\logs\boot.log
"""
import argparse
import os
import subprocess
import sys

BUILD = r"A:\AI\Recomps\smrpg-work\build"
ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=int, default=120)
ap.add_argument("--exe", default=os.path.join(BUILD, "bin", "smrpg.exe"))
ap.add_argument("rest", nargs="*")
a = ap.parse_args()
log = os.path.join(BUILD, "logs", "boot.log")
cmd = [a.exe, "--max-seconds", str(a.seconds), "--log", log, "--img", os.path.join(BUILD, "img")] + a.rest
p = subprocess.Popen(cmd, creationflags=0x4000 | 0x08000000, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    out, _ = p.communicate(timeout=a.seconds + 60)
except subprocess.TimeoutExpired:
    subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
    out = "(killed by run_boot after timeout)"
print(out[-3000:])
print("exit code", p.returncode, "log", log)
sys.exit(0)
