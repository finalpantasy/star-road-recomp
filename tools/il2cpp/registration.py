"""Locate Il2CppCodeRegistration and Il2CppMetadataRegistration in an NSO and
read the tables hanging off them (v29 layouts).

Locator strategy (no symbols exist in the NSO):
  * CodeRegistration: find the module-name string of image 0 (Assembly-CSharp.dll)
    in rodata, find the Il2CppCodeGenModule whose first field points at it, find the
    codeGenModules pointer array that holds that module, then find the field of
    CodeRegistration that points at the array. The u32 right before it is the module
    count (must equal the number of images with code). Field offsets are then
    validated against the layout (counts and pointers must be in range).
  * MetadataRegistration: find two adjacent {count, ptr} pairs whose counts both equal
    the metadata type-definition count (fieldOffsets and typeDefinitionsSizes).
"""
import struct
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CODEREG_FIELDS = [  # (name, kind) in order, every slot is 8 bytes on AArch64
    ("reversePInvokeWrapperCount", "count"), ("reversePInvokeWrappers", "ptr"),
    ("genericMethodPointersCount", "count"), ("genericMethodPointers", "ptr"),
    ("genericAdjustorThunks", "ptr"),
    ("invokerPointersCount", "count"), ("invokerPointers", "ptr"),
    ("unresolvedVirtualCallCount", "count"), ("unresolvedVirtualCallPointers", "ptr"),
    ("interopDataCount", "count"), ("interopData", "ptr"),
    ("windowsRuntimeFactoryCount", "count"), ("windowsRuntimeFactoryTable", "ptr"),
    ("codeGenModulesCount", "count"), ("codeGenModules", "ptr"),
]
METAREG_FIELDS = [
    ("genericClassesCount", "count"), ("genericClasses", "ptr"),
    ("genericInstsCount", "count"), ("genericInsts", "ptr"),
    ("genericMethodTableCount", "count"), ("genericMethodTable", "ptr"),
    ("typesCount", "count"), ("types", "ptr"),
    ("methodSpecsCount", "count"), ("methodSpecs", "ptr"),
    ("fieldOffsetsCount", "count"), ("fieldOffsets", "ptr"),
    ("typeDefinitionsSizesCount", "count"), ("typeDefinitionsSizes", "ptr"),
    ("metadataUsagesCount", "count"), ("metadataUsages", "ptr"),
]
CODEGEN_MODULE_FIELDS = [
    "moduleName", "methodPointerCount", "methodPointers", "adjustorThunkCount", "adjustorThunks",
    "invokerIndices", "reversePInvokeWrapperCount", "reversePInvokeWrapperIndices",
    "rgctxRangesCount", "rgctxRanges", "rgctxsCount", "rgctxs", "debuggerMetadata",
]

ELEMENT = {
    1: "void", 2: "bool", 3: "char", 4: "sbyte", 5: "byte", 6: "short", 7: "ushort", 8: "int", 9: "uint",
    10: "long", 11: "ulong", 12: "float", 13: "double", 14: "string", 22: "TypedReference",
    24: "IntPtr", 25: "UIntPtr", 28: "object",
}


def find_all(buf, pat, start=0, end=None, align=1):
    i = start
    end = len(buf) if end is None else end
    while True:
        i = buf.find(pat, i, end)
        if i < 0:
            return
        if i % align == 0:
            yield i
        i += 1


