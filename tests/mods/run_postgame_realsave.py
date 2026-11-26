r"""Star Road Recomp unlock_postgame real-save round trip (task-24 AC3). Our own code.

usage: python -I tests/mods/run_postgame_realsave.py SAVE_FILE [--work DIR]
SAVE_FILE is a save the game itself wrote (UTF-16 JSON, SMR_Save*). It is only read. The test builds postgame_save_tool.c with
runtime/mods/postgame.c (clang), then checks, on a copy under DIR (default: a temp dir next to the save's drive, never the repo):
  1. option off: the gate returns nothing and the bytes are untouched;
  2. option on: parsed field by field, ONLY the gate flags (2312, 2025, 2026, 2027 set; 2311 clear) and nothing else differ;
  3. a second pass over the output changes nothing (already set);
  4. a flag-array length that stays the same or grows only at the end.
No save data is printed beyond field paths and flag ids. Exit 0 pass, 1 fail, 3 not run.
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GATE = {2312: True, 2025: True, 2026: True, 2027: True, 2311: False}
FLAGS = "_global_flag_save"
CF = 0x4000 | 0x08000000


def load(p):
    b = open(p, "rb").read()
    if b[:2] == b"\xff\xfe":
        b = b[2:]
    return json.loads(b.decode("utf-16-le"))


def walk(a, b, path, out):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append(path + "/" + k + " (key added or removed)")
            else:
                walk(a[k], b[k], path + "/" + k, out)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(path + "[len] %d -> %d" % (len(a), len(b)))
        for i in range(min(len(a), len(b))):
            walk(a[i], b[i], "%s[%d]" % (path, i), out)
    elif a != b:
        out.append(path)


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if not args:
        print(__doc__)
        return 1
    src = args[0]
    work = sys.argv[sys.argv.index("--work") + 1] if "--work" in sys.argv else tempfile.mkdtemp(prefix="pg_real_")
    os.makedirs(work, exist_ok=True)
    clang = shutil.which("clang")
    if not clang:
        print("NOT_RUN: clang not found")
        return 3
    exe = os.path.join(work, "postgame_save_tool.exe")
    p = subprocess.run([clang, "-O1", "-Wall", "-std=gnu11", os.path.join(ROOT, "tests", "mods", "postgame_save_tool.c"),
                        os.path.join(ROOT, "runtime", "mods", "postgame.c"), "-o", exe], capture_output=True, text=True, creationflags=CF)
    if p.returncode:
        print(p.stdout + p.stderr + "\nFAIL: build")
        return 1
    orig = os.path.join(work, "orig.save")
    shutil.copyfile(src, orig)
    on1, on2 = os.path.join(work, "on1.save"), os.path.join(work, "on2.save")
    fails = []

    def run(a, b, *x):
        return subprocess.run([exe, a, b, *x], capture_output=True, text=True, creationflags=CF)

    r = run(orig, os.path.join(work, "off.save"), "off")
    if r.returncode != 3 or os.path.exists(os.path.join(work, "off.save")):
        fails.append("off: gate did not return NULL (rc %d)" % r.returncode)
    r = run(orig, on1)
    if r.returncode != 0:
        print(r.stdout)
        print("FAIL: option on did not recognize the save (rc %d)" % r.returncode)
        return 1
    print(r.stdout.strip())
    a, b = load(orig), load(on1)
    diffs = []
    walk(a, b, "", diffs)
    fl_a, fl_b = a[FLAGS], b[FLAGS]
    allowed = []
    for i in range(max(len(fl_a), len(fl_b), 2313)):
        va = fl_a[i] if i < len(fl_a) else False
        vb = fl_b[i] if i < len(fl_b) else False
        if va != vb:
            if i not in GATE or GATE[i] != vb:
                fails.append("flag %d changed to %s, not a gate flag" % (i, vb))
            allowed.append(i)
    others = [d for d in diffs if not d.startswith("/" + FLAGS)]
    if others:
        fails.append("%d non-flag fields differ: %s" % (len(others), others[:5]))
    if len(fl_b) < len(fl_a):
        fails.append("flag array shrank")
    if len(fl_b) > len(fl_a) and any(fl_b[len(fl_a):]) and not set(range(len(fl_a), len(fl_b))) & set(GATE) :
        fails.append("padding is not false")
    if run(on1, on2).returncode != 0 or open(on1, "rb").read() != open(on2, "rb").read():
        fails.append("second pass changed the bytes")
    print("gate flags changed: %s; other fields differing: %d; array length %d -> %d; chapter %s -> %s"
          % (sorted(allowed), len(others), len(fl_a), len(fl_b), a.get("_chapter_number"), b.get("_chapter_number")))
    if not args[1:] and "--work" not in sys.argv:
        shutil.rmtree(work, ignore_errors=True)
    print("PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
