r"""Fetch the pinned AMD FidelityFX FSR 1 sources (task-23). Our own code.

  python -I tools/thirdparty/fetch_fsr.py [--dest A:\AI\Recomps\thirdparty\fsr] [--force]

Downloads GPUOpen-Effects/FidelityFX-FSR v1.0.2 (MIT license) at its pinned commit, extracts only the three files the build uses
(ffx_a.h, ffx_fsr1.h, license.txt) and checks each against the sha256 pinned below. A mismatch deletes nothing and returns 1.
Nothing from FSR is committed: the files live on A: and runtime/gfx/upscale compiles its compute shaders against them at build time.
The credit line goes into the notices file at release (docs/REUSE_LEDGER.md lists the library).
"""
import argparse
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

REPO = "GPUOpen-Effects/FidelityFX-FSR"
TAG = "v1.0.2"
COMMIT = "a21ffb8f6c13233ba336352bdff293894c706575"
URL = "https://codeload.github.com/%s/zip/%s" % (REPO, COMMIT)
FILES = {   # path inside the archive -> (file name on disk, sha256)
    "ffx-fsr/ffx_a.h": ("ffx_a.h", "f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb"),
    "ffx-fsr/ffx_fsr1.h": ("ffx_fsr1.h", "93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5"),
    "license.txt": ("LICENSE.txt", "db089274ce766da70f5b7d791029c3486f9f9e27c8c79c652689603d3192e802"),
}
DEFAULT_DEST = r"A:\AI\Recomps\thirdparty\fsr"


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def have_all(dest):
    for _, (name, h) in FILES.items():
        p = os.path.join(dest, name)
        if not os.path.isfile(p) or sha256(open(p, "rb").read()) != h:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default=os.environ.get("SMRPG_FSR_DIR", DEFAULT_DEST))
    ap.add_argument("--force", action="store_true", help="download again even when the files are present and match")
    a = ap.parse_args()
    if have_all(a.dest) and not a.force:
        print("FSR %s already present and matching in %s" % (TAG, a.dest))
        return 0
    print("downloading", URL)
    with urllib.request.urlopen(URL, timeout=120) as r:
        data = r.read()
    print("archive: %d bytes, sha256 %s" % (len(data), sha256(data)))
    z = zipfile.ZipFile(io.BytesIO(data))
    root = "FidelityFX-FSR-%s/" % COMMIT
    out = {}
    for member, (name, want) in FILES.items():
        try:
            blob = z.read(root + member)
        except KeyError:
            print("missing in archive:", member)
            return 1
        got = sha256(blob)
        if got != want:
            print("sha256 mismatch for %s: got %s, pinned %s" % (member, got, want))
            return 1
        out[name] = blob
    os.makedirs(a.dest, exist_ok=True)
    for name, blob in out.items():
        with open(os.path.join(a.dest, name), "wb") as f:
            f.write(blob)
        print("wrote", os.path.join(a.dest, name), len(blob))
    with open(os.path.join(a.dest, "VERSION.txt"), "w") as f:
        f.write("%s %s\ncommit %s\nMIT license (LICENSE.txt)\n" % (REPO, TAG, COMMIT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
