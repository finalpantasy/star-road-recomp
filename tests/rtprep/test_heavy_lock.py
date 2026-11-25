r"""task-27 step 1: machine-wide heavy-slot lock (tools/rtprep/heavy_lock.py).

  python -I tests/rtprep/test_heavy_lock.py [--report PATH]

Uses a private lock file in a temp directory (SMRPG_HEAVY_LOCK_FILE), never the real A:\AI\Recomps\smrpg-work\heavy.lock. Cases:
  mutual exclusion  two real processes: the second prints a waiting line and gets the slot only after the first released it
  stale pid         a lock naming a dead pid is replaced and the stale line is printed
  pid reuse         a live pid with a different creation time is treated as stale
  timeout           a live holder makes a short wait raise HeavyLockTimeout
  nested            a child that inherits SMRPG_HEAVY_HELD does not wait on its ancestor's lock
  skip              SMRPG_HEAVY_LOCK=0 never touches the lock file
  release           the lock file is gone after the body, including after an exception
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RTPREP = os.path.abspath(os.path.join(HERE, "..", "..", "tools", "rtprep"))
sys.path.insert(0, RTPREP)
import heavy_lock as hl  # noqa: E402

CHILD = r"""
import sys, time
sys.path.insert(0, sys.argv[1])
import heavy_lock as hl
mode, hold, tag = sys.argv[2], float(sys.argv[3]), sys.argv[4]
with hl.heavy_slot(tag, wait=60, poll=0.2) as how:
    print("ACQUIRED", tag, how, "%.3f" % time.time(), flush=True)
    time.sleep(hold)
    print("RELEASING", tag, "%.3f" % time.time(), flush=True)
