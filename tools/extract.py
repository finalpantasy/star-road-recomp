#!/usr/bin/env python3
"""Reproducible extraction of the owner's Super Mario RPG dump.

Turns an XCI (base game) plus an NSP (v1.0.1 update) into the canonical layout:

    <out>/base/exefs/        base program exefs
    <out>/v101/exefs/        patched exefs
    <out>/v101/romfs/        patched romfs (base + update merged)
    <out>/manifest.json      sha256 of every exefs file, romfs count and bytes

and checks the result against tools/inputs.json.

Usage (run with -I):
    python -I tools/extract.py --dumps <dir> --keys <prod.keys> --hactool <hactool.exe> --out <dir>

Nothing from the game is written inside the repository. Key and titlekey
values are never printed; hactool output is captured and redacted.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

BELOW_NORMAL = 0x00004000  # Windows BELOW_NORMAL_PRIORITY_CLASS
HEX_RUN = re.compile(r"[0-9a-fA-F]{32,}")
TIK_TITLEKEY_OFFSET = 0x180
TIK_TITLEKEY_LEN = 16


def redact(text):
    return HEX_RUN.sub("<redacted>", text)


def log(msg):
    print(redact(msg), flush=True)


def filter_keys(src, dst):
    """Copy src to dst keeping only well formed lines. Returns (kept, skipped)."""
    kept = skipped = 0
    with open(src, "r", encoding="utf-8", errors="replace") as fi, \
            open(dst, "w", encoding="utf-8", newline="\n") as fo:
        for line in fi:
            raw = line.strip()
            if not raw or raw.startswith(";") or raw.startswith("#"):
                continue
            if "=" not in raw:
                skipped += 1
                continue
            name, _, val = raw.partition("=")
            val = val.strip()
            ok = (val != "" and re.fullmatch(r"[0-9a-fA-F]+", val) is not None
                  and len(val) % 32 == 0)
            if ok:
                fo.write("%s = %s\n" % (name.strip(), val))
                kept += 1
            else:
                skipped += 1
    return kept, skipped


def run_hactool(hactool, args, what):
    """Run hactool at BelowNormal priority, capture output, never echo secrets."""
    t0 = time.time()
    flags = BELOW_NORMAL if os.name == "nt" else 0
    p = subprocess.Popen([hactool] + args, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, creationflags=flags)
    out = p.communicate()[0].decode("utf-8", "replace")
    dt = time.time() - t0
    log("hactool %s: exit %d in %.1fs" % (what, p.returncode, dt))
    if p.returncode != 0:
        tail = "\n".join(out.splitlines()[-15:])
        log(tail)
        raise SystemExit("hactool failed: " + what)
    return out


def nca_info(hactool, keys, nca):
    out = run_hactool(hactool, ["-k", keys, "--suppresskeys", "--disablekeywarns",
                                "-i", nca], "info " + os.path.basename(nca))
    m = re.search(r"Content Type:\s+(\w+)", out)
    return m.group(1) if m else None


def find_program_nca(hactool, keys, folder):
    cands = [f for f in sorted(os.listdir(folder))
             if f.endswith(".nca") and not f.endswith(".cnmt.nca")]
    # Program NCAs are by far the largest; check biggest first.
    cands.sort(key=lambda f: -os.path.getsize(os.path.join(folder, f)))
    for f in cands:
        p = os.path.join(folder, f)
        if nca_info(hactool, keys, p) == "Program":
            return p
    raise SystemExit("no Program NCA in " + folder)


def read_titlekey(tik_path):
    with open(tik_path, "rb") as f:
        f.seek(TIK_TITLEKEY_OFFSET)
        k = f.read(TIK_TITLEKEY_LEN)
    if len(k) != TIK_TITLEKEY_LEN:
        raise SystemExit("ticket too short: " + tik_path)
    return k.hex()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def tree_stats(root):
    count = total = 0
    for d, _, files in os.walk(root):
        for n in files:
            count += 1
            total += os.path.getsize(os.path.join(d, n))
    return count, total


def clear(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dumps", required=True, help="folder holding the .xci and .nsp")
    ap.add_argument("--keys", required=True, help="path to prod.keys (never modified)")
    ap.add_argument("--hactool", required=True, help="path to hactool executable")
    ap.add_argument("--out", required=True, help="output root (canonical layout)")
    ap.add_argument("--inputs", default=os.path.join(here, "inputs.json"))
    ap.add_argument("--verify-dumps", action="store_true",
                    help="also sha256 the XCI and NSP (slow, about 7 GB)")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep the intermediate NCA folders")
    a = ap.parse_args()

    with open(a.inputs, "r", encoding="utf-8") as f:
        spec = json.load(f)
    xci = os.path.join(a.dumps, spec["dumps"]["base_xci"]["path"])
    nsp = os.path.join(a.dumps, spec["dumps"]["update_nsp"]["path"])
    for p in (xci, nsp, a.keys, a.hactool):
        if not os.path.isfile(p):
            raise SystemExit("missing file: " + p)

    out = os.path.abspath(a.out)
    work = os.path.join(out, "_work")
    os.makedirs(out, exist_ok=True)
    t_all = time.time()
    problems = []

    if a.verify_dumps:
        for k, p in (("base_xci", xci), ("update_nsp", nsp)):
            got = sha256_file(p)
            want = spec["dumps"][k]["sha256"]
            log("dump %s sha256 %s" % (k, "OK" if got == want else "MISMATCH"))
            if got != want:
                problems.append("dump " + k)

    keydir = tempfile.mkdtemp(prefix="smrpg-keys-")
    try:
        keys = os.path.join(keydir, "prod.keys")
        kept, skipped = filter_keys(a.keys, keys)
        log("keys: kept %d lines, skipped %d malformed lines" % (kept, skipped))

        # 1. XCI secure partition -> base NCAs
        xci_dir = os.path.join(work, "xci_secure")
        clear(xci_dir)
        run_hactool(a.hactool, ["-k", keys, "--suppresskeys", "--disablekeywarns",
                                "-t", "xci", "--securedir=" + xci_dir, xci], "xci secure")
        base_nca = find_program_nca(a.hactool, keys, xci_dir)

        # 2. NSP -> update NCAs and ticket
        nsp_dir = os.path.join(work, "nsp")
        clear(nsp_dir)
        run_hactool(a.hactool, ["-k", keys, "--suppresskeys", "--disablekeywarns",
                                "-t", "pfs0", "--pfs0dir=" + nsp_dir, nsp], "nsp")
        upd_nca = find_program_nca(a.hactool, keys, nsp_dir)
        tiks = [f for f in os.listdir(nsp_dir) if f.endswith(".tik")]
        if len(tiks) != 1:
            raise SystemExit("expected one .tik in the NSP, found %d" % len(tiks))
        titlekey = read_titlekey(os.path.join(nsp_dir, tiks[0]))

        # 3. base exefs
        base_exefs = os.path.join(out, "base", "exefs")
        clear(base_exefs)
        run_hactool(a.hactool, ["-k", keys, "--suppresskeys", "--disablekeywarns",
                                "--exefsdir=" + base_exefs, base_nca], "base exefs")

        # 4. patched exefs and romfs
        v_exefs = os.path.join(out, "v101", "exefs")
        v_romfs = os.path.join(out, "v101", "romfs")
        clear(v_exefs)
        clear(v_romfs)
        run_hactool(a.hactool, ["-k", keys, "--suppresskeys", "--disablekeywarns",
                                "--titlekey=" + titlekey, "--basenca=" + base_nca,
                                "--exefsdir=" + v_exefs, "--romfsdir=" + v_romfs,
                                upd_nca], "v101 exefs+romfs")
    finally:
        shutil.rmtree(keydir, ignore_errors=True)

    # 5. manifest and comparison
    manifest = {"extracted": {}, "romfs": {}}
    for rel, want in spec["extracted"].items():
        p = os.path.join(out, *rel.split("/"))
        if not os.path.isfile(p):
            log("FAIL %s missing" % rel)
            problems.append(rel)
            continue
        got = sha256_file(p)
        manifest["extracted"][rel] = got
        ok = got == want
        log("%s %s" % ("PASS" if ok else "FAIL", rel))
        if not ok:
            problems.append(rel)
    # files beyond the pinned list
    for sub in ("base/exefs", "v101/exefs"):
        for n in sorted(os.listdir(os.path.join(out, *sub.split("/")))):
            rel = sub + "/" + n
            if rel not in spec["extracted"]:
                manifest["extracted"][rel] = sha256_file(os.path.join(out, *rel.split("/")))
                log("NOTE unpinned %s" % rel)

    cnt, tot = tree_stats(v_romfs)
    manifest["romfs"] = {"dir": "v101/romfs", "file_count": cnt, "total_bytes": tot}
    want = spec["romfs"]
    rok = cnt == want["file_count"] and tot == want["total_bytes"]
    log("%s romfs: %d files, %d bytes (want %d, %d)" %
        ("PASS" if rok else "FAIL", cnt, tot, want["file_count"], want["total_bytes"]))
    if not rok:
        problems.append("romfs")

    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")

    if not a.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    log("total %.1fs, %s" % (time.time() - t_all,
                             "ALL MATCH" if not problems else "MISMATCH: " + ", ".join(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
