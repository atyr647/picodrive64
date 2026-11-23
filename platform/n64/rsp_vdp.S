##############################################################################
# RSP tile renderer (draw2 backend)
#
# The CPU (pico/draw2.c, emit mode) walks the planes, window and sprites
# exactly as the CPU renderer does, band by band, and instead of drawing a
# tile it emits one command word per (tile, 8-line band).  This overlay
# draws those commands into an 8-line band buffer in DMEM and DMAs each
# finished band into the CI8 framebuffer, so the pixel work runs here while
# the CPU goes on emulating.  Per pixel it does what draw2's put8() does:
# nibble 0 is transparent, others become (pal | nibble).
#
# Command stream (RDRAM, read in 256-byte chunks), per band:
#   header: bits 0-15 number of commands, bits 16-21 backfill colour
#   command: bits 0-8   x: framebuffer column of the tile's left pixel
#            bits 9-12  yrel: tile top row relative to band top (-7..7)
#            bits 13-23 tile index (VRAM byte address = index * 32)
#            bits 24-25 palette (pixel = pal << 4 | nibble)
#            bit  26    x flip, bit 27 y flip
#
# cmd_vdp_render: a0 = number of bands, a1 = command stream (RDRAM),
#                 a2 = framebuffer address of band 0, row 0, column 8,
#                 a3 = VRAM base (RDRAM, written back by the CPU)
##############################################################################

#include <rsp_queue.inc>

#define BW      344             /* band buffer row width (tiles may reach x+7 = 343) */
#define FBW     328             /* framebuffer row pitch */

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
tilebuf:    .space 32

    .text

    ##########################################################################
    # Register use:
    #   v0 = return to rspq          s7 = next command chunk (RDRAM)
    #   s6 = framebuffer, band row 0  s5 = VRAM base (RDRAM)
    #   s3/s2 = command chunk read pointer / end (DMEM)
    #   s1 = commands left in band    k1 = bands left
    #   k0 = 0x00FF00FF   fp = 0x0F0F0F0F   v1 = 0x01010101
    #   per command: a0 cmd, a1 dst, a2 pal x4, a3 rows left,
    #                t3 src row, t4 src step, s0/s4/t0/t1/t2/at for DMA
    ##########################################################################

    # next command-stream word -> reg (refills the 256-byte chunk)
    .macro GETWORD reg
        bne     s3, s2, 1f
        nop
        move    s0, s7
        li      s4, %lo(cmdbuf)
        jal     DMAIn
        li      t0, DMA_SIZE(256, 1)
        addiu   s7, s7, 256
        li      s3, %lo(cmdbuf)
        li      s2, %lo(cmdbuf) + 256
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
    move    s7, a1
    move    s6, a2
    move    s5, a3
    li      s3, 0
    li      s2, 0
    li      k0, 0x00FF00FF
    li      fp, 0x0F0F0F0F
    li      v1, 0x01010101

band_loop:
    beqz    k1, render_done
    nop
    GETWORD t9
    andi    s1, t9, 0xFFFF
    srl     t7, t9, 16
    andi    t7, t7, 0x3F
    sll     t8, t7, 8
    or      t7, t7, t8
    sll     t8, t7, 16
    or      t7, t7, t8              # backfill colour x4
    jal     DMAWaitIdle             # previous band's output DMA done?
    nop
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

cmd_loop:
    beqz    s1, band_out
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
    srl     a2, a0, 20
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
    srl     t9, a0, 27
    andi    t9, t9, 1
    bnez    t9, 4f
    li      t4, 4
    b       5f
    addiu   t3, t8, %lo(tilebuf)
4:  li      t4, -4
    li      t3, %lo(tilebuf) + 28
    subu    t3, t3, t8
5:
    srl     t9, a0, 26
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


