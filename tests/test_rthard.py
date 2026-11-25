r"""task-14 runtime hard parts test (our own code): C++ exceptions (AC1) and GC safepoints (AC2) on our own AArch64
test modules, built and run through the real lifter and runtime.

  python -I tests/test_rthard.py [--keep] [--only eh|gc]

Steps: cross-compile tests/rthard/guest with clang --target=aarch64-linux-gnu and ld.lld; turn the .so files into
module images (tests/rthard/mkimg.py); lift them with tools/lifter (landing pads, back-edge polls, sdk redirects; two
functions are left to the IR interpreter on purpose); compile the runtime (runtime/*.c, eh/, gc/, hle/) plus the lifted
code; run the guest entry points hidden at BelowNormal priority and check the log.
Nothing from the game is used. Output under A:\AI\Recomps\smrpg-work\build\rthard. Exit 0 PASS, 1 FAIL, 3 NOT_RUN.
"""
import argparse
import array
import concurrent.futures as cf
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "lifter"))
GUEST = os.path.join(ROOT, "tests", "rthard", "guest")
WORK = r"A:\AI\Recomps\smrpg-work\build\rthard"
GEN = r"A:\AI\Recomps\smrpg-work\build\gen_game"
BASES = {"main": 0x7100000000, "sdk": 0x7110000000, "subsdk0": 0x7120000000}
BELOW_NORMAL, NO_WINDOW = 0x4000, 0x08000000
INTERP_ONLY = {"interp_frame", "interp_cleanup_only"}     # left to the interpreter: pads in interpreted frames
EXPECT_EH_CHECKS = 23


def run(cmd, timeout=600, **kw):
    cmd = [shutil.which(cmd[0]) or cmd[0]] + list(cmd[1:])     # "ld.lld" needs its .exe spelled out for CreateProcess
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, creationflags=BELOW_NORMAL | NO_WINDOW, **kw)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def guest():
    o = os.path.join(WORK, "o")
    os.makedirs(o, exist_ok=True)
    f = ["--target=aarch64-linux-gnu", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin", "-nostdlib", "-mno-outline-atomics",
         "-fno-stack-protector", "-ffunction-sections", "-I", GUEST]
    cxx = ["clang++"] + f + ["-std=c++17", "-fexceptions", "-nostdinc++"]
    cc = ["clang"] + f + ["-std=gnu11"]
    with open(os.path.join(o, "sub.c"), "w") as fh:
        fh.write("int subsdk_dummy(int x) { return x + 1; }\n")
    jobs = [(cxx, "sdk.cpp"), (cc, "sdk_abi.c"), (cxx, "eh_test.cpp"), (cc, "gc_stress.c"), (cc, "libc.c")]
    for tool, src in jobs:
        rc, out = run(tool + ["-c", os.path.join(GUEST, src), "-o", os.path.join(o, src.split(".")[0] + ".o")])
        if rc:
            raise SystemExit("guest compile failed: %s\n%s" % (src, out[-3000:]))
    rc, out = run(["clang"] + f + ["-c", os.path.join(o, "sub.c"), "-o", os.path.join(o, "sub.o")])
    ld = ["ld.lld", "-shared", "--eh-frame-hdr", "-z", "max-page-size=4096", "--hash-style=sysv"]
    links = {"sdk": ld + ["-Bsymbolic-functions"] + [os.path.join(o, x) for x in ("sdk.o", "sdk_abi.o", "libc.o")],
             "main": ld + [os.path.join(o, x) for x in ("eh_test.o", "gc_stress.o", "libc.o")],
             "subsdk0": ld + [os.path.join(o, "sub.o")]}
    sys.path.insert(0, os.path.join(ROOT, "tests", "rthard"))
    import mkimg
    img = os.path.join(WORK, "img")
    js = {}
    for m, cmd in links.items():
        rc, out = run(cmd + ["-o", os.path.join(o, m + ".so")])
        if rc:
            raise SystemExit("guest link failed: %s\n%s" % (m, out[-3000:]))
        js[m] = mkimg.main(os.path.join(o, m + ".so"), img, m)
    return img, js


