r"""task-14 AC3 static check: every code pointer IL2CPP can store into a runtime-filled generic method slot is a lifted
function start (so a call through the slot never falls back to the interpreter). Our own code.

  python -I tools/lifter/check_generic_slots.py [--json out.json]

The slot fill path (docs/RECOMPILER.md 4.6) runs inside lifted libil2cpp: il2cpp::vm::MetadataCache resolves a method
usage slot to a MethodInfo whose methodPointer / invoker_method / virtualMethodPointer come from the registration tables
(CodeGenModule.methodPointers, genericMethodPointers through genericMethodTable, invokerPointers, adjustor thunks,
unresolvedVirtualCallPointers, interop and reverse P/Invoke wrappers). Calls through those fields are `blr` in lifted
code, which rt_icall resolves in the dispatch table. So "0 interpreter fallbacks for generic slots" holds exactly when
every non-null pointer in those tables is a function start that the lifter lifted (not one listed with a reason).

Inputs: build\img\main.img + main.rel (relocated here; RELATIVE values are module offsets), analysis\il2cpp\
registrations.json (task-2), build\inv\main.tsv and build\logs\lift_main.json (task-5 lift). Exit 0 PASS, 1 FAIL.
"""
import argparse
import collections
import json
import os
import struct
import sys

WORK = r"A:\AI\Recomps\smrpg-work"
IMG = os.path.join(WORK, "build", "img")
BASE = 0x7100000000
CODEGEN_FIELDS = 13          # Il2CppCodeGenModule fields read (Unity 2021.3, metadata v29), all 8 bytes wide


def load():
    img = bytearray(open(os.path.join(IMG, "main.img"), "rb").read())
    rb = open(os.path.join(IMG, "main.rel"), "rb").read()
    n = struct.unpack_from("<I", rb, 4)[0]
    for i in range(n):
        off, typ, _sym, add = struct.unpack_from("<QIIq", rb, 16 + i * 24)
        if typ == 1027 and off + 8 <= len(img):
            struct.pack_into("<Q", img, off, add & 0xFFFFFFFFFFFFFFFF)   # module offset (base 0)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    a = ap.parse_args()
    img = load()
    reg = json.load(open(os.path.join(WORK, "analysis", "il2cpp", "registrations.json")))
    cr, mr = reg["code_registration"], reg["metadata_registration"]
    h = lambda v: int(v, 16) if isinstance(v, str) else int(v)
    u64 = lambda o: struct.unpack_from("<Q", img, o)[0]
    i32s = lambda o, k: struct.unpack_from("<%di" % k, img, o) if k else ()

    def arr(o, k):
        return [u64(h(o) + 8 * i) for i in range(h(k))] if h(o) else []
    ptrs = collections.defaultdict(list)          # kind -> [module offset]
    ptrs["genericMethodPointers"] = arr(cr["genericMethodPointers"], cr["genericMethodPointersCount"])
    ptrs["invokerPointers"] = arr(cr["invokerPointers"], cr["invokerPointersCount"])
    ptrs["unresolvedVirtualCallPointers"] = arr(cr["unresolvedVirtualCallPointers"], cr["unresolvedVirtualCallCount"])
    ptrs["reversePInvokeWrappers"] = arr(cr["reversePInvokeWrappers"], cr["reversePInvokeWrapperCount"])
    for i in range(h(cr["interopDataCount"])):
        row = struct.unpack_from("<7Q", img, h(cr["interopData"]) + 56 * i)
        ptrs["interopData"] += [p for p in row[:5]]
    gmt = [struct.unpack_from("<4i", img, h(mr["genericMethodTable"]) + 16 * i) for i in range(h(mr["genericMethodTableCount"]))]
    nadj = max([e[3] for e in gmt] + [-1]) + 1
    ptrs["genericAdjustorThunks"] = [u64(h(cr["genericAdjustorThunks"]) + 8 * k) for k in range(nadj)]
    # rows: (genericMethodIndex into methodSpecs, methodIndex into genericMethodPointers, invokerIndex, adjustorThunkIndex)
    nspec = h(mr["methodSpecsCount"])
    bad_index = sum(1 for e in gmt if not (0 <= e[0] < nspec) or not (0 <= e[1] < len(ptrs["genericMethodPointers"]))
                    or not (-1 <= e[2] < len(ptrs["invokerPointers"])) or not (-1 <= e[3] < nadj))
    for p in arr(cr["codeGenModules"], cr["codeGenModulesCount"]):
        f = struct.unpack_from("<%dQ" % CODEGEN_FIELDS, img, p)
        n = f[1] & 0xFFFFFFFF
        ptrs["module.methodPointers"] += [u64(f[2] + 8 * k) for k in range(n)]
        for k in range(f[3] & 0xFFFFFFFF):
            ptrs["module.adjustorThunks"].append(u64(f[4] + 16 * k + 8))
    # lifted starts: the main inventory minus functions the lifter listed with a reason (those run interpreted)
    starts = set()
    for line in open(os.path.join(WORK, "build", "inv", "main.tsv")):
        c = line.split("\t")
        if c and c[0].strip():
            try:
                starts.add(int(c[0], 16))
            except ValueError:
                pass
    lift = json.load(open(os.path.join(WORK, "build", "logs", "lift_main.json")))
    listed = {fa - BASE for fa, _sz, _cat, _msg in lift["failed"]}
    res = {}
    total_bad = 0
    for kind, vals in sorted(ptrs.items()):
        nz = [v for v in vals if v]
        not_start = sorted({v for v in nz if v not in starts})
        interp = sorted({v for v in nz if v in listed})
        res[kind] = dict(slots=len(vals), non_null=len(nz), unique=len(set(nz)), not_a_function_start=len(not_start),
                         listed_with_reason=len(interp), examples=["%#x" % v for v in (not_start + interp)[:8]])
        total_bad += len(not_start) + len(interp)
    allp = {v for vals in ptrs.values() for v in vals if v}
    out = dict(ok=total_bad == 0 and bad_index == 0, unique_code_pointers=len(allp), bad_generic_method_table_rows=bad_index,
               inventory_starts=len(starts), lifter_listed_with_reason=len(listed), tables=res)
    for k, r in res.items():
        print("%-32s slots %6d non-null %6d unique %6d  not a start %d  interpreted %d %s" % (
            k, r["slots"], r["non_null"], r["unique"], r["not_a_function_start"], r["listed_with_reason"], " ".join(r["examples"])))
    print("unique code pointers %d; generic method table rows with a bad index %d" % (len(allp), bad_index))
    print("RESULT", "PASS" if out["ok"] else "FAIL")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    return 0 if out["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
