"""Export C# struct layouts for the Ghidra import script.

Writes <outdir>/ghidra_structs.tsv with one header line per type and one line per instance field:
  S  type_index  name  size  is_valuetype  is_enum  enum_underlying
  F  type_index  offset  field_name  kind  arg
kind: prim (arg = C type name), ptr (reference or pointer), vt (arg = type_index of a value type
with its own S record), enum (arg = underlying primitive), unk (generic value type, left as a gap).

Layout rules (IL2CPP, verified on UnityEngine.Vector3: instance_size 28, x/y/z at 0x10/0x14/0x18):
reference types carry a 16-byte object header and field offsets include it; value type field
offsets in the metadata also include the 16-byte box header, so the unboxed struct size is
instance_size - 16 and exported value type offsets are metadata offset - 16.
Inherited fields of non-generic base classes are flattened into the derived type.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_tables import Model, OUT_DEFAULT, sanitize  # noqa: E402

PRIM = {1: ("void", 0), 2: ("bool", 1), 3: ("ushort", 2), 4: ("sbyte", 1), 5: ("byte", 1), 6: ("short", 2), 7: ("ushort", 2),
        8: ("int", 4), 9: ("uint", 4), 10: ("long", 8), 11: ("ulong", 8), 12: ("float", 4), 13: ("double", 8),
        24: ("long", 8), 25: ("ulong", 8)}


def main(outdir=OUT_DEFAULT):
    M = Model()
    meta, reg, namer = M.meta, M.reg, M.namer
    n = len(meta.typeDefinitions)
    base = [sanitize(namer.typedef_name(i)) for i in range(n)]
    cnt = {}
    for b in base:
        cnt[b] = cnt.get(b, 0) + 1
    names = [b if cnt[b] == 1 else "%s_%d" % (b, i) for i, b in enumerate(base)]

    def parent_of(ti):
        t = meta.typeDefinitions[ti]
        if t.parentIndex < 0:
            return None
        pt = reg.types[t.parentIndex]
        return pt["data"] if pt["type"] == 0x12 else None  # non-generic class parent only

    def own_fields(ti):
        t = meta.typeDefinitions[ti]
        offs = reg.field_offsets(ti)
        out = []
        if not offs:
            return out
        for k in range(t.fieldCount):
            f = meta.fields[t.fieldStart + k]
            ft = reg.types[f.typeIndex]
            if ft["attrs"] & 0x10 or ft["attrs"] & 0x40:  # static or literal
                continue
            off = offs[k]
            if off < 0:
                continue
            out.append((off, meta.field_name(t.fieldStart + k), f.typeIndex))
        return out

    def classify_field(tidx):
        ft = reg.types[tidx]
        ty = ft["type"]
        if ft["byref"]:
            return "ptr", ""
        if ty in PRIM:
            return "prim", PRIM[ty][0]
        if ty == 0x11:  # VALUETYPE
            td = meta.typeDefinitions[ft["data"]]
            if (td.bitfield >> 1) & 1:  # enum -> underlying
                u = reg.types[td.elementTypeIndex]["type"]
                return "enum", PRIM.get(u, ("int", 4))[0]
            return "vt", str(ft["data"])
        if ty == 0x15 and ft["valuetype"]:
            return "unk", ""
        return "ptr", ""

    rows = []
    structs = 0
    fields = 0
    for ti, t in enumerate(meta.typeDefinitions):
        if t.genericContainerIndex >= 0:
            continue  # open generic definitions have no concrete layout
        sz = reg.typedef_sizes(ti)
        if not sz:
            continue
        is_vt = t.bitfield & 1
        is_enum = (t.bitfield >> 1) & 1
        size = sz[0] - 16 if is_vt else sz[0]
        if size <= 0 and not is_vt:
            continue
        chain = []
        cur = ti
        guard = 0
        while cur is not None and guard < 64:
            chain.append(cur)
            cur = parent_of(cur) if not is_vt else None
            guard += 1
        flds = []
        for c in reversed(chain):
            flds.extend(own_fields(c))
        under = ""
        if is_enum:
            u = reg.types[t.elementTypeIndex]["type"]
            under = PRIM.get(u, ("int", 4))[0]
        rows.append("S\t%d\t%s\t%d\t%d\t%d\t%s" % (ti, names[ti], size, is_vt, is_enum, under))
        structs += 1
        if is_enum:
            continue
        for off, fname, tidx in flds:
            kind, arg = classify_field(tidx)
            # metadata offsets of value type fields include the 16-byte box header (Vector3.x = 0x10)
            rows.append("F\t%d\t%d\t%s\t%s\t%s" % (ti, off - 16 if is_vt else off, sanitize(fname) or "f", kind, arg))
            fields += 1
    with open(os.path.join(outdir, "ghidra_structs.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    # sanity: well-known layouts
    probe = {}
    for ti in range(n):
        if names[ti] in ("UnityEngine.Vector3", "System.String", "UnityEngine.Quaternion"):
            probe[names[ti]] = reg.typedef_sizes(ti)
    print("structs", structs, "fields", fields, "probe sizes", probe)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT)
