##############################################################################
# RSP tile renderer (draw2 backend)
#
# The CPU (pico/draw2.c, emit mode) walks the planes, window and sprites
# once per frame and, instead of drawing a tile, appends one command word
# per (tile, 8-line band) to that band's lists.  This overlay draws those
# commands into an 8-line band buffer in DMEM and DMAs each finished band
# into the CI8 framebuffer, so the pixel work runs here while the CPU goes
# on emulating.  Per pixel it does what draw2's put8() does:
# nibble 0 is transparent, others become (pal | nibble).
#
# Command lists (RDRAM, struct rsp_lists in draw2.c): 64 halfword counts
# (128 bytes, [band*2 + hi]), then per band a "lo" and a "hi" list of
# LIST_BYTES each.  A band draws its lo list, then its hi list.
#   command: bits 0-8   x: framebuffer column of the tile's left pixel
#            bits 9-12  yrel: tile top row relative to band top (-7..7)
#            bits 13-23 tile index (VRAM byte address = index * 32)
#            bit  24    x flip, bit 25 y flip
#            bits 26-27 palette (pixel = pal << 4 | nibble)
#            (bits 13-27 are bits 0-14 of the VDP name / sprite code)
#
# cmd_vdp_render: a0 = number of bands | backfill colour << 16,
#                 a1 = command lists (RDRAM),
#                 a2 = framebuffer address of band 0, row 0, column 8,
#                 a3 = VRAM base (RDRAM, written back by the CPU)
##############################################################################

#include <rsp_queue.inc>

#define BW      344             /* band buffer row width (tiles may reach x+7 = 343) */
#define FBW     328             /* framebuffer row pitch */
#define LIST_BYTES 2048         /* RL_CAP * 4 in draw2.c */

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_vdp_render, 16
        RSPQ_DefineCommand cmd_vdp_copy, 12
    RSPQ_EndOverlayHeader

    RSPQ_BeginSavedState
vdp_dummy:  .word 0
    RSPQ_EndSavedState

    .align 3
rowoff:     .half 0*BW, 1*BW, 2*BW, 3*BW, 4*BW, 5*BW, 6*BW, 7*BW

    .bss
    .align 3
band:       .space 8*BW
    .align 3
cmdbuf:     .space 256
    .align 3
cnts:       .space 128
bfill:      .word 0
lbase:      .word 0
    .align 3
tilebuf:    .space 32

    .text

    ##########################################################################
    # Register use:
    #   v0 = return to rspq          s7 = next command chunk (RDRAM)
    #   s6 = framebuffer, band row 0  s5 = VRAM base (RDRAM)
    #   s3/s2 = command chunk read pointer / end (DMEM)
    #   s1 = commands left in list    k1 = bands left
    #   sp = count of the current list (DMEM, cnts + 2 * (band*2 + hi))
    #   k0 = 0x00FF00FF   fp = 0x0F0F0F0F   v1 = 0x01010101
    #   per command: a0 cmd, a1 dst, a2 pal x4, a3 rows left,
    #                t3 src row, t4 src step, s0/s4/t0/t1/t2/at for DMA
    ##########################################################################

    # next command of the list -> reg; refills cmdbuf with the next
    # min(256, rest of the list) bytes (s1 = commands left, this one included)
    .macro GETWORD reg
        bne     s3, s2, 1f
        sll     t0, s1, 2
        addiu   t0, t0, 7
        srl     t0, t0, 3
        sll     t0, t0, 3               # rest of the list, 8-byte multiple
        slti    t9, t0, 257
        bnez    t9, 2f
        nop
        li      t0, 256
2:      move    s0, s7
        addu    s7, s7, t0
        li      s4, %lo(cmdbuf)
        addiu   s2, t0, %lo(cmdbuf)
        jal     DMAIn
        addiu   t0, t0, -1              # DMA_SIZE(bytes, 1)
        li      s3, %lo(cmdbuf)
