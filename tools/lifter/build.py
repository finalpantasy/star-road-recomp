r"""Chunked lift, compile and link driver for the lifted game (task-5). Our own code.

Everything generated goes under A:\AI\Recomps\smrpg-work\build\ (c\ sources, obj\ object cache, logs\, bin\).

  python -I tools/lifter/build.py inv                    # function inventories (sdk, subsdk0 derived; main = task-2)
  python -I tools/lifter/build.py lift main sdk subsdk0  # lift to 400-function chunk files, AC1 report
  python -I tools/lifter/build.py compile [--budget 540] # compile changed chunks (object cache, resumable)
  python -I tools/lifter/build.py link                   # runtime + chunks + registry -> bin\smrpg.exe
  python -I tools/lifter/build.py status

Every long step stops starting new work after --budget seconds (default 540) so one invocation stays under
10 minutes; run it again to continue. All child processes run at BelowNormal priority.
"""
import argparse
import array
import collections
import concurrent.futures as cf
import ctypes
import hashlib
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

BUILD = r"A:\AI\Recomps\smrpg-work\build"
IMG = os.path.join(BUILD, "img")
GEN = os.path.join(BUILD, "gen_game")
CDIR = os.path.join(BUILD, "c")
OBJ = os.path.join(BUILD, "obj")
LOGS = os.path.join(BUILD, "logs")
BIN = os.path.join(BUILD, "bin")
INV = os.path.join(BUILD, "inv")
BASES = {"main": 0x7100000000, "sdk": 0x7110000000, "subsdk0": 0x7120000000}
CHUNK = 400
CLANG = "clang"
CFLAGS = ["-std=gnu11", "-O2", "-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-fno-exceptions",
          "-fno-asynchronous-unwind-tables", "-fno-stack-protector", "-w"]
EXT_NAMES = [x for x in os.environ.get("SMRPG_EXT", "simd_a,simd_b").split(",") if x]
CFLAGS += ["-DNO_" + x.upper() for x in ("simd_a", "simd_b") if x not in EXT_NAMES]
BELOW_NORMAL = 0x4000
NO_WINDOW = 0x08000000


def lower_priority():
    try:
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), BELOW_NORMAL)
    except Exception:
        pass


# ------------------------------------------------------------------------------------------- lifting (workers)
_W = {}


def _worker_init():
    lower_priority()


def _load_mod(name):
    m = _W.get(name)
    if m:
        return m
    import json as _j
    import lift as lf
    import inventory as inv
    j = _j.load(open(os.path.join(IMG, name + ".json")))
    img = open(os.path.join(IMG, name + ".img"), "rb").read()
    tsz = j["text_size"]
    words = array.array("I")
    words.frombytes(img[:tsz])
    base = BASES[name]
    rows = inv.load(name)
    starts = {base + s for s, e, k in rows}
    ends = {base + s: base + e for s, e, k in rows}
    size = len(img)

    def rd_mem(addr, n):
        o = addr - base
        if o < 0 or o + n > size:
            return None
        return int.from_bytes(img[o:o + n], "little")

    mod = dict(name=name, base=base, words=words, starts=starts, ends=ends, rd_mem=rd_mem)
    # task-14: LSDA call-site tables (landing pads) and the EH ABI entry points the runtime replaces in place
    import eh_tables
    eh = j.get("mod0", {}).get("eh_frame_hdr")
    if eh and eh[1] > eh[0]:
        mod["eh"] = eh_tables.EhInfo(img, base, eh[0], eh[1])
    mod["redirect"] = eh_redirects(name)
    m = _W[name] = (lf, lf.Lifter(mod), rows)
    return m


def eh_redirects(name):
    """{abs addr: symbol} for defined functions named in eh_tables.REDIRECT_NAMES (the sdk, in the game)."""
    import inventory as inv
    import eh_tables
    p = os.path.join(IMG, name + ".sym")
    if not os.path.exists(p):
        return {}
    want = set(eh_tables.REDIRECT_NAMES)
    return {BASES[name] + e["value"]: e["name"] for e in inv.load_sym(p)
            if e["defined"] and e["type"] == 2 and e["value"] and e["name"] in want}


CALL_RE = re.compile(r"\bf_([0-9a-f]+)\(c\)")


