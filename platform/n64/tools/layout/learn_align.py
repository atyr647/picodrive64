#!/usr/bin/env python3
"""learn_align.py OUT SECS... : infer input-section alignments from link maps
(map_sections output).  A section that starts after padding must have an
alignment that explains the padding; sections never seen padded keep the
largest power of two that divides every observed start (capped)."""
import sys
al = {}
lo = {}
for fn in sys.argv[2:]:
    rows = []
    for l in open(fn):
        p = l.split()
        n = p[2] if p[2] != '.text' else '.text@' + p[3]
        rows.append((int(p[0], 16), int(p[1]), n))
    rows.sort()
    prev_end = None
    for a, sz, n in rows:
        # max alignment consistent with the start address
        m = a & -a
        al[n] = min(al.get(n, 64), m, 64)
        if prev_end is not None and a > prev_end:
            # padding: alignment must be > (prev_end mod A) i.e. A >= smallest pow2 with ceil(prev_end/A)*A == a
            A = 4
            while A <= 64 and ((prev_end + A - 1) & ~(A - 1)) != a:
                A *= 2
            if A <= 64:
                lo[n] = max(lo.get(n, 4), A)
        prev_end = a + sz
with open(sys.argv[1], 'w') as f:
    for n in sorted(al):
        f.write('%d %d %s\n' % (lo.get(n, 4), al[n], n))
print(len(al), 'sections;', sum(1 for n in lo if lo[n] > 4), 'with known alignment > 4')