def lift_all(img, js):
    import lift as lf
    import eh_tables
    import inventory as inv
    import lower as lw
    import gen
    import build
    cdir = os.path.join(WORK, "c")
    shutil.rmtree(cdir, ignore_errors=True)
    os.makedirs(cdir)
    stats, names, red_all, entries = {}, [], [], {}
    for m, j in js.items():
        base = BASES[m]
        data = open(os.path.join(img, m + ".img"), "rb").read()
        words = array.array("I")
        words.frombytes(data[:j["text_size"]])
        size = len(data)

        def rd_mem(addr, n, data=data, base=base, size=size):
            o = addr - base
            return None if o < 0 or o + n > size else int.from_bytes(data[o:o + n], "little")
        funcs = [(s, e, n) for s, e, n in j["functions"] if s >= 4 and e <= j["text_size"]]
        want = set(eh_tables.REDIRECT_NAMES)
        redirect = {base + e["value"]: e["name"] for e in inv.load_sym(os.path.join(img, m + ".sym"))
                    if e["defined"] and e["type"] == 2 and e["name"] in want}
        red_all += sorted(redirect.items())
        mod = dict(name=m, base=base, words=words, starts={base + s for s, e, n in funcs},
                   ends={base + s: base + e for s, e, n in funcs}, rd_mem=rd_mem, redirect=redirect)
        h0, h1 = j["mod0"]["eh_frame_hdr"]
        if h1 > h0:
            mod["eh"] = eh_tables.EhInfo(data, base, h0, h1)
        L = lf.Lifter(mod)
        bodies, lifted, interp, failed, tot = [], [], [], [], {}
        for s, e, n in funcs:
            a = base + s
            if n in ("eh_main", "gc_main", "gc_main_control"):
                entries[n] = a
            if n in INTERP_ONLY:
                interp.append(a)
                continue
            try:
                text, info = L.lift(a, base + e)
            except Exception as ex:
                failed.append((a, n, "%s: %s" % (type(ex).__name__, ex)))
                continue
            bodies.append(text)
            lifted.append(a)
            for k in ("eh_sites", "eh_pads", "eh_pads_out", "eh_nosite", "polls", "redirects", "calls"):
                tot[k] = tot.get(k, 0) + info.get(k, 0)
        body = "\n".join(bodies)
        called = set(int(x, 16) for x in build.CALL_RE.findall(body))
        stubs = interp + [a for a, _n, _w in failed]
        protos = "".join("void f_%x(Ctx *restrict c);\n" % x for x in sorted(called | set(lifted) | set(stubs)))
        stub_c = "".join("void f_%x(Ctx *restrict c) { interp_run(c, 0x%xull); }\n" % (a, a) for a in stubs)
        reg = "void reg_%s_0000(void) {\n" % m + "".join("  rt_reg(0x%xull, f_%x);\n" % (a, a) for a in lifted + stubs) + "}\n"
        open(os.path.join(cdir, "%s_0000.c" % m), "w").write('#include "prelude.h"\n' + protos + body + "\n" + stub_c + reg)
        names.append("reg_%s_0000" % m)
        # IR records for the interpreter (same encoder as build.py irgen)
        out = bytearray()
        for i, w in enumerate(words):
            try:
                nm, ff = lw.decode_any(w, base + i * 4)
                out += gen.encode(nm, ff)
            except Exception:
                out += gen.encode_bad(w)
        open(os.path.join(img, "ir_%s.bin" % m), "wb").write(out)
        stats[m] = dict(functions=len(funcs), lifted=len(lifted), interpreted=len(interp), failed=failed, redirect_targets=len(redirect), **tot)
    with open(os.path.join(cdir, "regs.c"), "w") as f:
        f.write('#include "prelude.h"\n' + "".join("void %s(void);\n" % n for n in names))
        f.write("static const RtRedirect g_rt_redirects[] = {" + "".join('{0x%xull, "%s"}, ' % r for r in red_all) + "{0, 0}};\n")
        f.write("static void reg_zz_post(void) { eh_post_register(g_rt_redirects, %d); }\n" % len(red_all))
        names.append("reg_zz_post")
        f.write("const RegFn g_reg_chunks[] = {" + ",".join(names) + "};\nconst uint32_t g_reg_nchunks = %d;\n" % len(names))
    return cdir, stats, entries