def lift_chunk(job):
    name, k = job
    lf, lifter, rows = _load_mod(name)
    base = BASES[name]
    part = rows[k * CHUNK:(k + 1) * CHUNK]
    bodies = []
    lifted = []
    failed = []
    tot = collections.Counter()
    flags = collections.Counter()
    for s, e, kind in part:
        a, b = base + s, base + e
        try:
            text, info = lifter.lift(a, b)
        except lf.Unlifted as ex:
            msg = str(ex)
            failed.append((a, b - a, "undecodable", msg))
            continue
        except Exception as ex:  # a lifter bug: record it, do not stop the build
            failed.append((a, b - a, "lifter-error", "%s: %s" % (type(ex).__name__, ex)))
            continue
        bodies.append(text)
        lifted.append(a)
        for key in ("insns", "calls", "tail", "mid", "jt", "jt_unresolved", "br_tail_ptr", "eh_sites", "eh_pads", "eh_pads_out",
                    "eh_nosite", "polls", "redirects"):
            tot[key] += info.get(key, 0)
        if info["mid"]:
            flags["funcs_with_mid_function_targets"] += 1
        if info["jt_unresolved"]:
            flags["funcs_with_unresolved_br"] += 1
    body = "\n".join(bodies)
    called = set(int(x, 16) for x in CALL_RE.findall(body))
    protos = "".join("void f_%x(Ctx *restrict c);\n" % x for x in sorted(called | set(lifted) | set(f[0] for f in failed)))
    # functions the lifter listed with a reason run in the IR interpreter; the stub keeps direct callers linkable
    stubs = "".join("void f_%x(Ctx *restrict c) { interp_run(c, 0x%xull); }" % (fa, fa) + chr(10) for fa, _sz, _cat, _msg in failed)
    reg = ["void reg_%s_%04d(void) {" % (name, k)]
    reg += ["  rt_reg(0x%xull, f_%x);" % (a, a) for a in lifted + [fa for fa, _s, _c, _m in failed]]
    reg.append("}")
    text = '#include "prelude.h"\n' + protos + "\n" + body + "\n" + stubs + "\n".join(reg) + "\n"
    os.makedirs(CDIR, exist_ok=True)
    path = os.path.join(CDIR, "%s_%04d.c" % (name, k))
    data = text.encode()
    try:
        old = open(path, "rb").read()
    except OSError:
        old = None
    if old != data:
        with open(path, "wb") as f:
            f.write(data)
    return dict(mod=name, k=k, path=path, nfuncs=len(part), lifted=len(lifted), failed=failed, tot=dict(tot),
                flags=dict(flags), nbytes=len(data))


IRSEG = 65536


def irgen_seg(job):
    """IR records for words [k*IRSEG, (k+1)*IRSEG) of a module's text: one 32-byte record per word."""
    name, k = job
    import gen
    import lower as lw
    import dec
    lf, lifter, rows = _load_mod(name)
    words = lifter.mod["words"]
    base = BASES[name]
    out = bytearray()
    bad = 0
    for i in range(k * IRSEG, min(len(words), (k + 1) * IRSEG)):
        w = words[i]
        try:
            nm, f = lw.decode_any(w, base + i * 4)
            out += gen.encode(nm, f)
        except dec.Unsupported:
            out += gen.encode_bad(w)
            bad += 1
        except Exception:
            out += gen.encode_bad(w)
            bad += 1
    return k, bytes(out), bad


