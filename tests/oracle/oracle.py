"""AC2 oracle test: IR ops (interpreter semantics) and lifted C versus Unicorn (task-5). Our own code.

Run with the Unicorn venv:  A:\\AI\\Recomps\\tools-ext\\venv-unicorn\\Scripts\\python.exe tests/oracle/oracle.py [--ops a,b] [--per-op N] [--states N]
Cases come from instruction words sampled from the owner's `main` .text (never stored in the repo) plus register-field
mutations of them; random machine states with edge-value weighting. Output under A:\\AI\\Recomps\\smrpg-work\\build\\tests\\.
"""
import argparse
import json
import os
import random
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "lifter"))
sys.path.insert(0, os.path.join(ROOT, "tools", "il2cpp"))
import dec  # noqa: E402
import gen  # noqa: E402
import lift  # noqa: E402
import lower as lw  # noqa: E402
import ops  # noqa: E402

OUT = r"A:\AI\Recomps\smrpg-work\build\tests"
IMG = r"A:\AI\Recomps\smrpg-work\build\img"
MEM_BASE, MEM_SIZE = 0x20000000, 0x40000
PC = MEM_BASE + 0x20000
STATE = struct.Struct("<32Q512s4B2I4x2Q")
CASEIN_HDR = struct.Struct("<32sII")
CLANG = "clang"
FPSR_CMP = 0x0800009F   # QC and the IOC/DZC/OFC/UFC/IXC/IDC cumulative bits


def load_text_words():
    p = os.path.join(IMG, "main.text.bin")
    if not os.path.exists(p):
        from nso import NSO
        n = NSO(r"A:\AI\Recomps\smrpg-work\v101\exefs\main")
        open(p, "wb").write(bytes(n.text.data))
    b = open(p, "rb").read()
    return struct.unpack("<%dI" % (len(b) // 4), b)


def lower_word(w, pc=PC):
    try:
        return lw.decode_any(w, pc)
    except dec.Unsupported:
        return None, None
    except Exception:
        return None, None


def gather_words(per_op, only, seed):
    rnd = random.Random(seed)
    words = load_text_words()
    by_op = {}
    n = len(words)
    for _ in range(1_500_000):
        w = words[rnd.randrange(n)]
        nm, f = lower_word(w)
        if nm is None or nm in gen.CTRL or nm not in ops.OPS:
            continue
        if only and nm not in only:
            continue
        s = by_op.setdefault(nm, set())
        if len(s) < per_op:
            s.add(w)
    # register-field mutations
    out = {}
    for nm, s in by_op.items():
        res = set(s)
        for w in list(s):
            for _ in range(per_op // 2):
                m = w
                for lo in rnd.sample((0, 5, 10, 16), rnd.randint(1, 3)):
                    m = (m & ~(31 << lo)) | (rnd.choice((rnd.randrange(32), 31, 0, 30)) << lo)
                nm2, _ = lower_word(m)
                if nm2 == nm:
                    res.add(m)
        out[nm] = sorted(res)
    return out


EDGE64 = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF,
          0xFFFFFFFF80000000, 0x100000000, 0xFF, 0xFFFF, 0x80, 0x8000]


def rnd64(r):
    k = r.random()
    if k < 0.25:
        return r.choice(EDGE64)
    if k < 0.40:
        return r.randrange(256)
    if k < 0.55:
        return r.getrandbits(32)
    return r.getrandbits(64)


F32_EDGE = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001,
            0xFF800001, 0x00000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F000000, 0x40000000, 0x4B000000,
            0x4F000000, 0xCF000000, 0x4F800000, 0x3EAAAAAB, 0x80000001, 0x33800000]
F64_EDGE = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
            0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0xFFF0000000000001,
            0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
            0x3FE0000000000000, 0x4000000000000000, 0x43E0000000000000, 0x41DFFFFFFFC00000, 0xC1E0000000000000,
            0x4330000000000000, 0x3FD5555555555555, 0x8000000000000001, 0x3CA0000000000000]


def vec16(r):
    k = r.random()
    if k < 0.1:
        return bytes(16)
    if k < 0.15:
        return bytes([255]) * 16
    if k < 0.55:
        # floating-point edge lanes: f32 x4, f64 x2, or a mix of edge values and random bit patterns
        if r.random() < 0.5:
            return struct.pack("<4I", *[r.choice(F32_EDGE) if r.random() < 0.8 else r.getrandbits(32) for _ in range(4)])
        return struct.pack("<2Q", *[r.choice(F64_EDGE) if r.random() < 0.8 else rnd64(r) for _ in range(2)])
    return struct.pack("<QQ", rnd64(r), rnd64(r))


