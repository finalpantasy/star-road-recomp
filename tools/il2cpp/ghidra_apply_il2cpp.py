# Apply the recovered IL2CPP names, origin classes, import labels and C# struct types in Ghidra.
#
# Ghidra script (Jython 2.7 or PyGhidra: the syntax is Python 2 and 3 neutral). @category SMRPG
#
# Input: the TSV files written by export_tables.py, classify.py and ghidra_export.py, by default
# in A:\AI\Recomps\smrpg-work\analysis\il2cpp (override with the first script argument).
#
# How to use (no auto-analysis is needed or wanted; the names come from metadata, not from analysis):
#   1. Import main_v101.elf (written by nso2elf.py, AArch64, image base 0x7100000000 already baked
#      in) with the ELF loader. Pointers in .data and .rodata are already relocated, so the
#      loader's own relocation pass finds nothing to do. When Ghidra asks to analyze, say No.
#   2. Run this script once (Script Manager, or headless:
#      analyzeHeadless <proj> <name> -process main_v101.elf -noanalysis -postScript ghidra_apply_il2cpp.py).
#   3. Optionally run auto-analysis afterwards, now that every function start is known.
#
# What it does, in order:
#   a. struct types: one Ghidra structure per C# type in category /il2cpp (S and F rows of
#      ghidra_structs.tsv). Reference types start with the 16 byte Il2CppObject header (klass,
#      monitor). Value types are unboxed. Generic value type fields are left as gaps.
#   b. function names: every row of ghidra_names.tsv (generated C# method symbols, dynsyms, named
#      libil2cpp helpers) becomes a function with that name and a plate comment.
#   c. origin classes: every function in ghidra_classes.tsv that has no name gets <class>_<rva>.
#   d. import slots: labels for the GOT slots in imports.tsv.
# The script is idempotent: names are only set when they differ, existing structs are replaced.

import io
import os

from ghidra.program.model.data import (
    BooleanDataType, ByteDataType, CategoryPath, DataTypeConflictHandler,
    DoubleDataType, FloatDataType, IntegerDataType, LongLongDataType, PointerDataType,
    ShortDataType, SignedByteDataType, StructureDataType, UnsignedIntegerDataType,
    UnsignedLongLongDataType, UnsignedShortDataType)
from ghidra.program.model.symbol import SourceType

BASE = 0x7100000000
DEFAULT_DIR = "A:\\AI\\Recomps\\smrpg-work\\analysis\\il2cpp"
CATEGORY = CategoryPath("/il2cpp")

PRIM_TYPES = {
    "bool": BooleanDataType, "byte": ByteDataType, "sbyte": SignedByteDataType,
    "short": ShortDataType, "ushort": UnsignedShortDataType,
    "int": IntegerDataType, "uint": UnsignedIntegerDataType,
    "long": LongLongDataType, "ulong": UnsignedLongLongDataType,
    "float": FloatDataType, "double": DoubleDataType,
}


def prim(name):
    cls = PRIM_TYPES.get(name)
    if cls is None:
        return None
    return cls.dataType


def read_tsv(path):
    rows = []
    with io.open(path, "r", encoding="utf-8") as f:
        first = True
        for line in f:
            if first:
                first = False
                if line.startswith("rva") or line.startswith("slot_rva"):
                    continue
            line = line.rstrip("\n")
            if line:
                rows.append(line.split("\t"))
    return rows


def addr_of(rva):
    return toAddr(BASE + rva)


