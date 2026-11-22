#!/usr/bin/env bash
# make_dev_rom.sh ELF OUT.z64 FLAGS [TRACE_START] [TRACE_FRAMES] [INPUT] [BENCH_START] [BENCH_END]
#
# Package a PicoDrive64 ELF (the `picodrive` file a build leaves in the repo
# root) as a dev ROM: identical code to the release ROM made from the same
# ELF, plus a "dev.pdcfg" rompak file that main_n64.c reads at boot.
#   FLAGS: 1 = benchmark (scripted input, [BENCH]/[AUDIO]/[ZSTATE] lines)
#          2 = ares I/D-cache access trace of frames [TRACE_START, +TRACE_FRAMES)
#          4 = ares cache-miss histograms over the same window
#   INPUT: benchmark input, 0 = Gley Lancer script (default), 1 = none (e.g.
#          let an attract-mode demo run)
#   BENCH_START/BENCH_END: measured frame window (default 900-3300)
#   (2 and 4 need the patched ares described in tools/README-layout.md and
#    must never run on real hardware: they are reserved COP0 instructions.)
set -eu
ELF="$1"; OUT="$2"; FLAGS="$3"; TS="${4:-1000}"; TF="${5:-30}"
IN="${6:-0}"; BS="${7:-0}"; BE="${8:-0}"
export N64_INST="${N64_INST:-/opt/libdragon}"
T=$(mktemp -d)
cp "$ELF" "$T/picodrive"
"$N64_INST/bin/n64sym" "$T/picodrive" "$T/picodrive.sym"
cp "$T/picodrive" "$T/picodrive.stripped"
"$N64_INST/bin/mips64-elf-strip" -s "$T/picodrive.stripped"
"$N64_INST/bin/n64elfcompress" -o "$T" -c 1 "$T/picodrive.stripped" >/dev/null
python3 -c "import struct,sys; open(sys.argv[1],'wb').write(struct.pack('>8I', *[int(x) for x in sys.argv[2:8]], 0, 0))" \
	"$T/dev.pdcfg" "$FLAGS" "$TS" "$TF" "$IN" "$BS" "$BE"
"$N64_INST/bin/n64tool" --title "PicoDrive64" --toc --output "$OUT" \
	--align 256 "$T/picodrive.stripped" --align 8 "$T/picodrive.sym" --align 8 "$T/dev.pdcfg" >/dev/null
rm -rf "$T"
echo "dev ROM: $OUT (flags=$FLAGS trace=$TS+$TF input=$IN bench=$BS-$BE)"
