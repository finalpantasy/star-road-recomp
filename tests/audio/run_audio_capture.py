r"""Hidden audio capture of the opening (task-27 AC2). Our own code.

usage: python -I tests/audio/run_audio_capture.py [--seconds 150] [--name t27_open] [--audio-device] [--input FILE]

Runs the game through tools/rtprep/run_game.py (which takes the machine-wide heavy slot first), hidden, BelowNormal, on the SDL dummy audio
driver, with the scripted title input of task-26 and a disposable save on A:. SMRPG_AUDIO_DUMP records everything the guest appends to audout
before it reaches any device, so nothing is ever audible. With --audio-device the run also passes --audio, which opens the dummy device through
the real SDL path and paces buffer release at the device's rate (the dummy driver plays in real time but makes no sound).
Then runs tests/audio/analyze_pcm.py on the capture. Captures and logs stay on A:\AI\Recomps\smrpg-work\captures\<name>\.
Exit 0 when the capture shows audible signal, 1 when not, 2 when the run could not start.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
WORK = r"A:\AI\Recomps\smrpg-work"
DEFAULT_INPUT = os.path.join(WORK, "captures", "codex_t26_02", "input.txt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=150)
    ap.add_argument("--name", default="t27_open")
    ap.add_argument("--audio-device", action="store_true")
    ap.add_argument("--input", default=DEFAULT_INPUT)
    ap.add_argument("--min-active-seconds", type=int, default=10)
    a = ap.parse_args()
    exe = os.path.join(WORK, "build", "bin", "smrpg.exe")
    if not os.path.exists(exe):
        print("NOT_RUN: no", exe, "(python -I tools/rtprep/build_game.py)")
        return 2
    cap = os.path.join(WORK, "captures", a.name)
    save = os.path.join(WORK, "save", a.name)
    for d in (cap, save):
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
    wav = os.path.join(cap, "audout.wav")
    cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "rtprep", "run_game.py"), "--seconds", str(a.seconds), "--log", a.name,
           "--grep", r"hwopus|audout|audren|EXIT|FATAL|UNKNOWN",
           "--env", "SDL_AUDIO_DRIVER=dummy", "--env", "SDL_AUDIODRIVER=dummy", "--env", "SMRPG_AUDIO_DUMP=" + wav,
           "--env", "SMRPG_SAVE=" + save, "--env", "SMRPG_INPUT_FILE=" + a.input]
    if a.audio_device:
        cmd += ["--", "--audio"]
    env = dict(os.environ, SMRPG_HEAVY_HOLDER=os.environ.get("SMRPG_HEAVY_HOLDER", "claude-smrpg-audio"))
    print("running:", " ".join(cmd[2:]))
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    out = r.stdout.splitlines()
    print("\n".join(out[:40]))
    if len(out) > 40:
        print("... (%d more lines)" % (len(out) - 40))
    log = os.path.join(WORK, "build", "logs", a.name + ".log")
    if not os.path.exists(wav):
        print("NOT_RUN: no capture written; see", log)
        return 2
    an = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tests", "audio", "analyze_pcm.py"), wav, "--log", log, "--json",
                         os.path.join(cap, "analysis.json"), "--min-active-seconds", str(a.min_active_seconds)], capture_output=True, text=True)
    print(an.stdout)
    return an.returncode


if __name__ == "__main__":
    sys.exit(main())
