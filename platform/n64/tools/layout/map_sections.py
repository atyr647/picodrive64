#!/usr/bin/env python3
"""map_sections.py MAP OUT: list .text input sections (addr size name file) from a GNU ld map."""
import sys, re
lines = open(sys.argv[1]).read().splitlines()
out = []
in_text = False
i = 0
pend = None
while i < len(lines):
    l = lines[i]
    if l.startswith('.text '):
        in_text = True; i += 1; continue
    if in_text and re.match(r'^\.\S', l):   # next output section
        break
    if in_text:
        m = re.match(r'^ (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', l)
        if m:
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4)))
        else:
            m = re.match(r'^ (\.text\S*)\s*$', l)
            if m:
                m2 = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', lines[i + 1])
                if m2:
                    out.append((int(m2.group(1), 16), int(m2.group(2), 16), m.group(1), m2.group(3)))
                    i += 1
    i += 1
out = [o for o in out if o[1] > 0 and o[0] >= 0x80000000]
out.sort()
with open(sys.argv[2], 'w') as f:
    for a, sz, n, fl in out:
        f.write('%08x %d %s %s\n' % (a, sz, n, fl))
print(len(out), 'sections; text %#x..%#x' % (out[0][0], out[-1][0] + out[-1][1]))
