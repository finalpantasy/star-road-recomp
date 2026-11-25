r"""Build the minimal LGPL-only FFmpeg that the movie HLE loads at run time (task-21, D-011). Our own script.

  python -I tools/ffmpeg/build.py [--jobs N] [--clean] [--verify-only]

What it does (all on A:, nothing from FFmpeg ever enters Git):
  1. downloads the pinned release source and NASM into A:\AI\Recomps\ffmpeg\dl (sha256 checked), unpacks them,
  2. configures FFmpeg for shared libavcodec + libavutil with ONLY the VP9 decoder and parser (no GPL, no nonfree,
     no avformat/avfilter/swscale/swresample/network, no external libraries, nothing auto-detected),
  3. builds and installs to A:\AI\Recomps\ffmpeg\install (bin\avcodec-*.dll, bin\avutil-*.dll, include\, lib\),
  4. loads the DLL, prints avcodec_license() and checks that it says LGPL (log in build\logs\license.txt).

The CRIWARE movies carry VP9 video only (the USMs have no audio streams, task-3), so no Opus decoder is built.
The runtime opens the DLL with LoadLibrary (runtime/hle/movie_ffmpeg.c); a missing DLL only disables movies.
Runs at BelowNormal priority. Set SMRPG_FFMPEG_DIR to move the install.
"""
import argparse
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request

FFMPEG_VERSION = "9.0.2"
FFMPEG_URL = "https://ffmpeg.org/releases/ffmpeg-%s.tar.xz" % FFMPEG_VERSION
FFMPEG_SHA256 = "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
NASM_VERSION = "3.02"
NASM_URL = "https://www.nasm.us/pub/nasm/releasebuilds/%s/win64/nasm-%s-win64.zip" % (NASM_VERSION, NASM_VERSION)
NASM_SHA256 = "161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7"

BASE = os.environ.get("SMRPG_FFMPEG_DIR", r"A:\AI\Recomps\ffmpeg")
DL = os.path.join(BASE, "dl")
SRC = os.path.join(BASE, "src")
TOOLS = os.path.join(BASE, "tools")
BUILD = os.path.join(BASE, "build")
LOGS = os.path.join(BUILD, "logs")
INSTALL = os.path.join(BASE, "install")
MSYS_BASH = r"A:\msys64\usr\bin\bash.exe"
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000

# Only what the game's movies need. Everything else is off (--disable-everything), then VP9 is added back.
CONFIGURE = [
    "--prefix=" + INSTALL.replace("\\", "/"),
    "--arch=x86_64", "--target-os=mingw32",
    "--cc=clang", "--cxx=clang++", "--ar=llvm-ar", "--ranlib=llvm-ranlib", "--nm=llvm-nm",
    "--strip=llvm-strip", "--windres=llvm-windres",
    "--enable-shared", "--disable-static",
    "--disable-programs", "--disable-doc", "--disable-debug",
    "--disable-autodetect", "--disable-network", "--disable-iconv", "--disable-schannel",
    "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swscale", "--disable-swresample",
    "--disable-everything",
    "--enable-avcodec", "--enable-avutil",
    "--enable-decoder=vp9", "--enable-parser=vp9",
    "--enable-w32threads",
    "--extra-cflags=-O2",
]


def log(msg):
    print("[ffmpeg-build] " + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, sha, name):
    os.makedirs(DL, exist_ok=True)
    dst = os.path.join(DL, name)
    if not os.path.exists(dst):
        log("download " + url)
        with urllib.request.urlopen(url) as r, open(dst + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(dst + ".part", dst)
    got = sha256(dst)
    if got != sha:
        raise SystemExit("sha256 mismatch for %s: %s (expected %s)" % (name, got, sha))
    return dst


def run(cmd, log_path, cwd=None, env=None):
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT,
                           creationflags=BELOW_NORMAL | NO_WINDOW)
    return p.returncode


def prepare_sources():
    tar = fetch(FFMPEG_URL, FFMPEG_SHA256, "ffmpeg-%s.tar.xz" % FFMPEG_VERSION)
    src = os.path.join(SRC, "ffmpeg-" + FFMPEG_VERSION)
    if not os.path.isdir(src):
        os.makedirs(SRC, exist_ok=True)
        log("unpack " + tar)
        import tarfile
        with tarfile.open(tar) as t:
            t.extractall(SRC, filter="data")
    zp = fetch(NASM_URL, NASM_SHA256, "nasm-%s-win64.zip" % NASM_VERSION)
    nasm_dir = os.path.join(TOOLS, "nasm-" + NASM_VERSION)
    if not os.path.isdir(nasm_dir):
        import zipfile
        with zipfile.ZipFile(zp) as z:
            z.extractall(TOOLS)
    return src, nasm_dir


