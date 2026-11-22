#!/usr/bin/env bash
# ares_trace.sh ROM OUT [TIMEOUT]: run a trace dev ROM (make_dev_rom.sh flags 3) under
# the patched ares (ares-cachetrace.patch; set ARES=path) until [TRACE] done
# -> OUT.i (fetch runs) / OUT.d (data accesses)
ROM="$1"; OUT="$2"; TMO="${3:-3000}"
D=$((50 + RANDOM % 40))
Xvfb :$D -screen 0 1280x960x24 -ac -nolisten tcp >/dev/null 2>&1 & XPID=$!
sleep 2
ARES_TRACE="$OUT" DISPLAY=:$D LIBGL_ALWAYS_SOFTWARE=1 "${ARES:-ares}" --system "Nintendo 64" --no-file-prompt \
   --setting Audio/Mute=true --setting Developer/HomebrewMode=true "$ROM" > "$OUT.log" 2>&1 & APID=$!
t=0
while [ $t -lt $TMO ]; do sleep 5; t=$((t+5)); grep -aq "TRACE\] done" "$OUT.log" && break; done
sleep 2
kill -9 $APID $XPID 2>/dev/null; wait 2>/dev/null
echo "t=$t"; grep -a "TRACE\]" "$OUT.log" | tail -1; ls -la "$OUT.i" "$OUT.d" 2>/dev/null
