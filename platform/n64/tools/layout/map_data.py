#!/usr/bin/env python3
"""map_data.py MAP OUT: list data input sections (addr size name outsec) of .data/.rodata/.bss/.sdata/.sbss"""
import sys, re
lines = open(sys.argv[1]).read().splitlines()
out = []; cur = None; i = 0
while i < len(lines):
    l = lines[i]
    m = re.match(r'^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', l)
    if m and not l.startswith(' '):
        cur = m.group(1) if m.group(1) in ('.data', '.rodata', '.bss', '.sdata', '.sbss') else None
        i += 1; continue
    if re.match(r'^\.\S+\s*$', l) and not l.startswith(' '):
        cur = l.strip() if l.strip() in ('.data', '.rodata', '.bss', '.sdata', '.sbss') else None
        i += 1; continue
    if cur:
        m = re.match(r'^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', l)
        if m:
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), cur, m.group(4)))
        else:
            m = re.match(r'^ (\.\S+)\s*$', l)
            if m and i + 1 < len(lines):
                m2 = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', lines[i + 1])
                if m2:
                    out.append((int(m2.group(1), 16), int(m2.group(2), 16), m.group(1), cur, m2.group(3))); i += 1
    i += 1
out = [o for o in out if o[1] > 0 and o[0] >= 0x80000000]
out.sort()
with open(sys.argv[2], 'w') as f:
    for a, sz, n, sec, fl in out:
        f.write('%08x %d %s %s %s\n' % (a, sz, n, sec, fl))
import collections
c = collections.Counter(o[3] for o in out)
print(len(out), 'data sections', dict(c))