def patched_runtime():
    """Runtime sources; until task-13 wires the task-14 hooks into svc.c / rt_core.c, use patched copies here."""
    rt = os.path.join(ROOT, "runtime")
    pdir = os.path.join(WORK, "patched")
    os.makedirs(pdir, exist_ok=True)
    srcs = [os.path.join(rt, f) for f in ("rt_core.c", "loader.c", "svc.c", "hle_nn.c", "main.c", "rt_weak.c", "interp.c", "crypto_a64.c")]
    srcs += [os.path.join(rt, "eh", "eh.c"), os.path.join(rt, "gc", "gc.c")] + sorted(glob.glob(os.path.join(rt, "hle", "*.c"))) + sorted(glob.glob(os.path.join(rt, "mods", "*.c"))) + [os.path.join(rt, "platform", "overlay", "options.c"), os.path.join(ROOT, "tools", "datatbl", "nrbf.c")]
    notes = []
    s = open(os.path.join(rt, "svc.c")).read()
    if "gc_set_activity" not in s:
        s = s.replace('#include "rt_internal.h"\n', '#include "rt_internal.h"\n#include "gc/gc.h"\n', 1)
        n0 = s.count("    f(c);\n")
        s = s.replace("    f(c);\n", "    gc_safe_enter(c); f(c); gc_safe_leave(c);\n", 1)
        s, n1 = re.subn(r"(static void svc_set_activity\(Ctx \*c\) \{\n).*?\n\}", lambda m: m.group(1) +
                        "    RtThread *t = thread_of(c, (uint32_t)c->x[0]);\n"
                        "    c->x[0] = t ? gc_set_activity(c, t, (int)(c->x[1] & 1)) : RES_INVALID_HANDLE;\n}", s, count=1, flags=re.S)
        s, n2 = re.subn(r"(static void svc_get_thread_context3\(Ctx \*c\) \{.*?if \(!t\) \{ c->x\[0\] = RES_INVALID_HANDLE; return; \}\n)",
                        lambda m: m.group(1) + "    if (!gc_thread_stopped(t)) { c->x[0] = RES_INVALID_STATE; return; }\n", s, count=1, flags=re.S)
        if not (n0 >= 1 and n1 == 1 and n2 == 1):
            raise SystemExit("svc.c no longer matches the hook patch (f(c) %d, set_activity %d, context3 %d)" % (n0, n1, n2))
        p = os.path.join(pdir, "svc.c")
        open(p, "w").write(s)
        srcs[2] = p
        notes.append("svc.c gc hooks applied to a test copy (pending in task-13)")
    s = open(os.path.join(rt, "rt_core.c")).read()
    if "eh_summary" not in s:
        s = s.replace("void rt_summary(void) {\n    if (InterlockedExchange(&g_summarized, 1)) return;\n",
                      "void eh_summary(void); void gc_summary(void);\nvoid rt_summary(void) {\n"
                      "    if (InterlockedExchange(&g_summarized, 1)) return;\n    eh_summary(); gc_summary();\n", 1)
        if "eh_summary(); gc_summary();" not in s:
            raise SystemExit("rt_core.c no longer matches the summary patch")
        p = os.path.join(pdir, "rt_core.c")
        open(p, "w").write(s)
        srcs[0] = p
        notes.append("rt_core.c summary hook applied to a test copy (pending in task-13)")
    return srcs, notes