FPCR_CHOICE = lambda r: 0


def make_state(r, nm, f):
    x = [rnd64(r) for _ in range(32)]
    mem_op = ops.OPS[nm]["mem"]
    atomic = nm in ("ldar", "stlr", "ldxr", "stxr", "ldxp", "stxp", "atomic", "cas", "casp")
    if mem_op:
        base = MEM_BASE + 0x10000 + r.randrange(0, 0x8000)
        if atomic or nm.endswith("p") or "pv" in nm:
            base &= ~15
        elif r.random() < 0.5:
            base &= ~15
        if f.get("rn") is not None:
            x[f["rn"]] = base
        if "rm" in f:
            x[f["rm"]] = r.choice((0, 1, 2, 3, 4, 8, 16, 0xFFFFFFFFFFFFFFF0, 0xFFFFFFF0, 5))
            if f.get("opt") in (2, 0, 1):
                x[f["rm"]] &= 0xFF
    vb = b"".join(vec16(r) for _ in range(32))
    fl = [r.randrange(2) for _ in range(4)]
    return dict(x=x, v=vb, fl=fl, fpcr=FPCR_CHOICE(r), fpsr=0, tpidr=rnd64(r), tpidrro=rnd64(r))


def pack_state(s):
    return STATE.pack(*s["x"], s["v"], *s["fl"], s["fpcr"], s["fpsr"], s["tpidr"], s["tpidrro"])


def unpack_state(b):
    t = STATE.unpack(b)
    return dict(x=list(t[:32]), v=t[32], fl=list(t[33:37]), fpcr=t[37], fpsr=t[38], tpidr=t[39], tpidrro=t[40])


def lifted_c(idx, nm, f):
    L = ["void t_%d(Ctx *restrict c) {" % idx,
         "  u64 %s;" % ", ".join("x%d = c->x[%d]" % (n, n) for n in range(31)),
         "  u64 x31 = 0, sp = c->x[31];",
         "  v128 %s;" % ", ".join("v%d = c->v[%d]" % (n, n) for n in range(32)),
         "  u8 fl[4]; memcpy(fl, c->fl, 4);",
         "  " + lift.emit_op(nm, f)]
    L += ["  c->x[%d] = x%d;" % (n, n) for n in range(31)]
    L += ["  c->x[31] = sp;"] + ["  c->v[%d] = v%d;" % (n, n) for n in range(32)]
    L += ["  memcpy(c->fl, fl, 4);", "}"]
    return "\n".join(L)


def run_clang(args):
    return subprocess.run(args, capture_output=True, text=True, creationflags=0x4000)


