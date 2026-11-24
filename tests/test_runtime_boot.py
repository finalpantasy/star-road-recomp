"""Runtime boot test (task-5): builds runtime/ with a mock game library and boots synthetic module images.

Checks the loader (relocations, symbol resolution, HLE and stub slots), svc HLE (info, heap, threads, sync,
events, unimplemented), the libc and C++ exception HLE, init_array order, and the stop at the first nn
service request (exit code 0). No game data is used: the images are generated here. Under one minute.
Run: python tests/test_runtime_boot.py
"""
import os
import shutil
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "runtime"
WORK = Path(os.environ.get("SMRPG_TEST_DIR", r"A:\AI\Recomps\smrpg-work\build\test_rt"))


def write_module(d, name, size, text, rod, dat, syms, rels, init=None):
    """syms: list of (name, defined, bind, type, value); the null symbol is added as index 0."""
    (d / (name + ".img")).write_bytes(bytes(size))
    allsyms = [("", 0, 0, 0, 0)] + syms
    strtab = bytearray(b"\0")
    recs = bytearray()
    for nm, defined, bind, typ, val in allsyms:
        off = 0
        if nm:
            off = len(strtab)
            strtab += nm.encode() + b"\0"
        recs += struct.pack("<IBBBBQQ", off, defined, bind, typ, 0, val, 0)
    (d / (name + ".sym")).write_bytes(struct.pack("<4sIII", b"SYM0", len(allsyms), len(strtab), 0) + bytes(recs) + bytes(strtab))
    (d / (name + ".rel")).write_bytes(struct.pack("<4sIII", b"REL0", len(rels), 0, 0) + b"".join(struct.pack("<QIIq", *r) for r in rels))
    ia = init or (0, 0)
    (d / (name + ".json")).write_text(
        '{\n "image_size": %d,\n "text_size": %d,\n "rodata_off": %d,\n "rodata_size": %d,\n "data_off": %d,\n "data_size": %d,\n'
        ' "init_array": {\n  "off": %d,\n  "size": %d\n },\n "fini_array": {\n  "off": 0,\n  "size": 0\n }\n}\n'
        % (size, text, rod, 0x1000, dat, size - dat, ia[0], ia[1]))


def make_images(d):
    syms = [
        ("_ZN2nn3hid10InitializeEv", 0, 1, 0, 0),        # 1 undefined, not os/util/init/...: a service
        ("memcpy", 0, 1, 0, 0),                          # 2 HLE override
        ("nninitStartup", 1, 1, 2, 0x200),               # 3 defined in main
        ("__cxa_allocate_exception", 0, 1, 0, 0),        # 4
        ("__cxa_throw", 0, 1, 0, 0),                     # 5
        ("__cxa_begin_catch", 0, 1, 0, 0),               # 6
        ("__cxa_end_catch", 0, 1, 0, 0),                 # 7
        ("_ZN2nn2os14SomethingOtherEv", 0, 1, 0, 0),     # 8 os namespace: stub, not a service
    ]
    rels = [(0x2010, 1025, 1, 0), (0x2008, 1026, 2, 0), (0x2018, 1027, 0, 0x200), (0x2020, 1026, 4, 0), (0x2028, 1026, 5, 0),
            (0x2030, 1026, 6, 0), (0x2038, 1026, 7, 0), (0x2040, 1026, 8, 0), (0x3000, 1027, 0, 0x100)]
    write_module(d, "main", 0x4000, 0x1000, 0x1000, 0x2000, syms, rels, init=(0x3000, 8))
    write_module(d, "sdk", 0x2000, 0x1000, 0x1000, 0x1800, [("_ZN2nn4init5StartEmmPFvvES2_S2_", 1, 1, 2, 0x80)], [])
    write_module(d, "subsdk0", 0x2000, 0x1000, 0x1000, 0x1800, [], [])


def build(exe):
    clang = shutil.which("clang")
    if not clang:
        raise SystemExit("clang not on PATH")
    srcs = [SRC / f for f in ("rt_core.c", "loader.c", "svc.c", "hle_nn.c", "main.c", "rt_weak.c")]
    cmd = [clang, "-std=c11", "-O1", "-g", "-Wall", "-Wno-comment", "-D_WIN32_WINNT=0x0A00", "-I", str(SRC)] + [str(s) for s in srcs] + \
          [str(ROOT / "tests" / "runtime" / "mock_game.c"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit("build failed")


def run(exe, imgdir, extra=()):
    log = WORK / "boot_mock.log"
    p = subprocess.Popen([str(exe), "--img", str(imgdir), "--log", str(log), "--max-seconds", "30"] + list(extra),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=0x4000)  # BELOW_NORMAL
    try:
        out, _ = p.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        raise SystemExit("boot test timed out\n" + out)
    return p.returncode, out


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    img = WORK / "img"
    img.mkdir(exist_ok=True)
    make_images(img)
    exe = WORK / "smrpg_boot_mock.exe"
    build(exe)
    rc, out = run(exe, img, ["--entry", "start"])
    for need in [
        "module main    mapped at 0x7100000000",
        "relocated main",
        "STUB first call: _ZN2nn3hid10InitializeEv",
        "FIRST NN SERVICE REQUEST: _ZN2nn3hid10InitializeEv",
        "BOOT REACHED: nn service symbol _ZN2nn3hid10InitializeEv",
        "rtld: running init_array of main (1 entries)",
        "ctor main[0/1]",
        "svc 0x29 GetInfo",
        "svc 0x01 SetHeapSize",
        "svc 0x08 CreateThread",
        "guest thread",
        "svc 0x18 WaitSynchronization",
        "UNIMPLEMENTED svc 0x7f",
        "C++ throw:",
        "HLE first call: memcpy",
        "STUB first call: _ZN2nn2os14SomethingOtherEv",
    ]:
        check(need in out, "missing in log: " + need + "\n" + out[-3000:])
    check("FIRST NN SERVICE REQUEST: _ZN2nn2os" not in out, "os symbol flagged as a service")
    check("NOT REACHED" not in out, "guest continued past the first service request")
    check("FATAL" not in out, "fatal in log\n" + out[-3000:])
    check(rc == 0, "exit code %d\n%s" % (rc, out[-3000:]))
    rc2, out2 = run(exe, img, ["--entry", "start", "--continue"])
    check("mock: NOT REACHED" in out2, "--continue did not continue\n" + out2[-2000:])
    print("runtime boot test: PASS (%d log lines)" % out.count("\n"))


if __name__ == "__main__":
    main()
