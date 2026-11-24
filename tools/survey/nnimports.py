"""Group dynsym imports by service. Usage: python -I nnimports.py <survey nso dir> <out tsv>"""
import re, sys, os, collections

def classify(n):
    m = re.search(r'N[KV]*2nn(\d+)', n) if n.startswith('_Z') else None
    if m:
        L = int(m.group(1)); s = n[m.end():m.end()+L]
        return 'nn::' + s
    if n.startswith('nvn'): return 'nvn'
    if re.match(r'_ZN2nv\d', n): return 'nv'
    if re.match(r'nn[A-Z]', n) or n.startswith('nndetail'): return 'nn::core (C)'
    m = re.match(r'_ZN(?:K)?2nn', n)
    if n.startswith('nn') and not n.startswith('nninit'):
        m = re.match(r'nn([a-z]+)', n)
        return 'nn::' + (m.group(1) if m else '?') + ' (C)'
    if n.startswith('nninit') or n.startswith('nnMain'): return 'nn::startup'
    if n.startswith('_ZN') or n.startswith('_ZT') or n.startswith('_ZSt') or n.startswith('_Z'):
        m = re.match(r'_ZN[KV]*(\d+)', n)
        if m:
            L = int(m.group(1)); return 'cxx::' + n[m.end():m.end()+L]
        if re.match(r'_ZN(S|NS)', n) or '3__1' in n or n.startswith('_ZNSt3__1'): return 'libc++'
        return 'cxx::other'
    if n.startswith('__'): return 'runtime/abi'
    return 'libc/libm'

d = sys.argv[1]
rows = []
for mod in ('main', 'subsdk0', 'sdk'):
    p = os.path.join(d, mod + '.imports.tsv')
    for ln in open(p).read().splitlines()[1:]:
        i, n = ln.split('\t'); rows.append((mod, classify(n), n))
rows.sort()
with open(sys.argv[2], 'w') as f:
    f.write('module\tgroup\tsymbol\n')
    for r in rows: f.write('\t'.join(r) + '\n')
c = collections.Counter((m, g) for m, g, _ in rows)
for (m, g), k in sorted(c.items()): print(m, g, k)
