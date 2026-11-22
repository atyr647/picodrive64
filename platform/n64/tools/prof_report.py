#!/usr/bin/env python3
"""prof_report.py LOG ELF [TOP]: aggregate [PS] PC-sample buckets from an
N64_PROF_SAMPLE run (see main_n64.c) per function, using the ELF's symbols.

  platform/n64/tools/prof_report.py run.log picodrive 40
"""
import bisect, subprocess, sys

log, elf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
nm = sys.argv[4] if len(sys.argv) > 4 else "/opt/libdragon/bin/mips64-elf-nm"

syms = []
for line in subprocess.run([nm, "-n", "-S", "--defined-only", elf],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 4 and p[2] in "tTwW":
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), 0, p[2]))
syms.sort()
addrs = [s[0] for s in syms]

def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return "?"
    s = syms[i]
    if s[1] and a >= s[0] + s[1]:
        return "?+%s" % s[2]
    return s[2]

total = outside = 0
per = {}
for line in open(log, errors="replace"):
    if line.startswith("[PSBEGIN]"):
        kv = dict(x.split("=") for x in line.split()[1:] if "=" in x)
        total, outside = int(kv["total"]), int(kv["outside"])
    elif line.startswith("[PS] "):
        _, a, c = line.split()
        n = name(int(a, 16))
        per[n] = per.get(n, 0) + int(c)
if not total:
    sys.exit("no [PSBEGIN] in log")
print("samples: %d  (outside .text: %d = %.1f%%)" % (total, outside, 100.0 * outside / total))
cum = 0
for n, c in sorted(per.items(), key=lambda x: -x[1])[:top]:
    cum += c
    print("%6.2f%%  %6.2f%%  %7d  %s" % (100.0 * c / total, 100.0 * cum / total, c, n))
