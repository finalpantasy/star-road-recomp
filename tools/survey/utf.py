"""CRI @UTF table reader (read-only)."""
import struct
TY = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('I', 4), 5: ('i', 4), 6: ('Q', 8), 7: ('q', 8), 8: ('f', 4), 9: ('d', 8)}

def read_utf(b, base=0):
    assert b[base:base+4] == b'@UTF', b[base:base+4]
    size, = struct.unpack_from('>I', b, base+4)
    _, rows_off, str_off, data_off, name_off, nf, rsz, nrows = struct.unpack_from('>HHIIIHHI', b, base+8)
    T = base + 8
    def s(o):
        a = T + str_off + o; e = b.index(b'\0', a); return b[a:e].decode('utf8', 'replace')
    p = base + 0x20; fields = []
    for _ in range(nf):
        fl = b[p]; p += 1
        nm = s(struct.unpack_from('>I', b, p)[0]); p += 4
        st, ty = fl >> 4, fl & 15; const = None
        if st == 3:
            if ty == 0xA: const = s(struct.unpack_from('>I', b, p)[0]); p += 4
            elif ty == 0xB: const = ('data',) + struct.unpack_from('>II', b, p); p += 8
            else:
                f, n = TY[ty]; const = struct.unpack_from('>' + f, b, p)[0]; p += n
        fields.append((nm, st, ty, const))
    rows = []
    for r in range(nrows):
        q = T + rows_off + r * rsz; row = {}
        for nm, st, ty, const in fields:
            if st == 3: row[nm] = const; continue
            if st == 1 or st == 5 or True:
                if ty == 0xA: row[nm] = s(struct.unpack_from('>I', b, q)[0]); q += 4
                elif ty == 0xB: row[nm] = ('data',) + struct.unpack_from('>II', b, q); q += 8
                else:
                    f, n = TY[ty]; row[nm] = struct.unpack_from('>' + f, b, q)[0]; q += n
        rows.append(row)
    return rows
