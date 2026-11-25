r"""Measure an audout PCM capture (SMRPG_AUDIO_DUMP wav) and the runtime log that produced it (task-27). Our own code.

usage: python -I tests/audio/analyze_pcm.py CAPTURE.wav [--log RUN.log] [--json OUT.json] [--min-active-seconds N] [--min-peak N]

Prints and checks:
  format        channels, rate, duration, bytes
  levels        per second: peak, rms, share of non-zero frames; the whole capture's peak and rms
  activity      seconds with audible signal (rms above a floor), longest silent run, first audible second
  channels      per channel peak and rms (the Switch layout is 6 channels: the game mixes music to the front pair)
  pitch         strongest FFT bin of the loudest second (a sanity look for a tonal signal, not a pass criterion)
  timing        with --log: appended audio seconds against the run's wall clock between the first and last "audout" measurement lines
  hwopus        with --log: decoder opens, errors, silent fallbacks
Exit 0 when the capture passes (enough active seconds and peak), 1 when it fails, 2 on bad input. Captures stay on A:, never in Git.
"""
import argparse
import array
import json
import math
import re
import struct
import sys

FLOOR_RMS = 8.0          # about -72 dBFS: below this a second counts as silent


def read_wav(path):
    with open(path, "rb") as f:
        h = f.read(44)
        if len(h) < 44 or h[:4] != b"RIFF" or h[8:16] != b"WAVEfmt " or h[36:40] != b"data":
            raise ValueError("not a 16 bit PCM wav written by pcm_dump.h")
        ch, rate = struct.unpack_from("<HI", h, 22)
        bits = struct.unpack_from("<H", h, 34)[0]
        if bits != 16:
            raise ValueError("only 16 bit PCM")
        raw = f.read()                          # the header size field may lag behind a run that was killed: trust the file length
    n = len(raw) // (2 * ch) * (2 * ch)
    a = array.array("h")
    a.frombytes(raw[:n])
    if sys.byteorder != "little":
        a.byteswap()
    return ch, rate, a


