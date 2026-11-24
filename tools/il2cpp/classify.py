"""Origin classification of every .text function.

Evidence tiers (column `conf`):
  A  exact: code-registration pointer tables, dynamic symbols, import thunks (PLT stubs)
  B  strong string reference (a path or message only one library emits)
  C  inferred: both nearest exact/strong neighbours (by address) have the same class
  D  nearest-guess: neighbours disagree; the function takes the class of its closest
     labelled neighbour, or of a strong call-graph vote, and is flagged as a guess
  U  unknown (no neighbour at all)

Classes: game_csharp, unity_bcl_csharp, cri_csharp, il2cpp_glue, libil2cpp, unity_native,
cri_native, nn_sdk, libc_libcxx, thirdparty_c, unknown.

Usage: python -I tools/il2cpp/classify.py [outdir]
Needs features.pkl from build_features.py.
"""
import bisect
import collections
import json
import os
import pickle
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_tables import Model, OUT_DEFAULT, sanitize  # noqa: E402

CLASSES = ["game_csharp", "unity_bcl_csharp", "cri_csharp", "il2cpp_glue", "libil2cpp", "unity_native",
           "cri_native", "nn_sdk", "libc_libcxx", "thirdparty_c", "unknown"]

GAME_IMAGES = {"Assembly-CSharp.dll", "__Generated", "MeshBakerCore.dll"}


def image_class(name):
    if name in GAME_IMAGES:
        return "game_csharp"
    if name.startswith("CriMw"):
        return "cri_csharp"
    return "unity_bcl_csharp"


def import_class(sym):
    if sym.startswith(("_ZN2nn", "_ZNK2nn", "nn", "__nnDetail", "_ZThn", "__nn")) and "nnmusl" not in sym:
        return "nn_sdk"
    if "2nn" in sym[:8] or sym.startswith("nvn"):
        return "nn_sdk"
    return "libc_libcxx"  # musl libc, libc++ / libc++abi, unwinder, math


def dynsym_class(sym):
    if sym in ("malloc", "free", "calloc", "realloc", "aligned_alloc"):
        return "libc_libcxx"
    if sym.startswith(("nninit", "nnMain", "__nnDetail", "nndetail")) or "2nn" in sym[:8]:
        return "nn_sdk"
    if sym.startswith("Libz"):
        return "thirdparty_c"
    return "libc_libcxx"


# strong string patterns: (class, compiled regex)
STR_RULES = [
    ("libil2cpp", re.compile(r"(?i)\\il2cpp\\|/il2cpp/|libil2cpp|^GC_|Boehm|il2cpp_|Il2CppLog|Baselib_")),
    ("cri_native", re.compile(r"^E\d{8,}|CRIWARE|(?i:\bcri(?:atom|sofdec|mana|file|ware|ssl)[a-z_]*)|(?i:^cri[A-Z]\w+)")),
    ("unity_native", re.compile(r"(?i)^\.?/?(?:Runtime|Modules|Platforms|PlatformDependent|External)/|::Scripting::|PhysX|PxFoundation|FMOD|Program Files\\Unity\\Hub\\Editor\\[^\\]+\\Editor\\Data\\(?!il2cpp)|^UnityEngine")),
    ("unity_native", re.compile(r"m_[A-Z]|GameObject|Texture|Shader|Material|Mesh|Camera|Renderer|Collider|Rigidbody|Animator|Transform|Canvas|Particle|Cubemap|RenderTexture|Profiler|Prefab|Terrain|NavMesh|Joint|Quality|Graphics|Sprite")),
]
WEAK_FROM = 3  # STR_RULES[3:] are weak (still tier B but only used when no strong rule matched)


