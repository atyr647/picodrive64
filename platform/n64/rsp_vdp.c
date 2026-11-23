/*
 * RSP tile renderer - CPU side.  See rsp_vdp.S and pico/draw2.c (emit mode).
 */
#include <libdragon.h>
#include "rsp_vdp.h"

DEFINE_RSP_UCODE(rsp_vdp);

static uint32_t vdp_ovl_id;
static rspq_syncpoint_t vdp_sp;
volatile int rsp_vdp_pending;
unsigned int rsp_vdp_stat[4];   /* waits that blocked, ticks blocked, renders */

void rsp_vdp_init(void)
{
	vdp_ovl_id = rspq_overlay_register(&rsp_vdp);
}

/* VRAM snapshot the render reads: the CPU may write VRAM again as soon as
 * the RSP has copied it, while the render itself takes up to two frames */
static uint16_t vram_snap[0x8000] __attribute__((aligned(16)));
static rspq_syncpoint_t copy_sp;
volatile int rsp_vdp_copy_pending;

void rsp_vdp_render(const uint32_t *stream, int nwords, int nbands,
                    void *fb_band0, const void *vram)
{
	/* the RSP reads the stream and VRAM from RDRAM */
	data_cache_hit_writeback(stream, (nwords * 4 + 15) & ~15);
	data_cache_hit_writeback(vram, 0x10000);
	rspq_write(vdp_ovl_id, 1, 0x10000, PhysicalAddr(vram), PhysicalAddr(vram_snap));
	copy_sp = rspq_syncpoint_new();
	rspq_write(vdp_ovl_id, 0, nbands, PhysicalAddr(stream),
	           PhysicalAddr(fb_band0), PhysicalAddr(vram_snap));
	vdp_sp = rspq_syncpoint_new();
	rspq_flush();
	rsp_vdp_copy_pending = 1;
	rsp_vdp_pending = 1;
	rsp_vdp_stat[2]++;
}

/* before the CPU changes VRAM: wait until the RSP has its snapshot */
void rsp_vdp_wait_copy(void)
{
	if (rsp_vdp_copy_pending) {
		if (!rspq_syncpoint_check(copy_sp)) {
			uint32_t t0 = TICKS_READ();
			rspq_syncpoint_wait(copy_sp);
			rsp_vdp_stat[3] += TICKS_READ() - t0;
		}
		rsp_vdp_copy_pending = 0;
	}
}

void rsp_vdp_wait(void)
{
	if (rsp_vdp_pending) {
		if (!rspq_syncpoint_check(vdp_sp)) {
			uint32_t t0 = TICKS_READ();
			rspq_syncpoint_wait(vdp_sp);
			rsp_vdp_stat[0]++;
			rsp_vdp_stat[1] += TICKS_READ() - t0;
		}
		rsp_vdp_pending = 0;
	}
}