1:      lw      \reg, 0(s3)
        addiu   s3, s3, 4
    .endm

    # draw 4 pixels: \h = 16-bit (p0..p3 nibbles), at \off(a1)
    .macro PIX4 h, off
        sll     t7, \h, 8
        or      t7, t7, \h
        and     t7, t7, k0              # p0p1 in 23-16, p2p3 in 7-0
        sll     t8, t7, 4
        or      t7, t7, t8
        and     t7, t7, fp              # bytes p0 p1 p2 p3
        srl     t8, t7, 1
        or      t9, t7, t8
        srl     t8, t7, 2
        or      t9, t9, t8
        srl     t8, t7, 3
        or      t9, t9, t8
        and     t9, t9, v1              # 1 per non-transparent pixel
        beqz    t9, 3f
        sll     t8, t9, 8
        subu    t8, t8, t9              # 0xFF per non-transparent pixel
        addiu   t9, t8, 1
        bnez    t9, 2f                  # not all opaque
        or      t7, t7, a2              # | palette
        b       3f
        sw      t7, \off(a1)
2:      lw      t9, \off(a1)
        xor     t6, t9, t7
        and     t6, t6, t8
        xor     t9, t9, t6
        sw      t9, \off(a1)
3:
    .endm

cmd_vdp_render:
    move    v0, ra
    andi    k1, a0, 0xFFFF
    srl     t7, a0, 16
    andi    t7, t7, 0x3F
    sll     t8, t7, 8
    or      t7, t7, t8
    sll     t8, t7, 16
    or      t7, t7, t8              # backfill colour x4
    sw      t7, %lo(bfill)
    addiu   t8, a1, 128
    sw      t8, %lo(lbase)          # band 0's lo list
    move    s6, a2
    move    s5, a3
    move    s0, a1
    li      s4, %lo(cnts)
    jal     DMAIn
    li      t0, DMA_SIZE(128, 1)
    li      sp, %lo(cnts)
    li      k0, 0x00FF00FF
    li      fp, 0x0F0F0F0F
    li      v1, 0x01010101

band_loop:
    beqz    k1, render_done
    nop
    jal     DMAWaitIdle             # previous band's output DMA done?
    nop
    lw      t7, %lo(bfill)
    li      t3, %lo(band)
    li      t4, %lo(band) + 8*BW
fill_loop:
    sw      t7, 0(t3)
    sw      t7, 4(t3)
    sw      t7, 8(t3)
    sw      t7, 12(t3)
    sw      t7, 16(t3)
    sw      t7, 20(t3)
    sw      t7, 24(t3)
    addiu   t3, t3, 32
    bne     t3, t4, fill_loop
    sw      t7, -4(t3)

list_start:
    lhu     s1, 0(sp)
    lw      t8, %lo(lbase)
    li      t9, %lo(cnts)
    subu    t9, sp, t9
    sll     t9, t9, 10              # (index * 2) * LIST_BYTES / 2
    addu    s7, t8, t9
    li      s3, 0
    li      s2, 0

cmd_loop:
    beqz    s1, list_end
    nop
    GETWORD a0
    addiu   s1, s1, -1

    /* fetch the tile (32 bytes) */
    srl     t3, a0, 13
    andi    t3, t3, 0x7FF
    sll     t3, t3, 5
    addu    s0, s5, t3
    li      s4, %lo(tilebuf)
    jal     DMAIn
    li      t0, DMA_SIZE(32, 1)

    /* palette x4 */
    srl     a2, a0, 22
    andi    a2, a2, 0x30
    sll     t8, a2, 8
    or      a2, a2, t8
    sll     t8, a2, 16
    or      a2, a2, t8

    /* rows: yrel in -7..7; first visible tile row i0 = max(0,-yrel),
     * count = 8 - |yrel|, first band row = max(0, yrel) */
    sll     t5, a0, 19
    sra     t5, t5, 28              # yrel
    bltz    t5, 1f
    li      t6, 0                   # i0
    b       2f
    move    t7, t5                  # band row
