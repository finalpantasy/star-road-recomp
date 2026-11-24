"""Pass 1: class histogram over all bundles (metadata only). Output TSV."""
import sys, os, glob, collections, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unityfs import *
root, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(root + '/**/*.bundle', recursive=True))
tot = collections.Counter(); rows = []; comp = collections.Counter(); t0 = time.time()
plat = collections.Counter(); ver = collections.Counter(); tree = collections.Counter(); bad = 0
for f in fs:
    try:
        b = Bundle(f)
        comp[(b.comp_type, b.version)] += 1
        nm, o, sz, fl = b.nodes[0]
        h = b.read(o, 64)
        ms = struct.unpack_from('>I', h, 0)[0] or struct.unpack_from('>I', h, 0x14)[0]
        d = b.read(o, min(sz, ms + 0x40))
        s = Serialized(d)
        plat[s.platform] += 1; ver[s.version] += 1; tree[s.has_tree] += 1
        c = collections.Counter(CLASS.get(x[3], x[3]) for x in s.objects)
        tot.update(c)
        rows.append((os.path.relpath(f, root).replace(os.sep, '/'), os.path.getsize(f), len(b.nodes), dict(c)))
    except Exception as ex:
        bad += 1; rows.append((os.path.relpath(f, root).replace(os.sep, '/'), 0, 0, {'ERR': str(ex)[:60]}))
with open(out, 'w') as w:
    w.write('bundle\tbytes\tnodes\tclasses\n')
    for r in rows: w.write('%s\t%d\t%d\t%s\n' % (r[0], r[1], r[2], ';'.join('%s=%d' % kv for kv in sorted(r[3].items(), key=lambda x: str(x[0])))))
print('bundles', len(fs), 'bad', bad, 'secs', round(time.time() - t0, 1))
print('comp(type,ver)', dict(comp)); print('platform', dict(plat)); print('serialized ver', dict(ver)); print('typetree', dict(tree))
for k, v in tot.most_common(40): print(' ', k, v)