def to_msys(p):
    p = os.path.abspath(p).replace("\\", "/")
    return "/" + p[0].lower() + p[2:] if len(p) > 1 and p[1] == ":" else p


def build(jobs, clean):
    src, nasm_dir = prepare_sources()
    clang = shutil.which("clang")
    if not clang:
        raise SystemExit("clang (llvm-mingw) not on PATH")
    bdir = os.path.join(BUILD, "obj")
    if clean:
        shutil.rmtree(bdir, ignore_errors=True)
        shutil.rmtree(INSTALL, ignore_errors=True)
    os.makedirs(bdir, exist_ok=True)
    env = dict(os.environ)
    env["MSYSTEM"] = "MSYS"
    env["CHERE_INVOKING"] = "1"
    env["PATH"] = os.pathsep.join([os.path.dirname(clang), nasm_dir, r"A:\msys64\usr\bin", env["PATH"]])
    conf = " ".join(["'%s'" % a if " " in a else a for a in CONFIGURE])
    script = "cd %s && %s/configure %s" % (to_msys(bdir), to_msys(src), conf)
    for bad in ("--enable-gpl", "--enable-nonfree", "--enable-version3"):
        assert bad not in script
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, "configure_cmd.txt"), "w") as f:
        f.write(script + "\n")
    log("configure")
    if run([MSYS_BASH, "-c", script], os.path.join(LOGS, "configure.log"), env=env):
        raise SystemExit("configure failed, see " + os.path.join(LOGS, "configure.log"))
    log("make -j%d" % jobs)
    if run([MSYS_BASH, "-c", "cd %s && make -j%d && make install" % (to_msys(bdir), jobs)],
           os.path.join(LOGS, "make.log"), env=env):
        raise SystemExit("make failed, see " + os.path.join(LOGS, "make.log"))


def verify():
    """Load the installed avcodec DLL, print its license, require LGPL. Also checks the VP9 decoder exists and no GPL."""
    bindir = os.path.join(INSTALL, "bin")
    dlls = sorted(f for f in os.listdir(bindir) if f.startswith("avcodec-") and f.endswith(".dll")) if os.path.isdir(bindir) else []
    if not dlls:
        raise SystemExit("no avcodec DLL in " + bindir)
    os.add_dll_directory(bindir)
    lib = ctypes.CDLL(os.path.join(bindir, dlls[0]))
    lib.avcodec_license.restype = ctypes.c_char_p
    lib.avcodec_configuration.restype = ctypes.c_char_p
    lib.avcodec_version.restype = ctypes.c_uint
    lib.avcodec_find_decoder_by_name.restype = ctypes.c_void_p
    lib.avcodec_find_decoder_by_name.argtypes = [ctypes.c_char_p]
    lic = lib.avcodec_license().decode()
    cfg = lib.avcodec_configuration().decode()
    ver = lib.avcodec_version()
    vp9 = lib.avcodec_find_decoder_by_name(b"vp9")
    other = [n for n in (b"h264", b"hevc", b"aac", b"libx264") if lib.avcodec_find_decoder_by_name(n)]
    lines = [
        "dll: " + dlls[0],
        "avcodec_version: %d.%d.%d" % (ver >> 16, (ver >> 8) & 255, ver & 255),
        "avcodec_license: " + lic,
        "avcodec_configuration: " + cfg,
        "vp9 decoder present: %s" % bool(vp9),
        "other decoders present (expected none): %s" % other,
    ]
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, "license.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bad = [a for a in ("--enable-gpl", "--enable-nonfree", "--enable-version3") if a in cfg]
    if "LGPL" not in lic or "GPL" in lic.replace("LGPL", "") or bad or not vp9 or other:
        raise SystemExit("verify FAILED (license %r, bad flags %s)" % (lic, bad))
    log("verify OK: LGPL, VP9 only")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--verify-only", action="store_true")
    a = ap.parse_args()
    if not a.verify_only:
        build(a.jobs, a.clean)
    verify()
    return 0


if __name__ == "__main__":
    sys.exit(main())