def cmd_irgen(a):
    lower_priority()
    write_prelude()
    t0 = time.time()
    for m in a.modules or ["main", "sdk", "subsdk0"]:
        lf, lifter, rows = _load_mod(m)
        nw = len(lifter.mod["words"])
        segs = [(m, k) for k in range((nw + IRSEG - 1) // IRSEG)]
        path = os.path.join(IMG, "ir_%s.bin" % m)
        bad = 0
        with mp.Pool(a.jobs, initializer=_worker_init) as pool, open(path + ".tmp", "wb") as f:
            for k, data, b in pool.imap(irgen_seg, segs, chunksize=1):
                f.write(data)
                bad += b
        os.replace(path + ".tmp", path)
        print("%s: %d records, %d undecodable (%.0fs)" % (m, nw, bad, time.time() - t0), flush=True)


def cmd_inv(a):
    import inventory
    mods = a.modules or ["main", "sdk", "subsdk0"]
    for m in mods:
        if m == "main":
            rows = inventory.from_task2()
            inventory.write(m, rows)
            print("main: %d functions (task-2 inventory)" % len(rows))
        else:
            rows, st = inventory.derive(m)
            inventory.write(m, rows)
            print("%s: %d functions %s" % (m, len(rows), st))


def write_prelude():
    import gen
    os.makedirs(GEN, exist_ok=True)
    gen.write_all(GEN)
    with open(os.path.join(GEN, "prelude.h"), "w") as f:
        f.write('#include "ir_ops.h"\n#include "mode_lift.h"\n#include "ops_gen.h"\n')


def cmd_lift(a):
    import inventory
    lower_priority()
    write_prelude()
    t0 = time.time()
    os.makedirs(LOGS, exist_ok=True)
    mods = a.modules or ["main", "sdk", "subsdk0"]
    jobs = []
    os.makedirs(CDIR, exist_ok=True)
    if not a.limit:
        for f in os.listdir(CDIR):
            if f.split("_")[0] in mods and f.endswith(".c"):
                os.remove(os.path.join(CDIR, f))
    for m in mods:
        n = len(inventory.load(m))
        jobs += [(m, k) for k in range((n + CHUNK - 1) // CHUNK)]
    if a.limit:
        jobs = jobs[:a.limit]
    res = collections.defaultdict(list)
    with mp.Pool(a.jobs, initializer=_worker_init) as pool:
        for i, r in enumerate(pool.imap_unordered(lift_chunk, jobs, chunksize=1)):
            res[r["mod"]].append(r)
            if i % 20 == 0:
                print("  lifted %d/%d chunks %.0fs" % (i + 1, len(jobs), time.time() - t0), flush=True)
    report = {}
    for m, rs in res.items():
        rs.sort(key=lambda r: r["k"])
        total = sum(r["nfuncs"] for r in rs)
        lifted = sum(r["lifted"] for r in rs)
        tot = collections.Counter()
        flags = collections.Counter()
        failed = []
        for r in rs:
            tot.update(r["tot"])
            flags.update(r["flags"])
            failed += r["failed"]
        hist = collections.Counter()
        for fa, sz, cat, msg in failed:
            hist[cat + ": " + re.sub(r"^[0-9a-f]+: ", "", msg)[:60]] += 1
        raw_rows = sum(1 for _ in open(os.path.join(INV, m + ".tsv")))
        report[m] = dict(inventory_rows=raw_rows, functions=total, lifted=lifted, listed_with_reason=len(failed), chunks=len(rs),
                         totals=dict(tot), flags=dict(flags), reasons=dict(hist.most_common(60)),
                         failed=sorted(failed))
        with open(os.path.join(LOGS, "lift_%s.json" % m), "w") as f:
            json.dump(report[m], f, indent=1)
        print("%s: %d functions, lifted %d, listed with reason %d; %s; flags %s" %
              (m, total, lifted, len(failed), dict(tot), dict(flags)))
        for kk, v in hist.most_common(12):
            print("    %6d  %s" % (v, kk))
    # registry
    chunks = sorted(f[:-2] for f in os.listdir(CDIR) if f.endswith(".c") and f[:-2].split("_")[0] in BASES)
    names = ["reg_" + c for c in chunks]
    # task-14: EH ABI entry points inside the lifted modules that runtime/eh replaces at their own address. The post
    # hook runs last (after every chunk registered), so its rt_reg calls win over the lifted bodies.
    red = sorted((ad, nm) for m in BASES for ad, nm in eh_redirects(m).items())
    with open(os.path.join(CDIR, "regs.c"), "w") as f:
        f.write('#include "prelude.h"\n' + "".join("void %s(void);\n" % n for n in names))
        f.write("static const RtRedirect g_rt_redirects[] = {" + "".join('{0x%xull, "%s"}, ' % r for r in red) + "{0, 0}};\n")
        f.write("static void reg_zz_post(void) { eh_post_register(g_rt_redirects, %d); }\n" % len(red))
        names.append("reg_zz_post")
        f.write("const RegFn g_reg_chunks[] = {" + ",".join(names) + "};\n")
        f.write("const uint32_t g_reg_nchunks = %d;\n" % len(names))
    print("lift wall %.1fs" % (time.time() - t0))


# ------------------------------------------------------------------------------------------- compile and link
def header_hash():
    h = hashlib.sha256()
    for d in (os.path.join(ROOT, "runtime"), GEN):
        for fn in sorted(os.listdir(d)):
            if fn.endswith((".h", ".inc")) and not (fn.startswith("ir_simd_") and fn[3:-2] not in EXT_NAMES):
                h.update(open(os.path.join(d, fn), "rb").read())
    h.update(" ".join(CFLAGS).encode())
    return h.hexdigest()


def src_obj(path, hh):
    h = hashlib.sha256(open(path, "rb").read() + hh.encode()).hexdigest()[:20]
    return os.path.join(OBJ, "%s_%s.o" % (os.path.basename(path)[:-2], h))


def run_cc(path, obj, extra=()):
    os.makedirs(OBJ, exist_ok=True)
    tmp = obj + ".tmp"
    cmd = [CLANG, "-c", path, "-o", tmp] + CFLAGS + ["-I", os.path.join(ROOT, "runtime"), "-I", GEN] + list(extra)
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=BELOW_NORMAL | NO_WINDOW)
    if p.returncode:
        return path, False, p.stderr[:4000], time.time() - t
    os.replace(tmp, obj)
    return path, True, "", time.time() - t


def cmd_compile(a):
    t0 = time.time()
    lower_priority()
    hh = header_hash()
    files = sorted(os.path.join(CDIR, f) for f in os.listdir(CDIR) if f.endswith(".c"))
    todo = [(p, src_obj(p, hh)) for p in files]
    todo = [(p, o) for p, o in todo if not os.path.exists(o)]
    todo.sort(key=lambda po: -os.path.getsize(po[0]))     # biggest first: a slow chunk never starts near the budget end
    if a.only:
        todo = [(p, o) for p, o in todo if a.only in os.path.basename(p)]
    print("chunks %d, to compile %d" % (len(files), len(todo)), flush=True)
    done = fail = skipped = 0
    errs = []
    times = []
    slow = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        pending = set()
        it = iter(todo)
        stop = False
        while True:
            while not stop and len(pending) < a.jobs:
                if time.time() - t0 > a.budget:
                    stop = True
                    break
                try:
                    p, o = next(it)
                except StopIteration:
                    stop = True
                    break
                pending.add(ex.submit(run_cc, p, o))
            if not pending:
                break
            d, pending = cf.wait(pending, return_when=cf.FIRST_COMPLETED)
            for fu in d:
                p, ok, err, dt = fu.result()
                times.append(dt)
                slow.append((dt, os.path.basename(p)))
                if ok:
                    done += 1
                else:
                    fail += 1
                    errs.append((p, err))
            if (done + fail) % 25 == 0:
                print("  compiled %d failed %d  %.0fs" % (done, fail, time.time() - t0), flush=True)
    left = len(todo) - done - fail
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, "compile_errors.txt"), "w") as f:
        for p, e in errs:
            f.write("== %s\n%s\n" % (p, e))
    print("compiled %d, failed %d, still to do %d; wall %.0fs; mean %.1fs max %.1fs" %
          (done, fail, left, time.time() - t0, sum(times) / max(1, len(times)), max(times or [0])))
    print("slowest:", ", ".join("%s %.0fs" % (n, t) for t, n in sorted(slow, reverse=True)[:5]))
    for p, e in errs[:3]:
        print("FAIL", p, e[:600])


RT_SRC = ["rt_core.c", "loader.c", "svc.c", "hle_nn.c", "main.c", "rt_weak.c", "interp.c", "eh/eh.c", "gc/gc.c"]


def cmd_link(a):
    lower_priority()
    hh = header_hash()
    objs = []
    jobs = []
    for f in sorted(os.listdir(CDIR)):
        if f.endswith(".c"):
            p = os.path.join(CDIR, f)
            o = src_obj(p, hh)
            if not os.path.exists(o):
                print("missing object for", f, "(run compile)")
                return 2
            objs.append(o)
    for f in RT_SRC:
        p = os.path.join(ROOT, "runtime", f)
        if not os.path.exists(p):
            continue
        o = src_obj(p, hh)
        if not os.path.exists(o):
            jobs.append((p, o))
        objs.append(o)
    for p, o in jobs:
        r = run_cc(p, o, ["-D_WIN32_WINNT=0x0A00", "-g"])
        if not r[1]:
            print("FAIL", p, r[2])
            return 2
    os.makedirs(BIN, exist_ok=True)
    rsp = os.path.join(BIN, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(o.replace("\\", "/") for o in objs))
    exe = os.path.join(BIN, "smrpg.exe")
    t = time.time()
    p = subprocess.run([CLANG, "@" + rsp, "-o", exe, "-g"], capture_output=True, text=True,
                       creationflags=BELOW_NORMAL | NO_WINDOW)
    print("link rc %d %.1fs -> %s" % (p.returncode, time.time() - t, exe))
    if p.returncode:
        print(p.stderr[:3000])
        return 2
    return 0


def cmd_status(a):
    for m in BASES:
        p = os.path.join(LOGS, "lift_%s.json" % m)
        if os.path.exists(p):
            j = json.load(open(p))
            print(m, "functions", j["functions"], "lifted", j["lifted"], "listed", j["listed_with_reason"])
    n = len([f for f in os.listdir(OBJ)]) if os.path.isdir(OBJ) else 0
    print("objects", n)


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    for nm, fn in (("inv", cmd_inv), ("irgen", cmd_irgen), ("lift", cmd_lift), ("compile", cmd_compile), ("link", cmd_link),
                   ("status", cmd_status)):
        p = sp.add_parser(nm)
        p.add_argument("modules", nargs="*")
        p.add_argument("--jobs", type=int, default=16)
        p.add_argument("--budget", type=int, default=540)
        p.add_argument("--limit", type=int, default=0)
        p.add_argument("--only", default="")
        p.set_defaults(fn=fn)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
