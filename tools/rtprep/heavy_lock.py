r"""Machine-wide heavy-slot lock (task-27 step 1). Our own code.

Two leads (Claude and Codex) share this PC and the owner rule is one heavy process at a time (a build, a game run, a render).
build_game.py, run_game.py and every test runner that launches the game take this lock first:

    sys.path.insert(0, <tools/rtprep>)           # python -I does not put the script directory on sys.path
    from heavy_lock import heavy_slot
    with heavy_slot("build_game.py"):
        ...

Lock file: A:\AI\Recomps\smrpg-work\heavy.lock (override with SMRPG_HEAVY_LOCK_FILE, used by the tests), JSON
{holder, pid, script, started, pid_create}. It is taken atomically (O_CREAT|O_EXCL). A waiter polls every 5 s with a printed line and gives
up after 15 minutes. A lock whose pid is no longer alive (or whose pid now belongs to a different process) is stale and is replaced.
It is released in a finally block, and only if the file still names this process.

Nested calls: the holder exports SMRPG_HEAVY_HELD=<its pid>, so a child that inherits the environment (run_game.py started from a test
runner that already holds the slot) sees its ancestor owns the lock and does not wait on it. SMRPG_HEAVY_LOCK=0 skips the lock outright;
use it only for such nested calls.

CLI (for a one-off shell command that must hold the slot, the exit code is the command's):
    python -I tools/rtprep/heavy_lock.py -- <command...>                 (name taken from the command)
    python -I tools/rtprep/heavy_lock.py run <name> -- <command...>
"""
import contextlib
import ctypes
import json
import os
import subprocess
import sys
import time

LOCK_FILE = os.environ.get("SMRPG_HEAVY_LOCK_FILE") or r"A:\AI\Recomps\smrpg-work\heavy.lock"
WAIT_SECONDS = 15 * 60
POLL_SECONDS = 5
HELD_ENV = "SMRPG_HEAVY_HELD"
SKIP_ENV = "SMRPG_HEAVY_LOCK"
EX_TEMPFAIL = 75

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259


class HeavyLockTimeout(Exception):
    pass


def _create_time(pid):
    """Process creation time as a FILETIME integer, or None when the process is gone. Guards against pid reuse."""
    if os.name != "nt":
        return 0 if _alive_posix(pid) else None
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not h:
        return None
    try:
        code = ctypes.c_ulong()
        if not k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code)) or code.value != _STILL_ACTIVE:
            return None
        c, e, k, u = (ctypes.c_ulonglong() for _ in range(4))
        if not k32.GetProcessTimes(ctypes.c_void_p(h), ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
            return 0
        return c.value
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))


def _alive_posix(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def pid_alive(pid):
    return _create_time(pid) is not None


def _read(path):
    """The parsed lock, None when absent, or {} when it exists but is unreadable (being written, or corrupt)."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except FileNotFoundError:
        return None
    except (OSError, ValueError):
        return {}


def _is_stale(path, d):
    if d is None:
        return False
    if not d:                                   # unreadable: a writer may be mid-write, so only stale once it has sat for a while
        try:
            return time.time() - os.path.getmtime(path) > 30
        except OSError:
            return False
    pid = d.get("pid")
    if not isinstance(pid, int):
        return True
    ct = _create_time(pid)
    if ct is None:
        return True
    want = d.get("pid_create")
    return bool(want) and bool(ct) and want != ct           # the pid is alive but is a different process now


def _try_create(path, script):
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return False
    rec = {"holder": os.environ.get("SMRPG_HEAVY_HOLDER", "unspecified"), "pid": os.getpid(), "script": script,
           "started": time.strftime("%Y-%m-%dT%H:%M:%S"), "pid_create": _create_time(os.getpid()) or 0}
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(rec, f)
    except BaseException:
        with contextlib.suppress(OSError):
            os.remove(path)
        raise
    return True


def _steal_stale(path, seen):
    """Replace a stale lock. A short guard file serialises two waiters that both found it stale, so neither deletes the other's fresh lock."""
    guard = path + ".guard"
    try:
        gfd = os.open(guard, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        with contextlib.suppress(OSError):
            if time.time() - os.path.getmtime(guard) > 30:      # a crashed stealer
                os.remove(guard)
        return False
    os.close(gfd)
    try:
        now = _read(path)
        if now is not None and _is_stale(path, now) and now == seen:
            with contextlib.suppress(OSError):
                os.remove(path)
        return True
    finally:
        with contextlib.suppress(OSError):
            os.remove(guard)


def _describe(d):
    if not d:
        return "an unreadable lock file"
    return "%s (%s, pid %s, since %s)" % (d.get("holder"), d.get("script"), d.get("pid"), d.get("started"))


@contextlib.contextmanager
def heavy_slot(script, wait=WAIT_SECONDS, poll=POLL_SECONDS, path=None, log=print):
    """Hold the machine-wide heavy slot for the body. Raises HeavyLockTimeout after `wait` seconds."""
    path = path or os.environ.get("SMRPG_HEAVY_LOCK_FILE") or LOCK_FILE
    if os.environ.get(SKIP_ENV) == "0":
        yield "skipped"
        return
    held = os.environ.get(HELD_ENV)
    if held and held.isdigit() and int(held) != os.getpid():
        cur = _read(path)
        if cur and cur.get("pid") == int(held) and pid_alive(int(held)):
            yield "nested"                       # an ancestor process holds the slot for this whole call tree
            return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    deadline = time.time() + wait
    announced = 0
    while not _try_create(path, script):
        d = _read(path)
        if d is None:
            continue                             # released between our create attempt and the read: retry at once
        if _is_stale(path, d):
            log("[heavy-lock] stale lock from %s, replacing it" % _describe(d))
            _steal_stale(path, d)
            continue
        if time.time() >= deadline:
            raise HeavyLockTimeout("heavy slot still held by %s after %d s" % (_describe(d), wait))
        log("[heavy-lock] %s waiting for the heavy slot, held by %s" % (script, _describe(d)))
        announced += 1
        time.sleep(poll)
    if announced:
        log("[heavy-lock] %s got the heavy slot" % script)
    os.environ[HELD_ENV] = str(os.getpid())
    try:
        yield "held"
    finally:
        os.environ.pop(HELD_ENV, None)
        cur = _read(path)
        if cur and cur.get("pid") == os.getpid():
            with contextlib.suppress(OSError):
                os.remove(path)


def _auto_name(cmd):
    """Name for the lock record: the first argument that looks like a script or program, else the first word."""
    for a in cmd:
        base = os.path.basename(a.replace("\\", "/"))
        if base.lower().endswith((".py", ".exe", ".bat", ".cmd", ".ps1")) and not base.lower().startswith(("python", "py.")):
            return base
    return os.path.basename(cmd[0].replace("\\", "/")) if cmd else "command"


def main(argv):
    name = cmd = None
    if len(argv) >= 4 and argv[0] == "run" and argv[2] == "--":
        name, cmd = argv[1], argv[3:]
    elif len(argv) >= 2 and argv[0] == "--":
        cmd = argv[1:]
        name = _auto_name(cmd)
    if cmd:
        try:
            with heavy_slot(name):
                return subprocess.call(cmd)
        except HeavyLockTimeout as e:
            print("[heavy-lock]", e)
            return EX_TEMPFAIL
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