"""

results = []


def check(name, ok, detail=""):
    results.append({"name": name, "status": "passed" if ok else "failed", "detail": detail})
    print(("PASS " if ok else "FAIL ") + name + ((" : " + detail) if detail else ""))


def spawn(script, env, hold, tag, mode="x"):
    return subprocess.Popen([sys.executable, "-I", "-c", CHILD, RTPREP, mode, str(hold), tag], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, creationflags=0x08000000)


def stamp(text, word, tag):
    for ln in text.splitlines():
        p = ln.split()
        if len(p) >= 3 and p[0] == word and p[1] == tag:
            return float(p[-1])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="heavylock_")
    lock = os.path.join(tmp, "heavy.lock")
    env = dict(os.environ, SMRPG_HEAVY_LOCK_FILE=lock)
    env.pop("SMRPG_HEAVY_HELD", None)
    env.pop("SMRPG_HEAVY_LOCK", None)
    os.environ["SMRPG_HEAVY_LOCK_FILE"] = lock
    os.environ.pop("SMRPG_HEAVY_HELD", None)
    os.environ.pop("SMRPG_HEAVY_LOCK", None)
    logs = []

    # mutual exclusion across two processes
    p1 = spawn(None, env, 2.0, "first")
    deadline = time.time() + 15
    while not os.path.exists(lock) and time.time() < deadline:
        time.sleep(0.05)
    check("first process created the lock file", os.path.exists(lock))
    with open(lock) as f:
        rec = json.load(f)
    check("lock record has holder, pid, script, started", all(k in rec for k in ("holder", "pid", "script", "started")) and rec["pid"] == p1.pid and rec["script"] == "first", json.dumps(rec))
    p2 = spawn(None, env, 0.2, "second")
    o1, _ = p1.communicate(timeout=60)
    o2, _ = p2.communicate(timeout=60)
    acq1, rel1 = stamp(o1, "ACQUIRED", "first"), stamp(o1, "RELEASING", "first")
    acq2 = stamp(o2, "ACQUIRED", "second")
    check("second process got the slot only after the first released it", acq1 and rel1 and acq2 and acq2 >= rel1 - 0.01, "first %s..%s second %s" % (acq1, rel1, acq2))
    check("second process printed a waiting line", "[heavy-lock] second waiting for the heavy slot, held by" in o2 and "(first, pid %d" % p1.pid in o2, o2.strip().splitlines()[0] if o2.strip() else "")
    check("lock file removed after both finished", not os.path.exists(lock))

    # stale pid
    dead = subprocess.Popen([sys.executable, "-I", "-c", "pass"], creationflags=0x08000000)
    dead.wait()
    with open(lock, "w") as f:
        json.dump({"holder": "ghost", "pid": dead.pid, "script": "ghost.py", "started": "2020-01-01T00:00:00"}, f)
    check("a dead pid is not alive", not hl.pid_alive(dead.pid))
    t0 = time.time()
    with hl.heavy_slot("stale-test", wait=30, poll=0.2, log=logs.append) as how:
        with open(lock) as f:
            now = json.load(f)
        check("stale lock replaced by ours", how == "held" and now["pid"] == os.getpid() and now["script"] == "stale-test", json.dumps(now))
    check("stale replacement was immediate and announced", time.time() - t0 < 5 and any("stale lock from ghost" in s for s in logs), "; ".join(logs))
    check("lock released after the body", not os.path.exists(lock))

    # pid reuse: our own pid (alive) with a wrong creation time
    with open(lock, "w") as f:
        json.dump({"holder": "old", "pid": os.getpid(), "script": "old.py", "started": "x", "pid_create": 12345}, f)
    logs.clear()
    with hl.heavy_slot("reuse-test", wait=30, poll=0.2, log=logs.append) as how:
        check("live pid with a different creation time is stale", how == "held" and any("stale lock" in s for s in logs), "; ".join(logs))

    # timeout against a live holder
    p3 = spawn(None, env, 6.0, "holder")
    deadline = time.time() + 15
    while not os.path.exists(lock) and time.time() < deadline:
        time.sleep(0.05)
    logs.clear()
    t0 = time.time()
    try:
        with hl.heavy_slot("impatient", wait=1.0, poll=0.3, log=logs.append):
            check("timeout raised", False, "got the slot while a live holder had it")
    except hl.HeavyLockTimeout as e:
        check("timeout raised against a live holder", time.time() - t0 < 4 and "holder" in str(e), str(e))
    check("timeout printed waiting lines", len(logs) >= 2, "%d lines" % len(logs))
    p3.communicate(timeout=60)

    # nested: parent holds, child inherits SMRPG_HEAVY_HELD and must not wait
    with hl.heavy_slot("parent", wait=30, poll=0.2, log=logs.append):
        cenv = dict(os.environ)
        t0 = time.time()
        pc = spawn(None, cenv, 0.0, "child")
        oc, _ = pc.communicate(timeout=30)
        check("child of the holder runs nested without waiting", "ACQUIRED child nested" in oc and time.time() - t0 < 10, oc.strip()[:120])
        with open(lock) as f:
            check("nested call leaves the parent's lock in place", json.load(f)["script"] == "parent")
    check("parent released", not os.path.exists(lock))

    # SMRPG_HEAVY_LOCK=0 skip: even with a live foreign lock
    p4 = spawn(None, env, 3.0, "other")
    deadline = time.time() + 15
    while not os.path.exists(lock) and time.time() < deadline:
        time.sleep(0.05)
    senv = dict(env, SMRPG_HEAVY_LOCK="0")
    ps = spawn(None, senv, 0.0, "skipper")
    os_, _ = ps.communicate(timeout=30)
    check("SMRPG_HEAVY_LOCK=0 skips the lock", "ACQUIRED skipper skipped" in os_, os_.strip()[:120])
    p4.communicate(timeout=60)

    # release on exception
    try:
        with hl.heavy_slot("boom", wait=30, poll=0.2, log=logs.append):
            raise RuntimeError("x")
    except RuntimeError:
        pass
    check("lock released after an exception", not os.path.exists(lock))

    # CLI wrapper: `heavy_lock.py -- <command>` holds the slot while the command runs, passes its exit code on and releases afterwards
    cli = os.path.join(os.path.dirname(os.path.abspath(hl.__file__)), "heavy_lock.py")
    probe = "import json,os,sys; print('REC', json.dumps(json.load(open(os.environ['SMRPG_HEAVY_LOCK_FILE'])))); sys.exit(7)"
    pc = subprocess.run([sys.executable, "-I", cli, "--", sys.executable, "-c", probe], env=env, capture_output=True, text=True, timeout=60)
    rec = {}
    for line in pc.stdout.splitlines():
        if line.startswith("REC "):
            rec = json.loads(line[4:])
    check("CLI wrapper holds the slot while the command runs", rec.get("pid") not in (None, os.getpid()) and bool(rec.get("script")), pc.stdout.strip()[:160])
    check("CLI wrapper returns the command's exit code", pc.returncode == 7, "rc %d" % pc.returncode)
    check("CLI wrapper released the slot", not os.path.exists(lock))
    check("CLI name comes from the script, not the interpreter", hl._auto_name([sys.executable, "-I", "tests/test_sync.py", "--report", "x"]) == "test_sync.py")

    failed = [r for r in results if r["status"] != "passed"]
    print("heavy_lock: %d checks, %d failed" % (len(results), len(failed)))
    if a.report:
        with open(a.report, "w") as f:
            json.dump({"tests": results}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