class Registrations:
    def __init__(self, nso, meta, img=None):
        self.nso = nso
        self.meta = meta
        self.img = img if img is not None else nso.relocated_image()[0]
        self.locate_code_registration()
        self.locate_metadata_registration()
        self.load_tables()

    # ---- raw readers (relocated image)
    def u64(self, a):
        return struct.unpack_from("<Q", self.img, a)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.img, a)[0]

    def i32(self, a):
        return struct.unpack_from("<i", self.img, a)[0]

    def cstr(self, a):
        e = self.img.find(b"\0", a)
        return bytes(self.img[a:e]).decode("utf-8", "replace")

    def ptr_array(self, a, n):
        return list(struct.unpack_from("<%dQ" % n, self.img, a))

    def i32_array(self, a, n):
        return list(struct.unpack_from("<%di" % n, self.img, a))

    # ---- locators
    def locate_code_registration(self):
        nso, meta = self.nso, self.meta
        first = meta.image_name(0).encode() + b"\0"
        ro = nso.rodata
        cands = []
        for s in find_all(self.img, first, ro.mem_off, ro.mem_off + ro.size):
            if self.img[s - 1] != 0:
                continue
            cands.append(s)
        if not cands:
            raise RuntimeError("module name string not found")
        d = nso.data_seg
        found = None
        for s in cands:
            for m in find_all(self.img, struct.pack("<Q", s), d.mem_off, d.mem_off + d.size, 8):
                # candidate Il2CppCodeGenModule at m; find array entries pointing at m
                for arr_slot in find_all(self.img, struct.pack("<Q", m), d.mem_off, d.mem_off + d.size, 8):
                    # slot may be the first element of the modules array (Assembly-CSharp is image 0)
                    for reg_slot in find_all(self.img, struct.pack("<Q", arr_slot), d.mem_off, d.mem_off + d.size, 8):
                        count = self.u64(reg_slot - 8)
                        if 1 <= count <= 128:
                            found = (m, arr_slot, reg_slot, count)
                            break
                    if found:
                        break
                if found:
                    break
            if found:
                break
        if not found:
            raise RuntimeError("CodeRegistration not located")
        m, arr, reg_slot, count = found
        base = reg_slot - 8 * (len(CODEREG_FIELDS) - 1)
        self.code_reg_addr = base
        self.code_reg = {}
        for i, (name, kind) in enumerate(CODEREG_FIELDS):
            self.code_reg[name] = self.u64(base + 8 * i)
        assert self.code_reg["codeGenModulesCount"] == count
        assert self.code_reg["codeGenModules"] == arr
        # sanity: counts below the plausible bound, pointers inside the image
        for name, kind in CODEREG_FIELDS:
            v = self.code_reg[name]
            if kind == "count":
                assert v < 1 << 24, (name, v)
            elif v:
                assert nso.seg_of(v) is not None, (name, hex(v))

    def locate_metadata_registration(self):
        nso, meta = self.nso, self.meta
        n = len(meta.typeDefinitions)
        d = nso.data_seg
        pat = struct.pack("<Q", n)
        hit = None
        for h in find_all(self.img, pat, d.mem_off, d.mem_off + d.size, 8):
            if self.u64(h + 16) == n and nso.seg_of(self.u64(h + 8)) and nso.seg_of(self.u64(h + 24)):
                # layout: fieldOffsetsCount is field index 10
                base = h - 8 * 10
                hit = base
                break
        if hit is None:
            raise RuntimeError("MetadataRegistration not located")
        self.meta_reg_addr = hit
        self.meta_reg = {}
        for i, (name, kind) in enumerate(METAREG_FIELDS):
            self.meta_reg[name] = self.u64(hit + 8 * i)
        mr = self.meta_reg
        assert mr["fieldOffsetsCount"] == n and mr["typeDefinitionsSizesCount"] == n
        assert mr["genericMethodTableCount"] == self.code_reg["genericMethodPointersCount"], "generic table/pointer count mismatch"

    # ---- tables
    def load_tables(self):
        cr, mr, img, meta = self.code_reg, self.meta_reg, self.img, self.meta
        # types: pointers to Il2CppType (16 bytes: data, bits)
        tptrs = self.ptr_array(mr["types"], mr["typesCount"])
        self.types = []
        for p in tptrs:
            data, bits = struct.unpack_from("<QI", img, p)
            self.types.append({
                "data": data, "attrs": bits & 0xFFFF, "type": (bits >> 16) & 0xFF,
                "byref": (bits >> 29) & 1, "pinned": (bits >> 30) & 1,
                "valuetype": (bits >> 31) & 1, "addr": p,
            })
        # generic insts
        self.generic_insts = []
        for p in self.ptr_array(mr["genericInsts"], mr["genericInstsCount"]):
            argc = self.u64(p)
            argv = self.u64(p + 8)
            self.generic_insts.append([self.u64(argv + 8 * i) for i in range(argc)])
        # generic insts hold pointers to Il2CppType -> map pointer to type index
        self.type_addr_to_index = {t["addr"]: i for i, t in enumerate(self.types)}
        self.generic_insts = [[self.type_addr_to_index[p] for p in inst] for inst in self.generic_insts]
        # generic classes: {typeDefIndex(8), class_inst, method_inst, cached_class}
        self.generic_classes = []
        for p in self.ptr_array(mr["genericClasses"], mr["genericClassesCount"]):
            # the first 8 bytes are a type pointer in v29 (Il2CppType*) or an index; handle both
            first = self.u64(p)
            ci = self.u64(p + 8)
            mi = self.u64(p + 16)
            self.generic_classes.append((first, ci, mi))
        self.generic_inst_addr_to_index = {}
        ptrs = self.ptr_array(mr["genericInsts"], mr["genericInstsCount"])
        for i, p in enumerate(ptrs):
            self.generic_inst_addr_to_index[p] = i
        # method specs {methodDefinitionIndex, classIndexIndex, methodIndexIndex}
        self.method_specs = [struct.unpack_from("<3i", img, mr["methodSpecs"] + 12 * i)
                             for i in range(mr["methodSpecsCount"])]
        # generic method table {genericMethodIndex, methodIndex, invokerIndex, adjustorThunkIndex}
        self.generic_method_table = [struct.unpack_from("<4i", img, mr["genericMethodTable"] + 16 * i)
                                     for i in range(mr["genericMethodTableCount"])]
        self.generic_method_pointers = self.ptr_array(cr["genericMethodPointers"], cr["genericMethodPointersCount"])
        self.generic_adjustor_thunks = None  # count unknown; read lazily by index
        self.invoker_pointers = self.ptr_array(cr["invokerPointers"], cr["invokerPointersCount"])
        self.reverse_pinvoke = self.ptr_array(cr["reversePInvokeWrappers"], cr["reversePInvokeWrapperCount"])
        self.unresolved_virtual = self.ptr_array(cr["unresolvedVirtualCallPointers"], cr["unresolvedVirtualCallCount"])
        # interop data: 7 pointers each
        self.interop = [struct.unpack_from("<7Q", img, cr["interopData"] + 56 * i) for i in range(cr["interopDataCount"])]
        # codegen modules
        self.modules = []
        mods = self.ptr_array(cr["codeGenModules"], cr["codeGenModulesCount"])
        for p in mods:
            f = struct.unpack_from("<%dQ" % len(CODEGEN_MODULE_FIELDS), img, p)
            rec = dict(zip(CODEGEN_MODULE_FIELDS, f))
            rec["addr"] = p
            rec["name"] = self.cstr(rec["moduleName"])
            for k in ("methodPointerCount", "adjustorThunkCount", "reversePInvokeWrapperCount", "rgctxRangesCount", "rgctxsCount"):
                rec[k] &= 0xFFFFFFFF
            n = rec["methodPointerCount"]
            rec["pointers"] = self.ptr_array(rec["methodPointers"], n)
            rec["invokers"] = self.i32_array(rec["invokerIndices"], n) if rec["invokerIndices"] else []
            self.modules.append(rec)
        self.module_by_name = {m["name"]: m for m in self.modules}
        # field offsets (per type definition; pointer to int32[] or null)
        self.field_offset_ptrs = self.ptr_array(mr["fieldOffsets"], mr["fieldOffsetsCount"])
        # type definition sizes: 4 x u32 each, array of pointers
        self.typedef_size_ptrs = self.ptr_array(mr["typeDefinitionsSizes"], mr["typeDefinitionsSizesCount"])

    # ---- queries
    def typedef_sizes(self, ti):
        p = self.typedef_size_ptrs[ti]
        if not p:
            return None
        return struct.unpack_from("<4I", self.img, p)  # instance, native, static, thread-static

    def field_offsets(self, ti):
        t = self.meta.typeDefinitions[ti]
        p = self.field_offset_ptrs[ti]
        if not p or t.fieldCount == 0:
            return None
        return self.i32_array(p, t.fieldCount)


