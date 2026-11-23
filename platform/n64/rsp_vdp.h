#ifndef RSP_VDP_H
#define RSP_VDP_H
#include <stdint.h>

/* RSP tile renderer (rsp_vdp.S), fed by pico/draw2.c in emit mode */
void rsp_vdp_init(void);
/* queue a render of nbands 8-line bands from the command stream; the
 * framebuffer rows it writes must not be dirty in the CPU's D-cache */
void rsp_vdp_render(const uint32_t *stream, int nwords, int nbands,
                    void *fb_band0, const void *vram);
/* wait for a queued render to finish (no-op if none is pending) */
void rsp_vdp_wait(void);
extern volatile int rsp_vdp_pending;
/* wait only for the VRAM snapshot the render reads (before VRAM writes) */
void rsp_vdp_wait_copy(void);
extern volatile int rsp_vdp_copy_pending;

#endif