def classify():
    outdir = sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT
    t0 = time.time()
    M = Model()
    nso, reg, meta = M.nso, M.reg, M.meta
    F = pickle.load(open(os.path.join(outdir, "features.pkl"), "rb"))
    funcs, strs, gots, calls, tails = F["funcs"], F["strs"], F["gots"], F["calls"], F["tails"]
    S = F["strings"]
    nf = len(funcs)
    start_to_k = {f[0]: k for k, f in enumerate(funcs)}
    label = [None] * nf  # (class, sub, conf, evidence, name)
    names = [None] * nf
    comments = {}
    point_only = set()  # functions whose label must not propagate to neighbours

    def put(addr, cls, sub, conf, ev, name=None, override=False):
        k = start_to_k.get(addr)
        if k is None:
            return False
        if label[k] is not None and not override:
            # keep the first (exact) label but fill in a missing name
            if names[k] is None and name:
                names[k] = name
            return True
        label[k] = (cls, sub, conf, ev)
        if name and names[k] is None:
            names[k] = name
        return True

    missing = collections.Counter()
    syms = M.sym_names()
    # --- A1: managed methods
    for mi, p in enumerate(M.method_ptr):
        if p:
            iname = meta.image_name(M.type_image[M.method_type[mi]])
            if not put(p, image_class(iname), iname, "A", "method_ptr", syms[mi]):
                missing["method_ptr"] += 1
            else:
                comments[start_to_k[p]] = "%s | token %#x | method %d" % (M.method_full_name(mi), meta.methods[mi].token, mi)
    # --- A2: generic method instances (class by method definition's image)
    gtab = {gi: (mi, inv, adj) for (gi, mi, inv, adj) in reg.generic_method_table}
    gen_name = {}
    for si, (mdi, ci, mi_) in enumerate(reg.method_specs):
        e = gtab.get(si)
        if not e:
            continue
        gmi = e[0]
        if not (0 <= gmi < len(reg.generic_method_pointers)):
            continue
        p = reg.generic_method_pointers[gmi]
        if not p:
            continue
        iname = meta.image_name(M.type_image[M.method_type[mdi]])
        nm = "Generic$$" + sanitize(M.namer.typedef_name(M.method_type[mdi])) + "$$" + sanitize(meta.method_name(mdi)) + "_g%d" % si
        if not put(p, image_class(iname), iname, "A", "generic_method_ptr", nm):
            missing["generic_ptr"] += 1
    # --- A3: il2cpp glue
    for i, p in enumerate(reg.invoker_pointers):
        if p and not put(p, "il2cpp_glue", "invoker", "A", "invoker_ptr", "il2cpp_invoker_%d" % i):
            missing["invoker"] += 1
    for i, p in enumerate(reg.unresolved_virtual):
        if p and not put(p, "il2cpp_glue", "unresolved_virtual", "A", "unresolved_virtual_ptr", "il2cpp_unresolved_virtual_%d" % i):
            missing["unresolved_virtual"] += 1
    for i, p in enumerate(reg.reverse_pinvoke):
        if p and not put(p, "il2cpp_glue", "reverse_pinvoke", "A", "reverse_pinvoke_ptr", "il2cpp_reverse_pinvoke_%d" % i):
            missing["reverse_pinvoke"] += 1
    kinds = ("delegate_pinvoke", "marshal_to_native", "marshal_from_native", "marshal_cleanup", "create_ccw")
    for i, row in enumerate(reg.interop):
        tname = ""
        if row[6] in reg.type_addr_to_index:
            tname = sanitize(M.namer.type_name(reg.type_addr_to_index[row[6]]))
        for j, kname in enumerate(kinds):
            if row[j] and not put(row[j], "il2cpp_glue", "interop", "A", "interop_" + kname, "Interop$$%s$$%s" % (tname, kname)):
                missing["interop"] += 1
    for mod in reg.modules:
        if mod["adjustorThunkCount"]:
            for k in range(mod["adjustorThunkCount"]):
                p = reg.u64(mod["adjustorThunks"] + 16 * k + 8)
                if p and not put(p, "il2cpp_glue", "adjustor_thunk", "A", "adjustor_thunk", "il2cpp_adjustor_%s_%d" % (sanitize(mod["name"]), k)):
                    missing["adjustor"] += 1
    # generic adjustor thunks: count unknown, indexes referenced by the generic method table
    gadj_n = max([e[3] for e in reg.generic_method_table] + [-1]) + 1
    for k in range(gadj_n):
        p = reg.u64(reg.code_reg["genericAdjustorThunks"] + 8 * k)
        if p and not put(p, "il2cpp_glue", "adjustor_thunk", "A", "generic_adjustor_thunk", "il2cpp_generic_adjustor_%d" % k):
            missing["gadjustor"] += 1
    # --- A4: dynamic symbols
    dyn = []
    for nm, val, sz, info, shndx in nso.dynsyms():
        if shndx and val < nso.text.size and nm:
            put(val, dynsym_class(nm), "dynsym", "A", "dynsym", nm)
            dyn.append((val, dynsym_class(nm)))
    # a dynsym class only forms a band when >=10 same-class exports sit within 32 KB of each other
    dyn.sort()
    band_dyn = set()
    run = []
    for v, c in dyn + [(1 << 60, "")]:
        if run and (c != run[-1][1] or v - run[-1][0] > 32768):
            if len(run) >= 10:
                band_dyn.update(x[0] for x in run)
            run = []
        run.append((v, c))
    for v, c in dyn:
        if v not in band_dyn:
            point_only.add(start_to_k[v])
    # --- A5: import thunks (tiny functions whose only job is a jump through an import slot)
    for k in range(nf):
        if gots[k] and funcs[k][1] - funcs[k][0] <= 32 and label[k] is None:
            sym = gots[k][0]
            label[k] = (import_class(sym), "import_thunk", "A", "import_thunk")
            names[k] = "imp_" + sanitize(sym)
            point_only.add(k)
    # --- B: strong strings
    pri = {c: i for i, (c, _) in enumerate(STR_RULES)}
    for k in range(nf):
        if label[k] is not None or not strs[k]:
            continue
        votes = collections.Counter()
        for a in strs[k]:
            s = S[a]
            for ri, (cls, rx) in enumerate(STR_RULES):
                if ri < WEAK_FROM and rx.search(s):
                    votes[cls] += 1
        if not votes:
            for a in strs[k]:
                if STR_RULES[WEAK_FROM][1].search(S[a]):
                    votes["unity_native"] += 1
        if votes:
            cls = max(votes, key=lambda c: (votes[c], -pri[c]))
            label[k] = (cls, "string", "B", "string:" + cls)
    # --- region rule: FDE-less code regions that are not the PLT are third-party C (curl/zlib/...)
    plt_lo, plt_hi = None, None
    for a, b in F["gaps"]:
        if b - a > 200000:
            for k in range(nf):
                if a <= funcs[k][0] < b and label[k] is None:
                    label[k] = ("thirdparty_c", "fde_less_band", "B", "fde_less_region_%x" % a)
    # --- C/D: neighbour smoothing over band anchors only
    labelled = [k for k in range(nf) if label[k] is not None and k not in point_only]
    lab_pos = labelled
    unl = [k for k in range(nf) if label[k] is None]
    final = list(label)
    res_c = res_d = res_u = 0
    pending = []  # (k, cands) boundary functions to resolve by call graph, then nearest
    cand_of = {}
    for k in unl:
        i = bisect.bisect_left(lab_pos, k)
        prev = lab_pos[i - 1] if i > 0 else None
        nxt = lab_pos[i] if i < len(lab_pos) else None
        cp = label[prev][0] if prev is not None else None
        cn = label[nxt][0] if nxt is not None else None
        if cp is not None and cp == cn:
            sub = label[prev][1] if label[prev][1] == label[nxt][1] else "mixed"
            final[k] = (cp, sub, "C", "between_same_class")
            res_c += 1
        elif cp is None and cn is None:
            final[k] = ("unknown", "", "U", "none")
            res_u += 1
        else:
            cand_of[k] = (prev, nxt)
            pending.append(k)
    # reverse call graph (callers) for voting
    callers = collections.defaultdict(list)
    for k in range(nf):
        for c2 in (calls[k] or ()):
            callers[c2].append(k)
        for c2 in (tails[k] or ()):
            callers[c2].append(k)
    cur = {}  # class decided so far for pending functions
    for it in range(6):
        changed = 0
        for k in pending:
            if k in cur:
                continue
            prev, nxt = cand_of[k]
            cands = {label[x][0] for x in (prev, nxt) if x is not None}
            vote = collections.Counter()
            neigh = list(calls[k] or ()) + list(tails[k] or ()) + callers.get(k, [])
            for c2 in neigh:
                lc = final[c2][0] if final[c2] is not None and final[c2][2] != "D" else cur.get(c2)
                if final[c2] is None:
                    lc = cur.get(c2)
                if lc in cands:
                    vote[lc] += 1
            if len(vote) == 1 or (len(vote) == 2 and vote.most_common(2)[0][1] >= 2 * vote.most_common(2)[1][1]):
                cur[k] = vote.most_common(1)[0][0]
                changed += 1
        if not changed:
            break
    for k in pending:
        prev, nxt = cand_of[k]
        if k in cur:
            use = prev if prev is not None and label[prev][0] == cur[k] else nxt
            final[k] = (cur[k], label[use][1], "D", "boundary_callgraph")
        else:
            dp = k - prev if prev is not None else 1 << 60
            dn = nxt - k if nxt is not None else 1 << 60
            use = prev if dp <= dn else nxt
            final[k] = (label[use][0], label[use][1], "D", "boundary_nearest")
        res_d += 1
    # point-only anchors keep their own exact label
    label = final
    # --- outputs
    os.makedirs(outdir, exist_ok=True)
    rows = []
    sizes = collections.Counter()
    counts = collections.Counter()
    conf_counts = collections.defaultdict(collections.Counter)
    for k, (s, e, hf, lsda) in enumerate(funcs):
        cls, sub, conf, ev = label[k]
        nm = names[k] or ""
        rows.append((hex(s), hex(e), e - s, hf, lsda, cls, sub, conf, ev, nm))
        counts[cls] += 1
        sizes[cls] += e - s
        conf_counts[cls][conf] += 1
    with open(os.path.join(outdir, "functions.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("rva\tend_rva\tsize\thas_fde\thas_lsda\tclass\tsubclass\tconf\tevidence\tname\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    summary = {
        "total_functions": nf, "fde_functions": sum(1 for f in funcs if f[2]), "fde_less_functions": sum(1 for f in funcs if not f[2]),
        "classes": {c: {"functions": counts[c], "bytes": sizes[c], "by_conf": dict(conf_counts[c])} for c in CLASSES if counts[c]},
        "missing_anchor_targets": dict(missing),
        "sub_counts": {},
        "inferred": {"C": res_c, "D": res_d, "U": res_u},
    }
    sub = collections.defaultdict(collections.Counter)
    for k in range(nf):
        sub[label[k][0]][label[k][1]] += 1
    summary["sub_counts"] = {c: dict(v.most_common(80)) for c, v in sub.items()}
    with open(os.path.join(outdir, "origin_summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    # names for Ghidra
    with open(os.path.join(outdir, "ghidra_names.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("rva\tsize\tclass\tname\tcomment\n")
        for k, (s, e, hf, lsda) in enumerate(funcs):
            if names[k]:
                f.write("%#x\t%d\t%s\t%s\t%s\n" % (s, e - s, label[k][0], names[k], comments.get(k, "").replace("\t", " ")))
    # unnamed functions: class only (the Ghidra script names them <class>_<rva>)
    with open(os.path.join(outdir, "ghidra_classes.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("rva\tsize\tclass\tconf\n")
        for k, (s, e, hf, lsda) in enumerate(funcs):
            if not names[k]:
                f.write("%#x\t%d\t%s\t%s\n" % (s, e - s, label[k][0], label[k][2]))
    # import slots (GOT/JUMP_SLOT) for labels
    with open(os.path.join(outdir, "imports.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("slot_rva\tsymbol\n")
        for slot, sym in sorted(F["got_map"].items()):
            f.write("%#x\t%s\n" % (slot, sym))
    # hottest callees by distinct caller count (IL2CPP runtime entry points the recompiler must provide)
    fan = collections.Counter()
    for k in range(nf):
        for c2 in (calls[k] or ()):
            fan[c2] += 1
    with open(os.path.join(outdir, "hot_callees.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("rva\tsize\tclass\tdistinct_callers\tname\tfirst_string\n")
        for c2, n in fan.most_common(300):
            fs = ""
            if strs[c2]:
                fs = S[strs[c2][0]][:60].replace("\t", " ").replace("\n", " ")
            f.write("%#x\t%d\t%s\t%d\t%s\t%s\n" % (funcs[c2][0], funcs[c2][1] - funcs[c2][0], label[c2][0], n, names[c2] or "", fs))
    print("done %.1fs" % (time.time() - t0))
    print(json.dumps({c: v["functions"] for c, v in summary["classes"].items()}, indent=1))
    print(summary["inferred"], summary["missing_anchor_targets"])


if __name__ == "__main__":
    classify()
