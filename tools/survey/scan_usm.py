import sys, os, struct, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from utf import read_utf
out = open(sys.argv[2], 'w'); out.write('file\tbytes\tcodec\twidth\theight\tfps\tframes\talpha_type\tstreams\taudio_info\n')
codecs = collections.Counter()
for f in sorted(glob.glob(sys.argv[1] + '/*.usm')):
    fh = open(f, 'rb'); size = os.path.getsize(f); p = 0; streams = collections.Counter(); v = {}; a = ''
    while p + 32 < size:
        fh.seek(p); h = fh.read(32); sig = h[:4]; sz = struct.unpack_from('>I', h, 4)[0]
        if sig not in (b'CRID', b'@SFV', b'@SFA', b'@ALP', b'@CUE'): break
        typ = h[15] & 3; streams[sig.decode()] += 1
        if typ == 1 and not v.get(sig) :
            hs = struct.unpack_from('>H', h, 8)[0]; fs = struct.unpack_from('>H', h, 10)[0]
            fh.seek(p + 8 + hs); pl = fh.read(sz - hs - fs)
            if pl[:4] == b'@UTF':
                r = read_utf(pl)[0]
                if sig == b'@SFV': v[sig] = r
                elif sig == b'@SFA': v[sig] = r; a = ','.join('%s=%s' % (k, r[k]) for k in ('audio_codec', 'num_channels', 'sampling_rate', 'total_samples') if k in r)
                elif sig == b'@ALP': v[sig] = r
        p += 8 + sz
    r = v.get(b'@SFV', {})
    codecs[(r.get('mpeg_codec'), r.get('width'), r.get('height'), r.get('framerate_n', 0) / max(1, r.get('framerate_d', 1)))] += 1
    out.write('%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' % (os.path.basename(f), size, r.get('mpeg_codec'), r.get('width'), r.get('height'), r.get('framerate_n', 0) / max(1, r.get('framerate_d', 1)), r.get('total_frames'), r.get('alpha_type'), dict(streams), a))
print(codecs)