class TypeNamer:
    """Readable names for Il2CppType indexes and type definitions."""

    def __init__(self, reg):
        self.reg = reg
        self.meta = reg.meta
        self._typedef_cache = {}
        self._type_cache = {}

    def typedef_name(self, ti, with_ns=True):
        key = (ti, with_ns)
        r = self._typedef_cache.get(key)
        if r is not None:
            return r
        meta = self.meta
        t = meta.typeDefinitions[ti]
        nm = meta.string(t.nameIndex)
        if t.declaringTypeIndex >= 0:
            dt = self.reg.types[t.declaringTypeIndex]
            outer = self.typedef_name(dt["data"], with_ns)
            r = outer + "/" + nm
        else:
            ns = meta.string(t.namespaceIndex) if with_ns else ""
            r = (ns + "." if ns else "") + nm
        self._typedef_cache[key] = r
        return r

    def type_name(self, idx):
        r = self._type_cache.get(idx)
        if r is not None:
            return r
        self._type_cache[idx] = "<recursive>"
        r = self._type_name(self.reg.types[idx])
        self._type_cache[idx] = r
        return r

    def _type_name(self, t):
        reg = self.reg
        ty = t["type"]
        if ty in ELEMENT:
            s = ELEMENT[ty]
        elif ty in (0x11, 0x12):  # VALUETYPE, CLASS: data = typedef index
            s = self.typedef_name(t["data"])
        elif ty in (0x13, 0x1E):  # VAR, MVAR: data = generic parameter index
            gp = self.meta.genericParameters[t["data"]]
            s = self.meta.string(gp.nameIndex)
        elif ty == 0x1D:  # SZARRAY
            s = self.type_name(reg.type_addr_to_index[t["data"]]) + "[]"
        elif ty == 0x0F:  # PTR
            s = self.type_name(reg.type_addr_to_index[t["data"]]) + "*"
        elif ty == 0x14:  # ARRAY {etype ptr, rank u8, ...}
            et = reg.u64(t["data"])
            rank = reg.img[t["data"] + 8]
            s = self.type_name(reg.type_addr_to_index[et]) + "[" + "," * (rank - 1) + "]"
        elif ty == 0x15:  # GENERICINST -> Il2CppGenericClass*
            gc = t["data"]
            first = reg.u64(gc)
            ci = reg.u64(gc + 8)
            # first is a type definition index in v29 static data; if a pointer resolve it
            if first in reg.type_addr_to_index:
                base_t = reg.types[reg.type_addr_to_index[first]]
                tdi = base_t["data"]
            else:
                tdi = first
            base = self.typedef_name(tdi)
            args = reg.generic_insts[reg.generic_inst_addr_to_index[ci]]
            tick = base.find("`")
            if tick >= 0:
                end = tick + 1
                while end < len(base) and base[end].isdigit():
                    end += 1
                base = base[:tick] + base[end:]
            s = base + "<" + ",".join(self.type_name(a) for a in args) + ">"
        elif ty == 0x1B:
            s = "fnptr"
        else:
            s = "type%#x" % ty
        if t["byref"]:
            s += "&"
        return s

    def method_sig(self, mi):
        m = self.meta.methods[mi]
        ps = []
        for p in range(m.parameterStart, m.parameterStart + m.parameterCount):
            pr = self.meta.parameters[p]
            ps.append(self.type_name(pr.typeIndex))
        return "(" + ",".join(ps) + ")"