1:  subu    t6, zero, t5            # i0 = -yrel
    li      t7, 0
2:  subu    a3, zero, t5
    bltz    t5, 3f
    nop
    move    a3, t5
3:  li      t8, 8
    subu    a3, t8, a3              # rows = 8 - |yrel|
    blez    a3, cmd_loop
    nop
    /* dst = band + rowoff[band row] + x */
    sll     t8, t7, 1
    lhu     t8, %lo(rowoff)(t8)
    andi    a1, a0, 0x1FF
    addu    a1, a1, t8
    addiu   a1, a1, %lo(band)
    /* src: y flip walks the tile bottom-up */
    sll     t8, t6, 2
    srl     t9, a0, 25
    andi    t9, t9, 1
    bnez    t9, 4f
    li      t4, 4
    b       5f
    addiu   t3, t8, %lo(tilebuf)
4:  li      t4, -4
    li      t3, %lo(tilebuf) + 28
    subu    t3, t3, t8
5:
    srl     t9, a0, 24
    andi    t9, t9, 1               # x flip
    move    s0, t9

row_loop:
    lw      t5, 0(t3)
    beqz    t5, row_next
    addu    t3, t3, t4
    beqz    s0, 6f
    nop
    /* x flip: reverse the 8 nibbles */
    srl     t6, t5, 16
    sll     t5, t5, 16
    or      t5, t5, t6
    and     t6, t5, k0
    srl     t5, t5, 8
    and     t5, t5, k0
    sll     t6, t6, 8
    or      t5, t5, t6
    and     t6, t5, fp
    srl     t5, t5, 4
    and     t5, t5, fp
    sll     t6, t6, 4
    or      t5, t5, t6
6:  srl     t6, t5, 16
    PIX4    t6, 0
    andi    t6, t5, 0xFFFF
    PIX4    t6, 4
row_next:
    addiu   a3, a3, -1
    bnez    a3, row_loop
    addiu   a1, a1, BW
    b       cmd_loop
    nop

list_end:
    addiu   sp, sp, 2
    andi    t9, sp, 2               # lo list done -> the band's hi list
    bnez    t9, list_start
    nop

band_out:
    /* DMA the 8 rows' visible 320 bytes (columns 8..327) */
    li      t3, 0
    li      s4, %lo(band) + 8
out_loop:
    addu    s0, s6, t3
    jal     DMAOutAsync
    li      t0, DMA_SIZE(320, 1)
    addiu   t3, t3, FBW
    li      t8, 8*FBW
    bne     t3, t8, out_loop
    addiu   s4, s4, BW
    addiu   s6, s6, 8*FBW
    b       band_loop
    addiu   k1, k1, -1

render_done:
    jal     DMAWaitIdle
    nop
    jr      v0
    nop

    ##########################################################################
    # cmd_vdp_copy: a0 = bytes (multiple of 2048), a1 = source, a2 = dest
    # RDRAM to RDRAM copy through DMEM (the band buffer), e.g. a VRAM
    # snapshot that the render then reads while the CPU goes on writing VRAM
    ##########################################################################
cmd_vdp_copy:
    move    v0, ra
    li      t8, 0xFFFFFF
    and     k1, a0, t8
    move    s7, a1
    move    s6, a2
copy_loop:
    beqz    k1, copy_done
    nop
    move    s0, s7
    li      s4, %lo(band)
    jal     DMAIn
    li      t0, DMA_SIZE(2048, 1)
    move    s0, s6
    li      s4, %lo(band)
    jal     DMAOut
    li      t0, DMA_SIZE(2048, 1)
    addiu   s7, s7, 2048
    addiu   s6, s6, 2048
    b       copy_loop
    addiu   k1, k1, -2048
copy_done:
    jr      v0
    nop


