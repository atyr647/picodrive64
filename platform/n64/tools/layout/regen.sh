#!/usr/bin/env bash
# regen.sh WORKDIR [I_ITERS] [D_ITERS]
#
# Regenerate platform/n64/layout.ld for the current source tree (run from
# the repo root).  Needs the libdragon toolchain (N64_INST) and the patched
# ares (ARES=path, see README.md).  Steps:
#   1. build; keep ELF + link map of the traced build
#   2. trace four 30-frame gameplay windows from dev ROMs of that ELF
#   3. optimize I-cache colors (icolor) and D-cache colors (dcolor),
#      warm-started from colors/ (the last run's result)
#   4. realize layout.ld, relink, report simulated misses of the real link
# The new colors go to colors/ so the next run starts from them.
set -eu
W="$1"; IIT="${2:-20000}"; DIT="${3:-3000}"
T=platform/n64/tools/layout
export N64_INST="${N64_INST:-/opt/libdragon}"
export PATH="$N64_INST/bin:$PATH"
mkdir -p "$W"
gcc -O2 -o "$W/icolor" $T/icolor.c -lm 2>/dev/null
gcc -O2 -o "$W/dcolor" $T/dcolor.c -lm 2>/dev/null

build() {   # build [make args...] -> picodrive, picodrive.map
	make clean N64_EMBEDDED_ROM=1 >/dev/null 2>&1 || true
	make N64_EMBEDDED_ROM=1 -j"$(nproc)" "$@" > "$W/build.log" 2>&1
}
objs() {    # data objects (addr size type name) of an ELF
	mips64-elf-nm -S -n "$1" | awk 'NF==4 && $3 ~ /^[bBdDrR]$/' |
		python3 -c "import sys
for l in sys.stdin:
    a,s,t,n=l.split(); a=int(a,16); s=int(s,16)
    if a>=0x80000000 and s>0: print('%08x %d %s %s'%(a,s,t,n))"
}

echo "== build (current layout)"
build
cp picodrive "$W/base.elf"; cp picodrive.map "$W/base.map"
python3 $T/map_sections.py "$W/base.map" "$W/base.secs" >/dev/null
python3 $T/map_data.py "$W/base.map" "$W/base.data" >/dev/null
objs "$W/base.elf" > "$W/base.objs"

echo "== trace"
for f in 1000 1700 2500 3100; do
	platform/n64/tools/make_dev_rom.sh "$W/base.elf" "$W/t$f.z64" 3 $f 30 >/dev/null
	$T/ares_trace.sh "$W/t$f.z64" "$W/tr$f" 2400 >/dev/null
	[ -s "$W/tr$f.i" ] || { echo "trace $f failed (patched ares? ARES=$ARES)"; exit 1; }
done
cat "$W"/tr*.i > "$W/all.i"

echo "== optimize"
"$W/icolor" "$W/base.secs" "$W/all.i" "$IIT" 1 "$W/i.colors" $T/colors/i.colors 2> "$W/icolor.log" | tail -1
"$W/dcolor" "$W/base.objs" "$DIT" 1 "$W/d.colors" "$W"/tr*.d 2> "$W/dcolor.log" | tail -1

echo "== realize + relink"
python3 $T/learn_align.py "$W/align.txt" "$W/base.secs" >/dev/null
ALIGN_FILE="$T/colors/align.txt:$W/align.txt" python3 $T/realize2.py --text "$W/base.secs" "$W/i.colors" \
	--data "$W/base.objs" "$W/base.data" "$W/d.colors" "$W/layout.ld"
build N64_ORDER_FILE="$W/layout.ld"
python3 $T/map_sections.py picodrive.map "$W/new.secs" >/dev/null
objs picodrive > "$W/new.objs"
for f in 1000 1700 2500 3100; do
	echo "tr$f: $("$W/icolor" "$W/base.secs" "$W/tr$f.i" 0 0 - "$W/new.secs" 2>/dev/null | tail -1)"
done
"$W/dcolor" "$W/base.objs" -1 0 "$W/new.objs" "$W"/tr*.d 2>/dev/null | tail -1

cp "$W/layout.ld" platform/n64/layout.ld
cp "$W/i.colors" $T/colors/i.colors; cp "$W/d.colors" $T/colors/d.colors
python3 $T/learn_align.py $T/colors/align.txt "$W/base.secs" "$W/new.secs" >/dev/null
echo "== done: platform/n64/layout.ld updated (picodrive is built with it)"
