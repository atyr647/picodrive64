#!/usr/bin/env bash
# ares_profile.sh ROM [SECONDS] [OUTPREFIX]
#
# Run a PicoDrive64 ROM headless in ares and capture the profiler output
# ([F]/[SEC]/[DRC] lines arrive via IS-Viewer, which ares prints to its
# terminal) plus a final screenshot.  The profiler measures emulated N64 time,
# so numbers are meaningful regardless of host speed, and ares models VR4300
# cache-miss penalties -- it tracked real hardware within a few % here.
#
# Needs: ares with the N64 core (build with atyr647/Pak tools/build_ares.sh,
# installs to /opt/pak-ares), Xvfb, ImageMagick `import`.
#
# Prints per-frame averages for SKIP and REND frames (first 40 skipped as
# warm-up).  Frames drawn depend only on emulated time, so two builds run for
# the same SECONDS are directly comparable.
set -u
ROM="$1"; SECS="${2:-60}"; OUT="${3:-${ROM%.z64}_ares}"
ARES="${ARES:-/opt/pak-ares/bin/ares}"
D=$((90 + RANDOM % 9))
Xvfb :$D -screen 0 1280x960x24 -ac -nolisten tcp >/dev/null 2>&1 &
XPID=$!
sleep 2
DISPLAY=:$D LIBGL_ALWAYS_SOFTWARE=1 "$ARES" --system "Nintendo 64" --no-file-prompt \
	--setting Audio/Mute=true "$ROM" > "$OUT.log" 2>&1 &
APID=$!
sleep "$SECS"
DISPLAY=:$D import -window root "$OUT.png" 2>/dev/null
kill -9 $APID $XPID 2>/dev/null
wait 2>/dev/null

for k in SKIP REND; do
	grep -aE "^\[F\] $k" "$OUT.log" | tail -n +40 |
	sed -E 's/.*ft=([0-9.]+)ms cpu:([0-9]+) vdp:([0-9]+).*snd:([0-9]+).*/\1 \2 \3 \4/' |
	awk -v k=$k '{n++; ft+=$1; c+=$2; v+=$3; s+=$4}
		END{ if(n) printf "%s n=%d ft=%.1fms cpu=%.1f vdp=%.1f snd=%.1f\n",k,n,ft/n,c/n,v/n,s/n;
		     else printf "%s: no frames (did it boot? see %s)\n",k,FILENAME }'
done
grep -aE "^\[SEC\]" "$OUT.log" | tail -1
grep -aE "^\[DRC\]" "$OUT.log" | tail -1
echo "log: $OUT.log  screenshot: $OUT.png"
