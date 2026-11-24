import sys, os, struct, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from utf import read_utf
d = open(sys.argv[1], 'rb').read(3_000_000)
p = 0; seen = {}
while p + 32 < len(d):
    sig = d[p:p+4]; sz = struct.unpack_from('>I', d, p+4)[0]
    if sig not in (b'CRID', b'@SFV', b'@SFA', b'@ALP'): break
    hs = struct.unpack_from('>H', d, p+8)[0]; fs = struct.unpack_from('>H', d, p+10)[0]
    typ = d[p+15] & 3; payload = d[p+8+hs:p+8+sz-fs]
    if payload[:4] == b'@UTF':
        try:
            r = read_utf(payload)
            key = (sig, typ, str(sorted(r[0].keys()))[:40])
            if key not in seen:
                seen[key] = 1; print(sig, 'type', typ, {k: v for k, v in r[0].items() if k not in ('minchk',)})
        except Exception as e: print('err', e)
    p += 8 + sz
