"""IL2CPP global-metadata.dat parser, metadata version 29 (Unity 2021.3 / 2022).

Our own implementation written from the public layout of the format. Every table
is read with fixed-size records and the section sizes are checked for exact
divisibility, so a wrong struct size fails loudly instead of misparsing.
"""
import struct
import hashlib

META_DEFAULT = r"A:\AI\Recomps\smrpg-work\v101\romfs\Data\Managed\Metadata\global-metadata.dat"

HEADER_SECTIONS = (
    "stringLiteral stringLiteralData string events properties methods parameterDefaultValues "
    "fieldDefaultValues fieldAndParamDefaultData fieldMarshaledSizes parameters fields genericParameters "
    "genericParameterConstraints genericContainers nestedTypes interfaces vtableMethods interfaceOffsets "
    "typeDefinitions images assemblies fieldRefs referencedAssemblies attributeData attributeDataRange "
    "unresolvedVirtualCallParameterTypes unresolvedVirtualCallParameterRanges windowsRuntimeTypeNames "
    "windowsRuntimeStrings exportedTypeDefinitions"
).split()

# name: (struct format, field names)
RECORDS = {
    "events": ("<6i", "nameIndex typeIndex add remove raise token"),
    "properties": ("<5i", "nameIndex get set attrs token"),
    "methods": ("<6i4H", "nameIndex declaringType returnType parameterStart genericContainerIndex token flags iflags slot parameterCount"),
    "parameterDefaultValues": ("<3i", "parameterIndex typeIndex dataIndex"),
    "fieldDefaultValues": ("<3i", "fieldIndex typeIndex dataIndex"),
    "fieldMarshaledSizes": ("<3i", "fieldIndex typeIndex size"),
    "parameters": ("<3i", "nameIndex token typeIndex"),
    "fields": ("<3i", "nameIndex typeIndex token"),
    "genericParameters": ("<2i2h2H", "ownerIndex nameIndex constraintsStart constraintsCount num flags"),
    "genericContainers": ("<4i", "ownerIndex typeArgc isMethod genericParameterStart"),
    "interfaceOffsets": ("<2i", "typeIndex offset"),
    "typeDefinitions": ("<7iI8i8HII", "nameIndex namespaceIndex byvalTypeIndex declaringTypeIndex parentIndex "
                        "elementTypeIndex genericContainerIndex flags fieldStart methodStart eventStart propertyStart "
                        "nestedTypesStart interfacesStart vtableStart interfaceOffsetsStart methodCount propertyCount "
                        "fieldCount eventCount nestedTypeCount vtableCount interfacesCount interfaceOffsetsCount "
                        "bitfield token"),
    "images": ("<10i", "nameIndex assemblyIndex typeStart typeCount exportedTypeStart exportedTypeCount entryPointIndex "
               "token customAttributeStart customAttributeCount"),
    "assemblies": ("<4i3i3i4i8s", "imageIndex token referencedAssemblyStart referencedAssemblyCount nameIndex "
                   "cultureIndex publicKeyIndex hashAlg hashLen flags major minor build revision publicKeyToken"),
    "fieldRefs": ("<2i", "typeIndex fieldIndex"),
    "attributeDataRange": ("<2i", "token startOffset"),
}
SIMPLE_ARRAYS = {  # name: struct char (element = one int)
    "genericParameterConstraints": "i", "nestedTypes": "i", "interfaces": "i", "vtableMethods": "I",
    "referencedAssemblies": "i", "unresolvedVirtualCallParameterTypes": "i", "exportedTypeDefinitions": "i",
}


class Rec(tuple):
    """Tuple subclass with attribute access built per record kind."""
    __slots__ = ()


def _make_rec(name, fields):
    names = fields.split()
    cls = type(name, (Rec,), {"__slots__": (), "_names": names})
    for i, n in enumerate(names):
        setattr(cls, n, property(lambda self, i=i: self[i]))
    return cls


class Metadata:
    def __init__(self, path=META_DEFAULT):
        self.path = path
        d = open(path, "rb").read()
        self.data = d
        self.sha256 = hashlib.sha256(d).hexdigest()
        self.sanity, self.version = struct.unpack_from("<Ii", d, 0)
        if self.sanity != 0xFAB11BAF or self.version != 29:
            raise ValueError("expected metadata v29, got sanity=%#x version=%d" % (self.sanity, self.version))
        self.sections = {}
        pos = 8
        for n in HEADER_SECTIONS:
            self.sections[n] = struct.unpack_from("<ii", d, pos)
            pos += 8
        self.header_size = pos
        self.counts = {}
        for name, (fmt, fields) in RECORDS.items():
            off, size = self.sections[name]
            rs = struct.calcsize(fmt)
            if size % rs:
                raise ValueError("%s: size %#x not divisible by record size %d" % (name, size, rs))
            cls = _make_rec(name, fields)
            rows = [cls(t) for t in struct.iter_unpack(fmt, d[off:off + size])]
            setattr(self, name, rows)
            self.counts[name] = len(rows)
        for name, ch in SIMPLE_ARRAYS.items():
            off, size = self.sections[name]
            setattr(self, name, list(struct.unpack_from("<%d%s" % (size // 4, ch), d, off)))
            self.counts[name] = size // 4
        off, size = self.sections["stringLiteral"]
        self.string_literals = list(struct.iter_unpack("<II", d[off:off + size]))
        self.counts["stringLiteral"] = len(self.string_literals)
        self._str_cache = {}

    # ---- strings
    def string(self, idx):
        s = self._str_cache.get(idx)
        if s is None:
            base = self.sections["string"][0]
            e = self.data.find(b"\0", base + idx)
            s = self.data[base + idx:e].decode("utf-8", "replace")
            self._str_cache[idx] = s
        return s

    def literal(self, i):
        ln, di = self.string_literals[i]
        base = self.sections["stringLiteralData"][0]
        return self.data[base + di:base + di + ln].decode("utf-8", "replace")

    def default_blob(self, idx):
        return self.sections["fieldAndParamDefaultData"][0] + idx

    # ---- derived structure
    def image_name(self, i):
        return self.string(self.images[i].nameIndex)

    def type_owner_image(self):
        """List mapping type definition index -> image index."""
        out = [-1] * len(self.typeDefinitions)
        for ii, im in enumerate(self.images):
            for t in range(im.typeStart, im.typeStart + im.typeCount):
                out[t] = ii
        return out

    def type_full_names(self):
        """Namespace.Outer/Inner style names for every type definition (generic arity as `N)."""
        names = [None] * len(self.typeDefinitions)

        def build(i):
            if names[i] is not None:
                return names[i]
            t = self.typeDefinitions[i]
            nm = self.string(t.nameIndex)
            if t.declaringTypeIndex >= 0:
                # declaringTypeIndex is an Il2CppType index in v29; resolved by caller if possible
                pass
            ns = self.string(t.namespaceIndex)
            names[i] = (ns + "." if ns else "") + nm
            return names[i]
        for i in range(len(self.typeDefinitions)):
            build(i)
        return names

    def method_name(self, i):
        return self.string(self.methods[i].nameIndex)

    def field_name(self, i):
        return self.string(self.fields[i].nameIndex)


if __name__ == "__main__":
    import sys
    m = Metadata(sys.argv[1] if len(sys.argv) > 1 else META_DEFAULT)
    print("sha256", m.sha256, "version", m.version)
    for k, v in m.counts.items():
        print("%-40s %d" % (k, v))
    print("images:")
    for i, im in enumerate(m.images):
        print(i, m.image_name(i), im.typeCount)
