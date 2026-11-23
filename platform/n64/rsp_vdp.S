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
    # per value u of a command's yrel field (yrel = u < 8 ? u : u - 16):
    # band row offset (band row * BW), first tile row * 4, rows to draw
ytab:
    .half 0*BW
    .byte 0, 8
    .half 1*BW
    .byte 0, 7
    .half 2*BW
    .byte 0, 6
    .half 3*BW
    .byte 0, 5
    .half 4*BW
    .byte 0, 4
    .half 5*BW
    .byte 0, 3
    .half 6*BW
    .byte 0, 2
    .half 7*BW
    .byte 0, 1
    .half 0             # yrel -8: not used
    .byte 0, 0
    .half 0
    .byte 28, 1
    .half 0
    .byte 24, 2
    .half 0
    .byte 20, 3
    .half 0
    .byte 16, 4
    .half 0
    .byte 12, 5
    .half 0
    .byte 8, 6
    .half 0
    .byte 4, 7

    .bss
    .align 3
band:       .space 8*BW
    .align 3
cmdbuf:     .space 256
    .align 3
cnts:       .space 128
bfill:      .word 0
lbase:      .word 0
tcur:       .word 0             # tile buffer of the next command (tileA/B)
tpre:       .word 0             # its tile DMA was already started
    .align 3
tileA:      .space 32           # two tile buffers: one drawn, one loading
tileB:      .space 32

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
    #                t3 src row, t4 src step, t6 tile buffer, s0 x flip,
    #                at 0x80808080 (zero-byte test), t0-t2 MERGE4 temps;
    #                s0/s4/t0/t1/t2/at are also the DMA routines' registers
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

    # 4 pixels: \x = their nibbles spread to bytes (0..15), drawn at
    # \off(a1): nibble 0 is transparent, others become (pal | nibble)
    .macro MERGE4 x, off
        subu    t0, \x, v1
        and     t0, t0, $1
        bnez    t0, 1f                  # a transparent pixel
        or      t1, \x, a2              # | palette
        b       3f
        sw      t1, \off(a1)            # all four opaque
1:      srl     t0, \x, 1
        or      t2, \x, t0
        srl     t0, \x, 2
        or      t2, t2, t0
        srl     t0, \x, 3
        or      t2, t2, t0
        and     t2, t2, v1              # 1 per opaque pixel
        beqz    t2, 3f                  # all four transparent
        sll     t0, t2, 8
        subu    t0, t0, t2              # 0xFF per opaque pixel
        lw      t2, \off(a1)
        xor     t1, t1, t2
        and     t1, t1, t0
        xor     t2, t2, t1
        sw      t2, \off(a1)
3:
    .endm

    .set noat       # at holds 0x80808080 while drawing (reloaded per command)

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
    li      t8, %lo(tileA)
    sw      t8, %lo(tcur)
    sw      zero, %lo(tpre)
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

    /* this command's tile (32 bytes): DMA it unless it is already on the
     * way, and start the next command's tile if that command is already
     * in cmdbuf, so its DMA runs while this one is drawn */
    lw      t6, %lo(tcur)
    lw      t5, %lo(tpre)
    bnez    t5, 1f
    srl     t3, a0, 13
    andi    t3, t3, 0x7FF
    sll     t3, t3, 5
    addu    s0, s5, t3
    move    s4, t6
    jal     DMAInAsync
    li      t0, DMA_SIZE(32, 1)
1:  move    t5, zero
    beqz    s1, 2f
    nop
    beq     s3, s2, 2f
    nop
    lw      t7, 0(s3)
    srl     t3, t7, 13
    andi    t3, t3, 0x7FF
    sll     t3, t3, 5
    addu    s0, s5, t3
    li      s4, %lo(tileA)
    addiu   s4, s4, %lo(tileB)
    subu    s4, s4, t6              # the other buffer
    jal     DMAInAsync
    li      t0, DMA_SIZE(32, 1)
    li      t5, 1
2:  sw      t5, %lo(tpre)
    /* wait for this tile: with the next one queued behind it, until the
     * DMA queue has room again, else until the DMA engine is idle */
    beqz    t5, 4f
    nop
3:  mfc0    t8, COP0_DMA_FULL
    bnez    t8, 3b
    nop
    li      t8, %lo(tileA)
    addiu   t8, t8, %lo(tileB)
    subu    t8, t8, t6
    b       5f
    sw      t8, %lo(tcur)           # the next command's tile buffer
4:  mfc0    t8, COP0_DMA_BUSY
    bnez    t8, 4b
    nop
5:
    /* palette x4 */
    srl     a2, a0, 22
    andi    a2, a2, 0x30
    sll     t8, a2, 8
    or      a2, a2, t8
    sll     t8, a2, 16
    or      a2, a2, t8

    /* rows to draw, from the yrel field (ytab) */
    srl     t5, a0, 7
    andi    t5, t5, 0x3C
    lbu     a3, %lo(ytab) + 3(t5)   # rows
    beqz    a3, cmd_loop
    lhu     t8, %lo(ytab)(t5)       # band row * BW
    lbu     t9, %lo(ytab) + 2(t5)   # first tile row * 4
    /* dst = band + band row + x */
    andi    a1, a0, 0x1FF
    addu    a1, a1, t8
    addiu   a1, a1, %lo(band)
    /* src: y flip walks the tile bottom-up */
    srl     t8, a0, 25
    andi    t8, t8, 1
    bnez    t8, 6f
    li      t4, 4
    b       7f
    addu    t3, t6, t9
6:  li      t4, -4
    addiu   t3, t6, 28
    subu    t3, t3, t9
7:  srl     s0, a0, 24
    andi    s0, s0, 1               # x flip
    lui     $1, 0x8080
    ori     $1, $1, 0x8080

row_loop:
    lw      t5, 0(t3)
    beqz    t5, row_next            # all 8 transparent
    addu    t3, t3, t4
    beqz    s0, 8f
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
8:  /* spread the nibbles to bytes: t7 = p0..p3, t6 = p4..p7 */
    srl     t6, t5, 16
    sll     t7, t6, 8
    or      t7, t7, t6
    and     t7, t7, k0
    sll     t8, t7, 4
    or      t7, t7, t8
    and     t7, t7, fp
    andi    t6, t5, 0xFFFF
    sll     t8, t6, 8
    or      t6, t6, t8
    and     t6, t6, k0
    sll     t8, t6, 4
    or      t6, t6, t8
    and     t6, t6, fp
    /* all 8 opaque (no zero byte; bytes are < 16, so no false alarm)? */
    subu    t8, t7, v1
    subu    t9, t6, v1
    or      t8, t8, t9
    and     t8, t8, $1
    bnez    t8, 9f
    nop
    or      t7, t7, a2
    or      t6, t6, a2
    sw      t7, 0(a1)
    b       row_next
    sw      t6, 4(a1)
9:  MERGE4  t7, 0
    MERGE4  t6, 4
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

    .set at
