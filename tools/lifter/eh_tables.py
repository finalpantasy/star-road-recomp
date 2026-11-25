"""Build-time LSDA call-site tables for the lifter (task-14, docs/RECOMPILER.md 4.4). Our own code.

Mirrors runtime/eh/eh.c eh_lookup_site(): FDE through .eh_frame_hdr, LSDA header, call-site table. The
lifter bakes the answer for each call into the generated C (landing pad label and action-record address);
the runtime walks the action chain against the thrown type when an exception actually arrives.

Site kinds (same numbers as the runtime):
  0 no FDE or no LSDA      the frame has no handlers: return at once
  1 site with a pad        eh_land(), then jump to the pad
  2 site without a pad     return at once
  3 no call-site entry     C++ would call std::terminate; we log and propagate
"""
import bisect
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "il2cpp"))
from ehframe import parse_fde_table, _uleb, _sleb  # noqa: E402

# EH ABI entry points that the sdk also calls directly (bl inside the sdk). The lifter routes direct calls to
# these addresses through rt_icall, and eh_post_register() puts the runtime/eh implementations in the dispatch
# table at the same addresses. Keep in step with the EH_HLE / EH_INT names in runtime/eh/eh.c.
REDIRECT_NAMES = (
    "__cxa_allocate_exception", "__cxa_free_exception", "__cxa_throw", "__cxa_rethrow", "__cxa_begin_catch",
    "__cxa_end_catch", "__cxa_get_exception_ptr", "__cxa_current_exception_type", "__cxa_uncaught_exception",
    "__cxa_uncaught_exceptions", "_Unwind_Resume", "__gxx_personality_v0", "__cxa_call_unexpected", "_ZSt9terminatev",
    "_ZNSt13exception_ptrC1ERKS_", "_ZNSt13exception_ptrC2ERKS_", "_ZNSt13exception_ptrD1Ev", "_ZNSt13exception_ptrD2Ev",
    "_ZNSt13exception_ptraSERKS_", "_ZSt17current_exceptionv", "_ZSt17rethrow_exceptionSt13exception_ptr",
    "__cxa_current_primary_exception", "__cxa_rethrow_primary_exception", "__cxa_increment_exception_refcount",
    "__cxa_decrement_exception_refcount", "_Unwind_RaiseException", "_Unwind_Resume_or_Rethrow",
)


def _enc(img, i, enc):
    """DW_EH_PE value at module offset i, pcrel resolved to a module offset. No indirect (not needed here)."""
    if enc == 0xFF:
        return None, i
    fmt, app, start = enc & 0x0F, enc & 0x70, i
    if fmt in (0x00, 0x04, 0x0C):
        v = struct.unpack_from("<Q" if fmt != 0x0C else "<q", img, i)[0]; i += 8
    elif fmt == 0x01:
        v, i = _uleb(img, i)
    elif fmt == 0x09:
        v, i = _sleb(img, i)
    elif fmt == 0x02:
        v = struct.unpack_from("<H", img, i)[0]; i += 2
    elif fmt == 0x0A:
        v = struct.unpack_from("<h", img, i)[0]; i += 2
    elif fmt == 0x03:
        v = struct.unpack_from("<I", img, i)[0]; i += 4
    elif fmt == 0x0B:
        v = struct.unpack_from("<i", img, i)[0]; i += 4
    else:
        raise ValueError("encoding %#x" % enc)
    if v and app == 0x10:
        v += start
    return v, i


class EhInfo:
    def __init__(self, img, base, h0, h1):
        self.img, self.base = img, base
        fdes, _ = parse_fde_table(img, h0, h1)
        fdes.sort(key=lambda f: f["start"])
        self.starts = [f["start"] for f in fdes]
        self.fdes = fdes
        self.cache = {}
        self.n_lsda = sum(1 for f in fdes if f["lsda"])

    def fde(self, ip_off):
        k = bisect.bisect_right(self.starts, ip_off) - 1
        if k < 0:
            return None
        f = self.fdes[k]
        return f if f["start"] <= ip_off < f["start"] + f["size"] else None

    def lsda(self, lsda_off, func_off):
        key = (lsda_off, func_off)
        r = self.cache.get(key)
        if r is not None:
            return r
        img = self.img
        i = lsda_off
        lpenc = img[i]; i += 1
        lpstart = func_off
        if lpenc != 0xFF:
            lpstart, i = _enc(img, i, lpenc)
        ttenc = img[i]; i += 1
        if ttenc != 0xFF:
            _off, i = _uleb(img, i)
        csenc = img[i]; i += 1
        n, i = _uleb(img, i)
        end = i + n
        sites = []
        while i < end:
            s, i = _enc(img, i, csenc & 0x0F)
            ln, i = _enc(img, i, csenc & 0x0F)
            lp, i = _enc(img, i, csenc & 0x0F)
            act, i = _uleb(img, i)
            sites.append((s, ln, lp, act))
        r = (lpstart, sites, end)
        self.cache[key] = r
        return r

    def site(self, ra):
        """(kind, pad, lsda, action) for a call whose return address is ra (absolute guest addresses)."""
        ip = ra - 1 - self.base
        f = self.fde(ip)
        if not f or not f["lsda"]:
            return (0, 0, 0, 0)
        lsda = f["lsda"]
        lpstart, sites, actions = self.lsda(lsda, f["start"])
        off = ip - lpstart
        for s, ln, lp, act in sites:
            if off < s:
                break
            if off < s + ln:
                if not lp:
                    return (2, 0, self.base + lsda, 0)
                return (1, self.base + lpstart + lp, self.base + lsda, (self.base + actions + act - 1) if act else 0)
        return (3, 0, self.base + lsda, 0)

    def lsda_ranges(self):
        """[(fde_start_abs, fde_end_abs)] of every FDE with an LSDA."""
        return [(self.base + f["start"], self.base + f["start"] + f["size"]) for f in self.fdes if f["lsda"]]