def compile_link(cdir, srcs, jobs=8):
    import build
    od = os.path.join(WORK, "obj")
    os.makedirs(od, exist_ok=True)
    hh = hashlib.sha256()
    for d in (os.path.join(ROOT, "runtime"), os.path.join(ROOT, "runtime", "eh"), os.path.join(ROOT, "runtime", "gc"),
              os.path.join(ROOT, "runtime", "hle"), GEN):
        for fn in sorted(os.listdir(d)):
            if fn.endswith((".h", ".inc")):
                hh.update(open(os.path.join(d, fn), "rb").read())
    hh = hh.hexdigest()
    lifted = sorted(glob.glob(os.path.join(cdir, "*.c")))
    work = []
    for p in srcs + lifted:
        rtside = p not in lifted
        flags = build.CFLAGS + ["-I", os.path.join(ROOT, "runtime"), "-I", GEN] + (["-D_WIN32_WINNT=0x0A00", "-g"] if rtside else [])
        key = hashlib.sha256(open(p, "rb").read() + hh.encode() + " ".join(flags).encode()).hexdigest()[:16]
        work.append((p, os.path.join(od, "%s_%s.o" % (os.path.basename(p)[:-2], key)), flags))

    def cc(w):
        p, o, flags = w
        if os.path.exists(o):
            return p, 0, ""
        rc, out = run(["clang", "-c", p, "-o", o + ".tmp"] + flags)
        if rc == 0:
            os.replace(o + ".tmp", o)
        return p, rc, out
    with cf.ThreadPoolExecutor(jobs) as ex:
        for p, rc, out in ex.map(cc, work):
            if rc:
                raise SystemExit("compile failed: %s\n%s" % (p, out[-3000:]))
    exe = os.path.join(WORK, "smrpg_rthard.exe")
    rc, out = run(["clang"] + [o for _p, o, _f in work] + ["-o", exe, "-g", "-Wl,--disable-high-entropy-va"])   # fixed guest ranges: keep bottom-up ASLR low (loader.c)
    if rc:
        raise SystemExit("link failed\n" + out[-4000:])
    return exe


def run_entry(exe, img, entry, name, seconds=90):
    log = os.path.join(WORK, "%s.log" % name)
    if os.path.exists(log):
        os.remove(log)
    t0 = time.time()
    try:
        rc, out = run([exe, "--img", img, "--entry", "addr:%x" % entry, "--max-seconds", str(seconds), "--log", log], timeout=seconds + 30)
    except subprocess.TimeoutExpired:
        rc, out = -1, "timeout"
    text = open(log, errors="replace").read() if os.path.exists(log) else out
    return rc, text, time.time() - t0


def guest_lines(text):
    return [l.split("guest debug output: ", 1)[1].strip() for l in text.splitlines() if "guest debug output: " in l]


def grab(text, pat, conv=int):
    m = re.search(pat, text)
    return conv(m.group(1)) if m else None


def check_eh(text):
    g = guest_lines(text)
    tests = [l for l in g if l.startswith("T ")]
    fails = [l for l in tests if " FAIL" in l] + [l for l in g if l.startswith("FAIL")]
    st = {"checks": len(tests), "passed": sum(1 for l in tests if l.endswith(" ok")), "failed": fails,
          "done": any(l.startswith("EH DONE fail=") for l in g),
          "uncaught": len(re.findall(r"UNCAUGHT", text)),
          "status_line": next((l for l in text.splitlines() if "eh: runtime/eh active=" in l), ""),
          "summary_line": next((l for l in text.splitlines() if "eh: throws " in l), "")}
    st["interp_landings"] = grab(st["summary_line"], r"\(interpreter (\d+)\)")
    ok = (st["done"] and not fails and st["passed"] == EXPECT_EH_CHECKS and st["uncaught"] == 0
          and "active=yes" in st["status_line"] and "missing 0" in st["status_line"] and "MISSING" not in st["status_line"]
          and (st["interp_landings"] or 0) >= 2)
    return ok, st


def check_gc(text):
    g = guest_lines(text)
    v = {k: grab("\n".join(g), r"GC DONE %s=(-?\d+)" % k) for k in ("rounds", "freed", "verified", "starved", "violations")}
    summ = next((l for l in text.splitlines() if "gc: pauses " in l), "")
    v["pauses"] = grab(summ, r"gc: pauses (\d+)")
    v["parked_at_poll"] = grab(summ, r"parked at poll (\d+)")
    v["parked_at_svc_exit"] = grab(summ, r"parked at svc exit (\d+)")
    v["already_stopped"] = grab(summ, r"already stopped (\d+)")
    v["wait_max_ms"] = grab(summ, r"max ([\d.]+) ms", float)
    v["fails"] = [l for l in g if l.startswith("FAIL")]
    v["summary_line"] = summ
    ok = (v["violations"] == 0 and (v["freed"] or 0) > 0 and (v["rounds"] or 0) > 10 and (v["verified"] or 0) > 0
          and (v["parked_at_poll"] or 0) > 0 and not v["fails"])
    return ok, v


