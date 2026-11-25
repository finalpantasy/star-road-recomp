"""Minimal read-only MS-NRBF reader for the game's data_tbl files (research helper for task-24).

Only what the .tbl files use: an array of class records, class members that are primitives, strings,
primitive arrays or object references. Returns plain Python dicts and lists. Not a codec (task-25 owns
the round-trip codec); this never writes. Output of this tool stays on A:, never in Git.
"""
import struct

PRIM = {1: ("?", 1), 2: ("B", 1), 3: ("H", 2), 5: (None, 0), 6: ("d", 8), 7: ("h", 2), 8: ("i", 4),
        9: ("q", 8), 10: ("b", 1), 11: ("f", 4), 13: (None, 8), 14: ("H", 2), 15: ("I", 4), 16: ("Q", 8)}


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]; self.p += 4; return v

    def lps(self):
        n = 0; sh = 0
        while True:
            c = self.u8(); n |= (c & 0x7F) << sh; sh += 7
            if not c & 0x80:
                break
        s = self.b[self.p:self.p + n].decode("utf-8"); self.p += n; return s

    def prim(self, t):
        if t == 18:  # String via primitive type id 18
            return self.lps()
        f, n = PRIM[t]
        v = struct.unpack_from("<" + f, self.b, self.p)[0]; self.p += n; return v


def read_nrbf(data):
    r = R(data)
    objs, classes, refs = {}, {}, []
    root = None

    def read_member(bt, extra):
        if bt == 0:
            return r.prim(extra)
        return read_record()

    def read_class_values(cls):
        return {n: read_member(bt, ex) for n, bt, ex in zip(cls["names"], cls["types"], cls["extra"])}

    def read_record():
        t = r.u8()
        if t == 0:
            r.p += 16; return None
        if t == 12:
            r.i32(); r.lps(); return None
        if t == 11:
            return None
        if t == 10:
            return None
        if t == 13:
            r.u8(); return None
        if t == 14:
            r.i32(); return None
        if t == 9:
            return ("ref", r.i32())
        if t == 6:
            oid = r.i32(); s = r.lps(); objs[oid] = s; return s
        if t == 5:
            oid = r.i32(); name = r.lps(); n = r.i32(); names = [r.lps() for _ in range(n)]
            types = [r.u8() for _ in range(n)]; extra = []
            for bt in types:
                if bt in (0, 7):
                    extra.append(r.u8())
                elif bt == 3:
                    extra.append(r.lps())
                elif bt == 4:
                    extra.append((r.lps(), r.i32()))
                else:
                    extra.append(None)
            r.i32()
            cls = {"name": name, "names": names, "types": types, "extra": extra}
            classes[oid] = cls
            o = read_class_values(cls); o["__class"] = name; objs[oid] = o; return ("obj", oid)
        if t == 1:
            oid = r.i32(); mid = r.i32(); cls = classes[mid]; classes[oid] = cls
            o = read_class_values(cls); o["__class"] = cls["name"]; objs[oid] = o; return ("obj", oid)
        if t == 15:
            oid = r.i32(); n = r.i32(); pt = r.u8()
            objs[oid] = [r.prim(pt) for _ in range(n)]; return ("arr", oid)
        if t == 7:
            oid = r.i32(); at = r.u8(); rank = r.i32(); lens = [r.i32() for _ in range(rank)]
            if at >= 3:
                [r.i32() for _ in range(rank)]
            bt = r.u8(); extra = None
            if bt in (0, 7):
                extra = r.u8()
            elif bt == 3:
                extra = r.lps()
            elif bt == 4:
                extra = (r.lps(), r.i32())
            total = 1
            for x in lens:
                total *= x
            vals = []
            for _ in range(total):
                vals.append(read_member(bt, extra))
            objs[oid] = vals; return ("arr", oid)
        if t in (16, 17):
            oid = r.i32(); n = r.i32(); vals = []
            while len(vals) < n:
                v = read_record(); vals.append(v)
            objs[oid] = vals; return ("arr", oid)
        raise ValueError(f"record {t} at {r.p}")

    first = True
    while r.p < len(data):
        if data[r.p] == 11:
            break
        v = read_record()
        if first and v is not None:
            root = v; first = False
    return objs, root


def resolve(objs, v, depth=0):
    if isinstance(v, tuple):
        k, i = v
        return resolve(objs, objs[i], depth + 1)
    if isinstance(v, list):
        return [resolve(objs, x, depth + 1) for x in v]
    if isinstance(v, dict):
        return {k: resolve(objs, x, depth + 1) for k, x in v.items()}
    return v


def read_table(path):
    objs, root = read_nrbf(open(path, "rb").read())
    return resolve(objs, root)


if __name__ == "__main__":
    import json, sys
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(read_table(sys.argv[1]), ensure_ascii=False, indent=1))
