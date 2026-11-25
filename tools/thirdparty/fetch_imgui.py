r"""Fetch the pinned Dear ImGui release for the options overlay (task-16, D-012). Our own script.

  python -I tools/thirdparty/fetch_imgui.py [--dir DIR] [--force]

Dear ImGui is MIT licensed (https://github.com/ocornut/imgui). The script downloads the pinned release archive, checks its sha256,
unpacks it to <dir>\src and prints the paths the build uses. Nothing from ImGui is committed: it lives on A: only
(default A:\AI\Recomps\thirdparty\imgui, override with SMRPG_IMGUI_DIR). The build (runtime/CMakeLists.txt) compiles imgui*.cpp plus the
Vulkan backend from <dir>\src\imgui-<version>; without the unpacked source the runtime builds without the overlay.
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = "1.93.0"
URL = "https://github.com/ocornut/imgui/archive/refs/tags/v%s.zip" % VERSION
SHA256 = "e21907fd8dd5ea59aa00538632a02ad0a3ededb4ddc533b94702331fbcd6c470"
DEFAULT_DIR = r"A:\AI\Recomps\thirdparty\imgui"
# files the overlay build needs (relative to the unpacked release root); checked after unpacking
REQUIRED = ["imgui.h", "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "imgui_internal.h",
            "backends/imgui_impl_vulkan.cpp", "backends/imgui_impl_vulkan.h", "backends/imgui_impl_sdl3.cpp", "backends/imgui_impl_sdl3.h", "LICENSE.txt"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def src_root(base):
    return os.path.join(base, "src", "imgui-" + VERSION)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.environ.get("SMRPG_IMGUI_DIR", DEFAULT_DIR))
    ap.add_argument("--force", action="store_true", help="download and unpack again")
    a = ap.parse_args()
    base = a.dir
    root = src_root(base)
    if not a.force and all(os.path.exists(os.path.join(root, r)) for r in REQUIRED):
        print("imgui %s already unpacked: %s" % (VERSION, root))
        print("IMGUI_ROOT=" + root)
        return 0
    os.makedirs(base, exist_ok=True)
    zpath = os.path.join(base, "imgui-v%s.zip" % VERSION)
    if a.force or not os.path.exists(zpath) or sha256(zpath) != SHA256:
        print("downloading", URL)
        with urllib.request.urlopen(URL, timeout=120) as r, open(zpath, "wb") as f:
            shutil.copyfileobj(r, f)
    got = sha256(zpath)
    if got != SHA256:
        print("sha256 mismatch for %s\n  expected %s\n  got      %s" % (zpath, SHA256, got))
        return 1
    print("sha256 ok", got)
    if os.path.isdir(os.path.join(base, "src")):
        shutil.rmtree(os.path.join(base, "src"))
    with zipfile.ZipFile(zpath) as z:
        for n in z.namelist():                                    # no path traversal out of the target directory
            dest = os.path.abspath(os.path.join(base, "src", n))
            if not dest.startswith(os.path.abspath(os.path.join(base, "src")) + os.sep):
                print("unsafe path in archive:", n)
                return 1
        z.extractall(os.path.join(base, "src"))
    missing = [r for r in REQUIRED if not os.path.exists(os.path.join(root, r))]
    if missing:
        print("missing after unpack:", missing)
        return 1
    print("unpacked", root)
    print("IMGUI_ROOT=" + root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
