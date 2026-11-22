# Cache-colored code/data layout (`platform/n64/layout.ld`)

The VR4300 has a 16 KB direct-mapped I-cache (32 B lines) and an 8 KB
direct-mapped D-cache (16 B lines, write-back).  Measured in ares, ~45 % of a
gameplay frame was cache-miss stall, mostly conflict misses: hot functions
and hot data that happen to map to the same cache sets evict each other on
every pass (e.g. FAME's `move.w (a0),d0` and `move.w d0,(a0)+` handlers sat
on the same I-cache sets; FAME's dispatch-table line for `swap` shared a
D-cache set with the stack slot the handler call writes).

`layout.ld` is a `--section-ordering-file` that pins every hot `.text` section
to an I-cache position ("color" = start offset mod 16 KB) and every hot data
object to a D-cache position (offset mod 8 KB).  The colors are chosen by
simulated annealing against exact access traces of real gameplay, so they
minimise simulated misses for this emulator running this game.  Result on the
ares gameplay bench: 22.96 -> 17.64 ms/frame, emulation bit-identical.

Each hot entry is preceded by a padding statement that moves the location
counter to its exact color, so a placement does not depend on the sizes of
the sections before it; cold sections fill most of the gaps.  After code
changes the layout degrades gracefully: hot sections whose names still match
stay pinned.  A section whose name changed (LTO clone suffixes such as
`.constprop.0` / `.lto_priv.0` can change when code changes) is no longer
listed and falls back to default placement.  Regenerate after significant
changes to hot code.

## Regenerating

After changing hot code, from the repo root:

```sh
ARES=/path/to/patched/ares platform/n64/tools/layout/regen.sh /tmp/layout-work
```

(~40 min: two LTO builds, four traces, ~20k I-cache and ~3k D-cache
annealing steps.)  It warm-starts from `colors/`, rewrites `layout.ld` and
`colors/`, and leaves the repo built with the new layout.  The manual steps
it automates are below.

Needs the patched ares (`ares-cachetrace.patch`, against the ares tree that
atyr647/Pak `tools/build_ares.sh` builds, ares commit af4cbb0).  It adds emux
`XPROF` codes 6/7 (cache-miss histograms) and 8/9 (I-fetch-run / data-access
trace of a frame window, run in the interpreter so every access goes through
the cache model).  Dev ROMs are made from the normal ELF with
`../make_dev_rom.sh`, so the traced code is exactly the release code.

```sh
make N64_EMBEDDED_ROM=1                       # build; keep picodrive + picodrive.map
python3 map_sections.py picodrive.map base.secs     # .text input sections
python3 map_data.py picodrive.map base.data         # data input sections
mips64-elf-nm -S -n picodrive | awk 'NF==4' > base.nm   # data objects (filter b/B/d/D/r/R)
for f in 1000 1700 2500 3100; do                    # 30-frame windows of the bench run
  ../make_dev_rom.sh picodrive t$f.z64 3 $f 30      # flags 3 = bench input + trace
  ARES=/path/to/patched/ares ./ares_trace.sh t$f.z64 tr$f
done
cat tr*.i > all.i
gcc -O2 -o icolor icolor.c -lm && ./icolor base.secs all.i 30000 1 i.colors
gcc -O2 -o dcolor dcolor.c -lm && ./dcolor base.objs 4000 1 d.colors tr*.d
python3 learn_align.py align.txt base.secs           # (add maps of earlier links)
ALIGN_FILE=align.txt python3 realize2.py --text base.secs i.colors \
    --data base.objs base.data d.colors ../../layout.ld
```

`base.objs` is the `nm` output reduced to `addr size type name` for data
symbols (types b/B/d/D/r/R).  Check a new link with
`./icolor base.secs trN.i 0 0 - new.secs` and
`./dcolor base.objs -1 0 new.objs trN.d` (the real layout's simulated misses),
then benchmark a `make_dev_rom.sh ... 1` ROM.

The heap and the stack are not movable here; the D-cache model treats them as
fixed, so its estimate is optimistic if the heap start moves a lot.
