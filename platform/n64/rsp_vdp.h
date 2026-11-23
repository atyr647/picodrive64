#ifndef RSP_VDP_H
#define RSP_VDP_H
#include <stdint.h>

/* RSP tile renderer (rsp_vdp.S), fed by pico/draw2.c in emit mode */
void rsp_vdp_init(void);
/* queue a copy of VRAM (64KB, written back here) into the snapshot that
 * renders read; the CPU may change VRAM once rsp_vdp_wait_copy() returned */
void rsp_vdp_snapshot(const void *vram);
/* queue a render of nbands 8-line bands from the per-band command lists
 * (struct rsp_lists in draw2.c, written back to RDRAM by the caller) and
 * the last snapshot; the framebuffer rows it writes must not be dirty in
 * the CPU's D-cache */
void rsp_vdp_render(const void *lists, int nbands, int backfill, void *fb_band0);
/* wait for a queued render to finish (no-op if none is pending) */
void rsp_vdp_wait(void);
extern volatile int rsp_vdp_pending;
/* wait only for the VRAM snapshot the render reads (before VRAM writes) */
void rsp_vdp_wait_copy(void);
extern volatile int rsp_vdp_copy_pending;

#endif
