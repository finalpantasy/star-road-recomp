"""Export IL2CPP metadata and registration tables as TSVs.

Usage: python -I tools/il2cpp/export_tables.py [outdir]
Outputs (default A:\\AI\\Recomps\\smrpg-work\\analysis\\il2cpp\\):
  images.tsv types.tsv methods.tsv fields.tsv strings.tsv generic_insts.tsv generic_classes.tsv
  generic_methods.tsv registrations.json
All addresses are module-relative (RVA from NSO image base; Ghidra base = 0x7100000000 + RVA).
Nothing here is committed to Git; the files are derived from the owner's dump.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import NSO  # noqa: E402
from metadata import Metadata  # noqa: E402
from registration import Registrations, TypeNamer  # noqa: E402

OUT_DEFAULT = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp"
_BAD = re.compile(r"[^A-Za-z0-9_.$]")


def esc(s):
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")


def hx(v):
    return "" if not v else "%#x" % v


def sanitize(s):
    return _BAD.sub("_", s)


class Model:
    """Everything needed by exporters and the classifier."""

    def __init__(self):
        self.nso = NSO()
        self.meta = Metadata()
        self.img = self.nso.relocated_image()[0]
        self.reg = Registrations(self.nso, self.meta, self.img)
        self.namer = TypeNamer(self.reg)
        self.build_methods()

    def build_methods(self):
        meta, reg = self.meta, self.reg
        self.type_image = meta.type_owner_image()
        self.method_type = [None] * len(meta.methods)
        for ti, t in enumerate(meta.typeDefinitions):
            for mi in range(t.methodStart, t.methodStart + t.methodCount):
                self.method_type[mi] = ti
        assert all(x is not None for x in self.method_type)
        # pointers
        self.method_ptr = [0] * len(meta.methods)
        self.method_invoker = [-1] * len(meta.methods)
        self.method_sym = [None] * len(meta.methods)
        self.missing_module = set()
        for mi, m in enumerate(meta.methods):
            ii = self.type_image[self.method_type[mi]]
            name = meta.image_name(ii)
            mod = reg.module_by_name.get(name)
            if mod is None:
                self.missing_module.add(name)
                continue
            rid = m.token & 0xFFFFFF
            if rid == 0:
                continue
            k = rid - 1
            assert k < mod["methodPointerCount"], (mi, name, rid)
            self.method_ptr[mi] = mod["pointers"][k]
            self.method_invoker[mi] = mod["invokers"][k] if mod["invokers"] else -1

    def method_full_name(self, mi):
        ti = self.method_type[mi]
        return "%s::%s%s" % (self.namer.typedef_name(ti), self.meta.method_name(mi), self.namer.method_sig(mi))

    def sym_names(self):
        """Unique sanitized symbol per method: Ns.Type$$Method (suffix _<idx> on collisions)."""
        if getattr(self, "_syms", None):
            return self._syms
        base = []
        for mi in range(len(self.meta.methods)):
            ti = self.method_type[mi]
            base.append(sanitize(self.namer.typedef_name(ti)) + "$$" + sanitize(self.meta.method_name(mi)))
        seen = {}
        for b in base:
            seen[b] = seen.get(b, 0) + 1
        self._syms = [b if seen[b] == 1 else "%s_%d" % (b, mi) for mi, b in enumerate(base)]
        return self._syms


def write_tsv(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    return len(rows)


def main(outdir=OUT_DEFAULT):
    os.makedirs(outdir, exist_ok=True)
    t0 = time.time()
    M = Model()
    meta, reg, namer = M.meta, M.reg, M.namer
    counts = {}

    # images
    img_methods = [0] * len(meta.images)
    for mi in range(len(meta.methods)):
        img_methods[M.type_image[M.method_type[mi]]] += 1
    rows = []
    for i, im in enumerate(meta.images):
        name = meta.image_name(i)
        mod = reg.module_by_name.get(name)
        ptrs = [p for p in (mod["pointers"] if mod else []) if p]
        rows.append((i, name, im.assemblyIndex, im.typeStart, im.typeCount, "%#x" % im.token, img_methods[i],
                     mod["methodPointerCount"] if mod else "", len(ptrs),
                     hx(min(ptrs)) if ptrs else "", hx(max(ptrs)) if ptrs else ""))
    counts["images"] = write_tsv(os.path.join(outdir, "images.tsv"),
                                 "image_index name assembly_index type_start type_count token methods module_method_slots "
                                 "methods_with_code min_rva max_rva".split(), rows)

    # types
    rows = []
    for ti, t in enumerate(meta.typeDefinitions):
        sz = reg.typedef_sizes(ti)
        parent = namer.type_name(t.parentIndex) if t.parentIndex >= 0 else ""
        decl = reg.types[t.declaringTypeIndex]["data"] if t.declaringTypeIndex >= 0 else ""
        rows.append((ti, meta.image_name(M.type_image[ti]), esc(namer.typedef_name(ti)), esc(meta.string(t.namespaceIndex)),
                     esc(meta.string(t.nameIndex)), "%#x" % t.token, "%#x" % t.flags, t.bitfield & 1, (t.bitfield >> 1) & 1,
                     esc(parent), decl, t.genericContainerIndex, t.fieldStart, t.fieldCount, t.methodStart, t.methodCount,
                     sz[0] if sz else "", sz[1] if sz else "", sz[2] if sz else "", sz[3] if sz else ""))
    counts["types"] = write_tsv(os.path.join(outdir, "types.tsv"),
                                "type_index image full_name namespace name token flags is_valuetype is_enum parent "
                                "declaring_type generic_container field_start field_count method_start method_count "
                                "instance_size native_size static_size thread_static_size".split(), rows)

    # methods
    syms = M.sym_names()
    rows = []
    for mi, m in enumerate(meta.methods):
        ti = M.method_type[mi]
        ptr = M.method_ptr[mi]
        inv = M.method_invoker[mi]
        inv_rva = reg.invoker_pointers[inv] if 0 <= inv < len(reg.invoker_pointers) else 0
        rows.append((mi, meta.image_name(M.type_image[ti]), ti, esc(namer.typedef_name(ti)), esc(meta.method_name(mi)),
                     esc(namer.method_sig(mi)), esc(namer.type_name(m.returnType)), "%#x" % m.token, "%#x" % m.flags,
                     "%#x" % m.iflags, m.slot if m.slot != 0xFFFF else -1, m.parameterCount,
                     1 if m.genericContainerIndex >= 0 else 0, hx(ptr), inv, hx(inv_rva), syms[mi]))
    counts["methods"] = write_tsv(os.path.join(outdir, "methods.tsv"),
                                  "method_index image type_index type name signature return_type token flags iflags slot "
                                  "param_count is_generic_def rva invoker_index invoker_rva symbol".split(), rows)

    # fields
    rows = []
    for ti, t in enumerate(meta.typeDefinitions):
        offs = reg.field_offsets(ti)
        for k in range(t.fieldCount):
            fi = t.fieldStart + k
            f = meta.fields[fi]
            ft = reg.types[f.typeIndex]
            attrs = ft["attrs"]
            static = 1 if attrs & 0x10 else 0
            literal = 1 if attrs & 0x40 else 0
            rows.append((fi, ti, esc(namer.typedef_name(ti)), esc(meta.field_name(fi)), esc(namer.type_name(f.typeIndex)),
                         offs[k] if offs else "", static, literal, "%#x" % attrs))
    counts["fields"] = write_tsv(os.path.join(outdir, "fields.tsv"),
                                 "field_index type_index type name field_type offset is_static is_literal attrs".split(), rows)

    # strings
    rows = [(i, ln, esc(meta.literal(i))) for i, (ln, di) in enumerate(meta.string_literals)]
    counts["string_literals"] = write_tsv(os.path.join(outdir, "strings.tsv"), "index length value".split(), rows)

    # generics
    rows = [(i, len(a), esc(",".join(namer.type_name(x) for x in a))) for i, a in enumerate(reg.generic_insts)]
    counts["generic_insts"] = write_tsv(os.path.join(outdir, "generic_insts.tsv"), "index argc args".split(), rows)

    rows = []
    for i, (first, ci, mi_) in enumerate(reg.generic_classes):
        tdi = first
        if first in reg.type_addr_to_index:
            tdi = reg.types[reg.type_addr_to_index[first]]["data"]
        cidx = reg.generic_inst_addr_to_index.get(ci, -1)
        rows.append((i, tdi, esc(namer.typedef_name(tdi)), cidx, esc(",".join(namer.type_name(x) for x in reg.generic_insts[cidx])) if cidx >= 0 else ""))
    counts["generic_classes"] = write_tsv(os.path.join(outdir, "generic_classes.tsv"),
                                          "index type_def_index type class_inst class_args".split(), rows)

    # generic method instances (methodSpecs) joined with the generic method table
    gtab = {gi: (mi, inv, adj) for (gi, mi, inv, adj) in reg.generic_method_table}
    rows = []
    for si, (mdi, ci, mi_) in enumerate(reg.method_specs):
        ptr = inv = adj = ""
        entry = gtab.get(si)
        inv_rva = ""
        if entry:
            gmi, ginv, gadj = entry
            ptr = hx(reg.generic_method_pointers[gmi]) if 0 <= gmi < len(reg.generic_method_pointers) else ""
            inv_rva = hx(reg.invoker_pointers[ginv]) if 0 <= ginv < len(reg.invoker_pointers) else ""
            adj = gadj
            inv = ginv
        cargs = ",".join(namer.type_name(x) for x in reg.generic_insts[ci]) if ci >= 0 else ""
        margs = ",".join(namer.type_name(x) for x in reg.generic_insts[mi_]) if mi_ >= 0 else ""
        rows.append((si, mdi, esc(namer.typedef_name(M.method_type[mdi]) + "::" + meta.method_name(mdi)), ci, esc(cargs), mi_,
                     esc(margs), ptr, inv, inv_rva, adj))
    counts["generic_methods"] = write_tsv(os.path.join(outdir, "generic_methods.tsv"),
                                          "spec_index method_def_index method class_inst class_args method_inst method_args "
                                          "rva invoker_index invoker_rva adjustor_index".split(), rows)

    info = {
        "nso_sha256": M.nso.raw_sha256, "metadata_sha256": meta.sha256,
        "code_registration_rva": "%#x" % reg.code_reg_addr, "metadata_registration_rva": "%#x" % reg.meta_reg_addr,
        "code_registration": {k: "%#x" % v for k, v in reg.code_reg.items()},
        "metadata_registration": {k: "%#x" % v for k, v in reg.meta_reg.items()},
        "metadata_counts": meta.counts, "tsv_rows": counts,
        "modules": [{"name": m["name"], "methodPointerCount": m["methodPointerCount"], "adjustorThunkCount": m["adjustorThunkCount"],
                     "addr": "%#x" % m["addr"]} for m in reg.modules],
        "images_without_module": sorted(M.missing_module),
    }
    with open(os.path.join(outdir, "registrations.json"), "w") as f:
        json.dump(info, f, indent=1)
    print("done in %.1fs" % (time.time() - t0), counts)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT)