# ---------------------------------------------------------------- structs
def build_structs(outdir):
    path = os.path.join(outdir, "ghidra_structs.tsv")
    if not os.path.exists(path):
        print("no ghidra_structs.tsv, skipping structs")
        return
    structs = {}   # type index -> dict(name, size, vt, enum, under, fields)
    for r in read_tsv(path):
        if r[0] == "S":
            structs[int(r[1])] = {"name": r[2], "size": int(r[3]), "vt": r[4] == "1", "enum": r[5] == "1",
                                  "under": r[6] if len(r) > 6 else "", "fields": []}
        elif r[0] == "F":
            ti = int(r[1])
            if ti in structs:
                structs[ti]["fields"].append((int(r[2]), r[3], r[4], r[5] if len(r) > 5 else ""))
    dtm = currentProgram.getDataTypeManager()
    made = {}
    building = set()
    ptr = PointerDataType.dataType

    def get(ti):
        if ti in made:
            return made[ti]
        info = structs.get(ti)
        if info is None or ti in building:
            return None
        building.add(ti)
        size = max(info["size"], 1)
        if info["enum"]:
            dt = prim(info["under"]) or IntegerDataType.dataType
            made[ti] = dt
            building.discard(ti)
            return dt
        st = StructureDataType(CATEGORY, info["name"], size)
        if not info["vt"] and size >= 16:
            st.replaceAtOffset(0, ptr, 8, "klass", "Il2CppClass*")
            st.replaceAtOffset(8, ptr, 8, "monitor", "")
        for off, fname, kind, arg in info["fields"]:
            ftype = None
            if kind == "prim":
                ftype = prim(arg)
            elif kind == "enum":
                ftype = prim(arg)
            elif kind == "ptr":
                ftype = ptr
            elif kind == "vt":
                ftype = get(int(arg))
            if ftype is None:
                continue  # generic value type or unresolved: leave the bytes undefined
            flen = ftype.getLength()
            if flen <= 0 or off < 0 or off + flen > size:
                continue
            try:
                st.replaceAtOffset(off, ftype, flen, fname, "")
            except Exception as e:  # overlapping field in a flattened hierarchy
                pass
        made[ti] = st
        building.discard(ti)
        return st

    count = 0
    for ti in sorted(structs):
        dt = get(ti)
        if dt is not None and isinstance(dt, StructureDataType):
            dtm.addDataType(dt, DataTypeConflictHandler.REPLACE_HANDLER)
            count += 1
    print("structs added: %d" % count)


# ---------------------------------------------------------------- functions
def set_function(rva, size, name, comment):
    a = addr_of(rva)
    fn = getFunctionAt(a)
    if fn is None:
        disassemble(a)
        fn = createFunction(a, name)
        if fn is None:
            return False
    elif fn.getName() != name:
        fn.setName(name, SourceType.USER_DEFINED)
    if comment:
        setPlateComment(a, comment)
    return True


def apply_names(outdir):
    path = os.path.join(outdir, "ghidra_names.tsv")
    done = fail = 0
    for r in read_tsv(path):
        rva = int(r[0], 16)
        comment = r[4] if len(r) > 4 else ""
        if set_function(rva, int(r[1]), r[3], comment):
            done += 1
        else:
            fail += 1
        if monitor.isCancelled():
            break
    print("named functions: %d, failed: %d" % (done, fail))


def apply_classes(outdir):
    path = os.path.join(outdir, "ghidra_classes.tsv")
    done = fail = 0
    for r in read_tsv(path):
        rva = int(r[0], 16)
        name = "%s_%x" % (r[2], rva)
        if set_function(rva, int(r[1]), name, ""):
            done += 1
        else:
            fail += 1
        if monitor.isCancelled():
            break
    print("class named functions: %d, failed: %d" % (done, fail))


def apply_imports(outdir):
    path = os.path.join(outdir, "imports.tsv")
    n = 0
    for r in read_tsv(path):
        a = addr_of(int(r[0], 16))
        try:
            createLabel(a, "imp_" + r[1], True)
            n += 1
        except Exception:
            pass
    print("import labels: %d" % n)


def main():
    args = getScriptArgs()
    outdir = args[0] if len(args) > 0 else DEFAULT_DIR
    if not currentProgram.getMemory().contains(addr_of(0x100)):
        printerr("image base is not %#x: import main_v101.elf (base baked in), not a rebased copy" % BASE)
        return
    build_structs(outdir)
    apply_names(outdir)
    apply_classes(outdir)
    apply_imports(outdir)
    print("done")


main()