def fft_peak(samples, rate):
    """Strongest bin of a 4096 point Hann-windowed DFT of the most energetic window (pure python radix-2)."""
    n = 4096
    if len(samples) < n:
        return None
    best, start = -1.0, 0
    for s in range(0, len(samples) - n + 1, n):
        e = sum(v * v for v in samples[s:s + n])
        if e > best:
            best, start = e, s
    x = [complex(samples[start + i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)), 0) for i in range(n)]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    size = 2
    while size <= n:
        w = complex(math.cos(2 * math.pi / size), -math.sin(2 * math.pi / size))
        for s in range(0, n, size):
            wk = 1 + 0j
            for k in range(size // 2):
                u, v = x[s + k], x[s + k + size // 2] * wk
                x[s + k], x[s + k + size // 2] = u + v, u - v
                wk *= w
        size <<= 1
    mags = [abs(c) for c in x[: n // 2]]
    k = max(range(1, n // 2), key=lambda i: mags[i])
    return k * rate / n


def analyze(path, log=None):
    ch, rate, a = read_wav(path)
    frames = len(a) // ch
    res = {"file": path, "channels": ch, "rate": rate, "frames": frames, "seconds": frames / rate}
    per_sec = []
    for s in range(0, frames // rate + (1 if frames % rate else 0)):
        seg = a[s * rate * ch:(s + 1) * rate * ch]
        if not seg:
            break
        peak = max(max(seg), -min(seg))
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        nz = sum(1 for i in range(0, len(seg), ch) if any(seg[i:i + ch])) / max(1, len(seg) // ch)
        per_sec.append({"s": s, "peak": peak, "rms": round(rms, 1), "nonzero": round(nz, 3)})
    res["per_second"] = per_sec
    active = [p for p in per_sec if p["rms"] > FLOOR_RMS]
    res["active_seconds"] = len(active)
    res["first_active_second"] = active[0]["s"] if active else None
    longest = run = 0
    for p in per_sec:
        run = run + 1 if p["rms"] <= FLOOR_RMS else 0
        longest = max(longest, run)
    res["longest_silent_run_seconds"] = longest
    res["peak"] = max((p["peak"] for p in per_sec), default=0)
    tot = sum(v * v for v in a)
    res["rms"] = round(math.sqrt(tot / len(a)), 1) if len(a) else 0.0
    res["clipped_samples"] = sum(1 for v in a if v >= 32767 or v <= -32768)
    chans = []
    for c in range(ch):
        col = a[c::ch]
        chans.append({"ch": c, "peak": max(max(col), -min(col)) if len(col) else 0,
                      "rms": round(math.sqrt(sum(v * v for v in col) / len(col)), 1) if len(col) else 0.0})
    res["per_channel"] = chans
    if active:
        loud = max(active, key=lambda p: p["rms"])["s"]
        seg = a[loud * rate * ch:(loud + 1) * rate * ch]
        mix = [sum(seg[i:i + 2]) / 2 for i in range(0, len(seg) - 1, ch)]          # front pair
        res["loudest_second"] = loud
        res["loudest_second_peak_hz"] = fft_peak(mix, rate)
    if log:
        res["log"] = log_facts(log, rate)
    return res


def log_facts(path, rate):
    rx = re.compile(r"^\[\s*([0-9.]+) T\d+\] \[hle\] audout: (\d+) frames \(([0-9.]+) s\) appended, last 5 s: peak=(\d+) rms=([0-9.]+)")
    pts = []
    opens = errs = silent = 0
    opens_line = []
    with open(path, errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m:
                pts.append((float(m.group(1)), int(m.group(2))))
            if "[hle] hwopus: open" in line:
                opens += 1
                if "SILENT" in line:
                    silent += 1
                if len(opens_line) < 3:
                    opens_line.append(line.strip()[:160])
            if "[hle] hwopus: decode error" in line:
                errs += 1
    out = {"measurements": len(pts), "hwopus_opens": opens, "hwopus_silent_opens": silent, "hwopus_error_lines": errs, "hwopus_open_samples": opens_line}
    if len(pts) >= 2:
        dt = pts[-1][0] - pts[0][0]
        df = pts[-1][1] - pts[0][1]
        out["wall_seconds"] = round(dt, 2)
        out["audio_seconds"] = round(df / rate, 2)
        out["audio_per_wall"] = round(df / rate / dt, 3) if dt > 0 else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--log")
    ap.add_argument("--json")
    ap.add_argument("--min-active-seconds", type=int, default=10)
    ap.add_argument("--min-peak", type=int, default=2000)
    a = ap.parse_args()
    try:
        r = analyze(a.wav, a.log)
    except (OSError, ValueError) as e:
        print("bad input:", e)
        return 2
    print("capture %s: %d ch, %d Hz, %.1f s (%d frames)" % (a.wav, r["channels"], r["rate"], r["seconds"], r["frames"]))
    print("peak %d, rms %.1f, clipped samples %d" % (r["peak"], r["rms"], r["clipped_samples"]))
    print("active seconds (rms > %.0f): %d of %d, first at %s, longest silent run %d s" % (FLOOR_RMS, r["active_seconds"], len(r["per_second"]),
                                                                                      r["first_active_second"], r["longest_silent_run_seconds"]))
    print("channels:", ", ".join("%d: peak %d rms %.0f" % (c["ch"], c["peak"], c["rms"]) for c in r["per_channel"]))
    if "loudest_second" in r:
        print("loudest second %d: strongest bin %.0f Hz" % (r["loudest_second"], r["loudest_second_peak_hz"] or 0))
    if "log" in r:
        print("log:", json.dumps(r["log"]))
    ok = r["active_seconds"] >= a.min_active_seconds and r["peak"] >= a.min_peak
    r["pass"] = ok
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f, indent=1)
    print("PASS" if ok else "FAIL: not enough audible signal (min %d active seconds, peak %d)" % (a.min_active_seconds, a.min_peak))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
