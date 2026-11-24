import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
d = open(sys.argv[1], 'rb').read(); s = Serialized(d)
for (pid, bo, bs, cid, ti) in s.objects:
    if cid == 47:
        raw = d[s.data_offset + bo: s.data_offset + bo + bs]
        p = 0
        cur, = struct.unpack_from('<i', raw, p); p += 4
        n, = struct.unpack_from('<i', raw, p); p += 4
        print('first', cur, 'count', n)
        for lv in range(n):
            ln, = struct.unpack_from('<i', raw, p); p += 4; nm = raw[p:p+ln].decode(); p += (ln + 3) // 4 * 4
            vals = []
            # print next 40 dwords as hex/float/int
            w = struct.unpack_from('<40I', raw, p)
            print(lv, nm, ' '.join('%d' % x if x < 100000 else '%.2f' % struct.unpack('<f', struct.pack('<I', x)) for x in w))
            # advance until next name (heuristic): search next plausible length+ascii
            q = p + 4
            while q + 8 < len(raw):
                l2, = struct.unpack_from('<i', raw, q)
                if 2 <= l2 <= 12 and raw[q+4:q+4+l2].isalpha(): break
                q += 4
            p = q