def take_lock():
    """One oracle run at a time (heavy-process rule): a lock file with a 20 minute stale limit."""
    import atexit
    lp = os.path.join(OUT, "oracle.lock")
    os.makedirs(OUT, exist_ok=True)
    while True:
        try:
            fd = os.open(lp, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            atexit.register(lambda: os.path.exists(lp) and os.remove(lp))
            return
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lp) > 1200:
                    os.remove(lp)
                    continue
            except OSError:
                pass
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ops", default="")
    ap.add_argument("--per-op", type=int, default=24)
    ap.add_argument("--states", type=int, default=8)
    ap.add_argument("--seed", type=int, default=20261124)
    ap.add_argument("--tag", default="int")
    ap.add_argument("--fpcr-modes", action="store_true", help="also test FPCR rounding mode, FZ and DN variants")
    a = ap.parse_args()
    take_lock()
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
    from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_Q0, UC_ARM64_REG_NZCV,
                                     UC_ARM64_REG_FPCR, UC_ARM64_REG_FPSR, UC_ARM64_REG_CPACR_EL1,
                                     UC_ARM64_REG_TPIDR_EL0, UC_ARM64_REG_TPIDRRO_EL0)
    import unicorn
    from unicorn.arm64_const import UC_ARM64_REG_X29, UC_ARM64_REG_X30
    XR = [UC_ARM64_REG_X0 + n for n in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
    os.makedirs(OUT, exist_ok=True)
    if a.fpcr_modes:
        global FPCR_CHOICE
        FPCR_CHOICE = lambda r: 0 if r.random() < 0.4 else (r.getrandbits(2) << 22) | (r.getrandbits(1) << 24) | (r.getrandbits(1) << 25)
    t0 = time.time()
    gdir = gen.write_all(os.path.join(OUT, "gen_" + a.tag))
    only = set(a.ops.split(",")) if a.ops else None
    rnd = random.Random(a.seed)
    by_op = gather_words(a.per_op, only, a.seed)
    print("ops with words:", len(by_op), "words:", sum(len(v) for v in by_op.values()), "%.1fs" % (time.time() - t0))
    # ---- build the C side
    cases = []   # (fn_index, word, nm, f, state)
    fns = []
    wordcase = []
    for nm, ws in sorted(by_op.items()):
        for w in ws:
            nm2, f = lower_word(w)
            if nm2 != nm:
                continue
            # skip unpredictable forms
            if nm in ("ldp", "ldpv", "ldp_wb", "ldpv_wb") and f["rt"] == f["rt2"]:
                continue
            if nm.endswith("_wb") and f.get("rn") in (f.get("rt"), f.get("rt2")) and f.get("rn") != 31:
                continue
            if nm in ("stxr", "stxp") and f["rs"] in (f["rt"], f["rn"], f.get("rt2")):
                continue
            idx = len(fns)
            fns.append(lifted_c(idx, nm, f))
            for _ in range(a.states):
                cases.append((idx, w, nm, f, make_state(rnd, nm, f)))
    print("functions", len(fns), "cases", len(cases))
    CH = 500
    srcs = []
    for k in range(0, len(fns), CH):
        p = os.path.join(OUT, "lt_%s_%d.c" % (a.tag, k // CH))
        with open(p, "w") as fo:
            fo.write('#include "ir_ops.h"\n#include "mode_lift.h"\n#include "ops_gen.h"\n')
            fo.write("\n".join(fns[k:k + CH]))
        srcs.append(p)
    p = os.path.join(OUT, "lt_%s_tab.c" % a.tag)
    with open(p, "w") as fo:
        fo.write('#include "rt.h"\n' + "".join("void t_%d(Ctx*);\n" % i for i in range(len(fns))))
        fo.write("void (*const t_fns[])(Ctx*) = {" + ",".join("t_%d" % i for i in range(len(fns))) + "};\n")
    srcs.append(p)
    flags = ["-DNO_" + x.upper() for x in ("simd_a", "simd_b") if x not in gen.EXT_NAMES] + ["-O2", "-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-I", os.path.join(ROOT, "runtime"),
             "-I", gdir, "-I", os.path.join(ROOT, "runtime")]
    objs = []
    procs = []
    for s in srcs + [os.path.join(ROOT, "tests", "oracle", "runner.c")]:
        o = os.path.join(OUT, a.tag + "_" + os.path.basename(s)[:-2] + ".o")
        objs.append(o)
        procs.append((s, subprocess.Popen([CLANG, "-c", s, "-o", o] + flags, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                          creationflags=0x4000)))
        while sum(1 for _, q in procs if q.poll() is None) >= 12:
            time.sleep(0.2)
    for s, q in procs:
        out = q.communicate()[0]
        if q.returncode:
            print("COMPILE FAIL", s, out[:3000])
            sys.exit(2)
    exe = os.path.join(OUT, "runner_%s.exe" % a.tag)
    r = run_clang([CLANG] + objs + ["-o", exe])
    if r.returncode:
        print(r.stderr[:3000])
        sys.exit(2)
    print("built %.1fs" % (time.time() - t0))
    # ---- write cases and memory
    mem = bytes(rnd.getrandbits(8) for _ in range(MEM_SIZE))
    open(os.path.join(OUT, "mem.bin"), "wb").write(mem)
    cp = os.path.join(OUT, "cases_%s.bin" % a.tag)
    with open(cp, "wb") as fo:
        fo.write(struct.pack("<II", 0x45534143, len(cases)))
        for idx, w, nm, f, st in cases:
            rec = gen.encode(nm, f)
            fo.write(rec + struct.pack("<II", idx, w) + pack_state(st))
    rp = os.path.join(OUT, "results_%s.bin" % a.tag)
    r = subprocess.run([exe, cp, os.path.join(OUT, "mem.bin"), rp], capture_output=True, text=True, creationflags=0x4000)
    print("runner rc", r.returncode, r.stderr[:500])
    res = open(rp, "rb").read()
    CO = STATE.size * 2 + 8 + 128 * 4 * 2
    assert len(res) == CO * len(cases), (len(res), CO, len(cases))
    # ---- unicorn
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(MEM_BASE, MEM_SIZE)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    stats = {}
    fails = {}
    examples = {}
    skipped = 0
    for k, (idx, w, nm, f, st) in enumerate(cases):
        pristine = bytearray(mem)
        pristine[PC - MEM_BASE:PC - MEM_BASE + 4] = struct.pack("<I", w)
        uc.mem_write(MEM_BASE, bytes(pristine))
        for n in range(31):
            uc.reg_write(XR[n], st["x"][n])
        uc.reg_write(UC_ARM64_REG_SP, st["x"][31])
        for n in range(32):
            uc.reg_write(UC_ARM64_REG_Q0 + n, int.from_bytes(st["v"][16 * n:16 * n + 16], "little"))
        nz = (st["fl"][0] << 31) | (st["fl"][1] << 30) | (st["fl"][2] << 29) | (st["fl"][3] << 28)
        uc.reg_write(UC_ARM64_REG_NZCV, nz)
        uc.reg_write(UC_ARM64_REG_FPCR, st["fpcr"])
        uc.reg_write(UC_ARM64_REG_FPSR, st["fpsr"])
        uc.reg_write(UC_ARM64_REG_TPIDR_EL0, st["tpidr"])
        uc.reg_write(UC_ARM64_REG_TPIDRRO_EL0, st["tpidrro"])
        try:
            uc.emu_start(PC, PC + 4, count=1)
        except UcError:
            skipped += 1
            continue
        ux = [uc.reg_read(XR[n]) for n in range(31)] + [uc.reg_read(UC_ARM64_REG_SP)]
        uv = b"".join(uc.reg_read(UC_ARM64_REG_Q0 + n).to_bytes(16, "little") for n in range(32))
        unz = uc.reg_read(UC_ARM64_REG_NZCV)
        ufl = [(unz >> 31) & 1, (unz >> 30) & 1, (unz >> 29) & 1, (unz >> 28) & 1]
        ufpsr = uc.reg_read(UC_ARM64_REG_FPSR)
        newmem = uc.mem_read(MEM_BASE, MEM_SIZE)
        udiff = [(o, newmem[o]) for o in range(MEM_SIZE) if newmem[o] != pristine[o]] if newmem != pristine else []
        base = k * CO
        si = unpack_state(res[base:base + STATE.size])
        sl = unpack_state(res[base + STATE.size:base + 2 * STATE.size])
        ndi, ndl = struct.unpack_from("<II", res, base + 2 * STATE.size)
        di = struct.unpack_from("<128I", res, base + 2 * STATE.size + 8)[:ndi if ndi <= 128 else 0]
        dl = struct.unpack_from("<128I", res, base + 2 * STATE.size + 8 + 512)[:ndl if ndl <= 128 else 0]
        for tag, sx, dd, nd in (("interp", si, di, ndi), ("lifted", sl, dl, ndl)):
            ok = True
            why = []
            if nd == 0xFFFFFFFF:
                ok = False; why.append("fault")
            if nd != 0xFFFFFFFF and sx["x"] != ux:
                ok = False; why.append("x")
            if nd != 0xFFFFFFFF and sx["v"] != uv:
                ok = False; why.append("v")
            if nd != 0xFFFFFFFF and (sx["fpsr"] & FPSR_CMP) != (ufpsr & FPSR_CMP):
                ok = False; why.append("fpsr")
            if nd != 0xFFFFFFFF and sx["fl"] != ufl:
                ok = False; why.append("nzcv")
            if nd != 0xFFFFFFFF and sorted((v >> 8, v & 255) for v in dd) != sorted(udiff):
                ok = False; why.append("mem")
            s = stats.setdefault(nm, [0, 0, 0, 0])
            s[0 if tag == "interp" else 2] += 1
            if not ok:
                s[1 if tag == "interp" else 3] += 1
                fails.setdefault(nm, set()).update(why)
                ex = examples.setdefault(nm, [])
                if len(ex) < 2:
                    ex.append(dict(tag=tag, word="%08x" % w, why=why, fields=f))
    tot = sum(s[0] + s[2] for s in stats.values())
    bad = sum(s[1] + s[3] for s in stats.values())
    rep = dict(unicorn=unicorn.__version__, cases=len(cases), skipped_by_unicorn=skipped, compared=tot, mismatches=bad,
               per_op={k: dict(interp_cases=v[0], interp_fail=v[1], lifted_cases=v[2], lifted_fail=v[3],
                               why=sorted(fails.get(k, []))) for k, v in sorted(stats.items())},
               examples={k: v for k, v in examples.items()}, seconds=round(time.time() - t0, 1))
    jp = os.path.join(OUT, "report_%s.json" % a.tag)
    json.dump(rep, open(jp, "w"), indent=1, default=str)
    print("compared", tot, "mismatches", bad, "skipped", skipped, "report", jp)
    for k, v in sorted(stats.items()):
        if v[1] or v[3]:
            print("  FAIL", k, v, sorted(fails.get(k, [])))


if __name__ == "__main__":
    main()