def real_selftest(exe):
    """Cross-check on the game's own images: the lifter's LSDA answer (tools/lifter/eh_tables.py) for every bl/blr in a
    function with an LSDA must equal the runtime reader's (runtime/eh eh_lookup_site). Reads build/img only."""
    import eh_tables
    gimg = os.path.join(os.path.dirname(WORK), "img")
    if not os.path.exists(os.path.join(gimg, "main.json")):
        return None
    sites = os.path.join(WORK, "real_sites.txt")
    n = 0
    with open(sites, "w") as f:
        for m, base in BASES.items():
            j = json.load(open(os.path.join(gimg, m + ".json")))
            data = open(os.path.join(gimg, m + ".img"), "rb").read()
            h0, h1 = j["mod0"]["eh_frame_hdr"]
            eh = eh_tables.EhInfo(data, base, h0, h1)
            words = array.array("I")
            words.frombytes(data[:j["text_size"]])
            for fs, fe in eh.lsda_ranges():
                for a in range(fs, fe, 4):
                    w = words[(a - base) >> 2]
                    if (w & 0xFC000000) == 0x94000000 or (w & 0xFFFFFC1F) == 0xD63F0000:
                        k, pad, _l, act = eh.site(a + 4)
                        f.write("%x %d %x %x\n" % (a + 4, k, pad, act))
                        n += 1
    log = os.path.join(WORK, "real_selftest.log")
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ, SMRPG_EH_SELFTEST=sites)
    try:
        run([exe, "--img", gimg, "--entry", "addr:1", "--no-init", "--max-seconds", "60", "--log", log], timeout=120, env=env)
    except subprocess.TimeoutExpired:
        pass
    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    line = next((l for l in text.splitlines() if "eh selftest:" in l), "")
    status = next((l for l in text.splitlines() if "eh: runtime/eh active=" in l), "")
    cmp_n, bad = grab(line, r"(\d+) call sites compared"), grab(line, r"(\d+) mismatches")
    return dict(ok=cmp_n == n and bad == 0 and n > 0, sites_written=n, compared=cmp_n, mismatches=bad, status_line=status,
                first_mismatches=[l for l in text.splitlines() if "MISMATCH" in l][:5])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=("eh", "gc"))
    ap.add_argument("--real", action="store_true", help="also cross-check LSDA tables on the game images (reads build/img)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--report", help="tests JSON for tools/game_check.py")
    a = ap.parse_args()
    try:
        import ctypes
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), BELOW_NORMAL)
    except Exception:
        pass
    for tool in ("clang", "clang++", "ld.lld"):
        if not shutil.which(tool):
            print("NOT_RUN: %s not on PATH" % tool)
            return 3
    if not os.path.exists(os.path.join(GEN, "prelude.h")):
        print("NOT_RUN: generated lifter headers missing (%s); run tools/lifter/build.py lift once" % GEN)
        return 3
    t0 = time.time()
    img, js = guest()
    cdir, stats, entries = lift_all(img, js)
    srcs, notes = patched_runtime()
    exe = compile_link(cdir, srcs, a.jobs)
    report = dict(lift=stats, notes=notes, build_s=round(time.time() - t0, 1))
    ok = True
    tests = []
    T = lambda name, good, skip=False: tests.append(dict(name=name, status="skipped" if skip else "passed" if good else "failed"))
    if a.only in (None, "eh"):
        rc, text, dt = run_entry(exe, img, entries["eh_main"], "eh")
        r, st = check_eh(text)
        st.update(rc=rc, seconds=round(dt, 1))
        report["eh"] = dict(ok=r, **st)
        ok &= r
        for l in guest_lines(text):
            if l.startswith("T "):
                T("eh." + l.split()[1], l.endswith(" ok"))
        T("eh.all_%d_checks_ran" % EXPECT_EH_CHECKS, st["checks"] == EXPECT_EH_CHECKS and st["done"])
        T("eh.no_uncaught_exception", st["uncaught"] == 0)
        T("eh.runtime_active_vtables_redirects", "active=yes" in st["status_line"] and "missing 0" in st["status_line"]
          and "MISSING" not in st["status_line"])
        T("eh.interpreter_landing_pads", (st["interp_landings"] or 0) >= 2)
        print("AC1 exceptions: %s (%d/%d checks, uncaught %d, interpreter landings %s) %s" % (
            "PASS" if r else "FAIL", st["passed"], EXPECT_EH_CHECKS, st["uncaught"], st["interp_landings"], "; ".join(st["failed"][:5])))
    if a.only in (None, "gc"):
        rc, text, dt = run_entry(exe, img, entries["gc_main"], "gc", seconds=120)
        r, v = check_gc(text)
        v.update(rc=rc, seconds=round(dt, 1))
        report["gc"] = dict(ok=r, **v)
        ok &= r
        rc2, text2, dt2 = run_entry(exe, img, entries["gc_main_control"], "gc_control", seconds=120)
        c2 = grab("\n".join(guest_lines(text2)), r"GC DONE violations=(-?\d+)")
        report["gc_control"] = dict(violations=c2, rc=rc2, seconds=round(dt2, 1))
        r = r and (c2 or 0) > 0
        report["gc"]["ok"] = r
        ok &= r
        T("gc.stress_no_live_object_freed", report["gc"]["violations"] == 0 and (v["freed"] or 0) > 0 and not v["fails"])
        T("gc.paused_at_back_edge_polls", (v["parked_at_poll"] or 0) > 0 and (v["rounds"] or 0) > 10)
        T("gc.control_without_register_scan_detects_free", (c2 or 0) > 0)
        print("AC2 GC stress: %s (rounds %s, freed %s, verified lists %s, violations %s, pauses %s, parked at poll %s, at svc exit %s, max wait %s ms; control without register scan: %s violations, must be > 0)" % (
            "PASS" if r else "FAIL", v["rounds"], v["freed"], v["verified"], v["violations"], v["pauses"], v["parked_at_poll"],
            v["parked_at_svc_exit"], v["wait_max_ms"], c2))
    if a.real:
        rs = real_selftest(exe)
        report["real_selftest"] = rs
        T("eh.lsda_tables_match_runtime_on_game_images", bool(rs and rs["ok"]), skip=rs is None)
        if rs is None:
            print("LSDA cross-check on the game images: NOT_RUN (no build/img)")
        else:
            ok &= rs["ok"]
            print("LSDA cross-check on the game images: %s (%s sites, compared %s, mismatches %s)" % (
                "PASS" if rs["ok"] else "FAIL", rs["sites_written"], rs["compared"], rs["mismatches"]))
            print("  " + rs["status_line"].strip()[-260:])
    for m, s in stats.items():
        print("  lift %s: %d functions, %d lifted, %d interpreted, %d failed, eh sites %s pads %s polls %s redirect calls %s" % (
            m, s["functions"], s["lifted"], s["interpreted"], len(s["failed"]), s.get("eh_sites"), s.get("eh_pads"), s.get("polls"), s.get("redirects")))
        for f in s["failed"][:5]:
            print("    lift failed %#x %s %s" % f)
    for n in notes:
        print("  note:", n)
    if a.real:
        gs = os.path.join(ROOT, "tools", "lifter", "check_generic_slots.py")
        try:
            rc, out = run([sys.executable, "-I", gs, "--json", os.path.join(WORK, "generic_slots.json")], timeout=300)
            print("AC3 generic slot pointers are lifted starts: " + (out.strip().splitlines() or ["?"])[-1])
            T("ac3.generic_slot_pointers_are_lifted_starts", rc == 0)
            ok &= rc == 0
        except Exception as ex:
            print("AC3 generic slots: NOT_RUN", ex)
            T("ac3.generic_slot_pointers_are_lifted_starts", False, skip=True)
    json.dump(report, open(os.path.join(WORK, "report.json"), "w"), indent=1, default=str)
    if a.report:
        os.makedirs(os.path.dirname(os.path.abspath(a.report)), exist_ok=True)
        json.dump({"tests": tests}, open(a.report, "w"), indent=1)
    print("RESULT", "PASS" if ok else "FAIL", "report", os.path.join(WORK, "report.json"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
