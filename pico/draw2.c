/*
 * tile renderer
 * (C) notaz, 2006-2008
 * (C) irixxxx, 2020-2023
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "pico_int.h"

#if defined(N64) && (defined(N64_PERF_HUD) || defined(N64_BENCH_TIMERS))
#include <libdragon.h>
extern unsigned int prof_vdp_sprite_ticks;
extern unsigned int prof_vdp_layer_ticks;
#define PROF2_START() unsigned int _pd = timer_ticks()
#define PROF2_SPR()   do { prof_vdp_sprite_ticks += timer_ticks() - _pd; _pd = timer_ticks(); } while(0)
#define PROF2_LAY()   do { prof_vdp_layer_ticks  += timer_ticks() - _pd; _pd = timer_ticks(); } while(0)
#else
#define PROF2_START()
#define PROF2_SPR()
#define PROF2_LAY()
#endif

#define START_ROW  0 // which row of tiles to start rendering at?
#define END_ROW   28 // ..end

#define VSRAM      0 // 2-cell vscroll (broken for line based hscroll)
#define INTERLACE  0 // interlace mode 2

#define TILE_ROWS END_ROW-START_ROW

// note: this is not implemented in ARM asm
#if defined(DRAW2_OVERRIDE_LINE_WIDTH)
#define LINE_WIDTH DRAW2_OVERRIDE_LINE_WIDTH
#else
#define LINE_WIDTH 328
#endif

static unsigned char PicoDraw2FB_[LINE_WIDTH * (8+240+8) + 8];

static u32 HighCache2A[2*41*(TILE_ROWS+1)+1+1]; // caches for high layers
static u32 HighCache2B[2*41*(TILE_ROWS+1)+1+1];

unsigned short *PicoCramHigh=PicoMem.cram; // pointer to CRAM buff (0x40 shorts), converted to native device color (works only with 16bit for now)
void (*PicoPrepareCram)(void) = NULL;      // prepares PicoCramHigh for renderer to use


// stuff available in asm:
#ifdef _ASM_DRAW_C
void BackFillFull(void *dst, int reg7, int lwidth);
void DrawLayerFull(int plane, u32 *hcache, int planestart, int planeend,
                   struct PicoEState *est);
void DrawTilesFromCacheF(u32 *hc, struct PicoEState *est);
void DrawWindowFull(int start, int end, int prio, struct PicoEState *est);
void DrawSpriteFull(unsigned int *sprite, struct PicoEState *est);
#else


#if defined(N64) && !INTERLACE
/* Band rendering (N64): instead of sweeping each layer over the whole 84KB
 * framebuffer (~6 passes through an 8KB D-cache, every line missed and
 * written back each pass), run the complete layer sequence on one band of
 * DRAW2_BAND lines at a time so the band stays cached.  Every draw is
 * clipped to [clip_lo, clip_hi) (row pointers); per pixel, the writes and
 * their order are exactly those of the full-frame passes.  0 disables. */
#ifndef DRAW2_BAND
#define DRAW2_BAND 16
#endif
#define BANDED (DRAW2_BAND > 0)
static unsigned char *clip_lo = (unsigned char *)0;
static unsigned char *clip_hi = (unsigned char *)~(uintptr_t)0;
/* the same band as framebuffer row indices, for the tile row clip */
static int band_r0 = 0, band_r1 = 1 << 30;
#ifdef N64_RSP_VDP
/* RSP mode: the pixels are drawn by the RSP (platform/n64/rsp_vdp.S); the
 * CPU only turns the layers into per-band command lists (EmitDisplayRSP) */
#include "../platform/n64/rsp_vdp.h"
#include <n64sys.h>   /* data_cache_hit_writeback_invalidate */
int draw2_rsp;                         /* set by the platform: use the RSP */
unsigned int draw2_rsp_ovf;            /* frames drawn on the CPU: lists full */
unsigned int draw2_rsp_maxlist;        /* longest list so far */
/* the CPU wrote the framebuffer since it was last written back (the
 * platform writes it back before displaying it, and clears this) */
int draw2_fb_dirty = 1;
static int rsp_took;                   /* this frame is the RSP's */
#endif
/* does the 8-line tile row starting at p intersect the clip band? */
#define ROW_VIS(p, stride) ((p) + 8*(stride) > clip_lo && (p) < clip_hi)
#else
#define BANDED 0
#define ROW_VIS(p, stride) 1
#endif
#if defined(N64) && !INTERLACE
#define TILE_Y(y) , (y)   /* tile functions also get the tile row's top framebuffer row */
#else
#define TILE_Y(y)
#endif

#if defined(N64) && !INTERLACE
/* N64 (big-endian MIPS III, 64-bit registers): draw a whole 8-pixel tile row
 * with 64-bit SWAR instead of 8 test-and-store byte writes.  A VRAM tile row
 * is one 32-bit word, pixels p0..p7 from the top nibble down.  spread8()
 * turns it into 8 bytes p0..p7; the transparency mask (0xFF where p != 0)
 * selects which destination bytes change, via one unaligned 64-bit
 * read-modify-write (ldl/ldr, sdl/sdr), or a plain store if the row is
 * fully opaque.  Produces exactly the same bytes as the generic version. */
typedef unsigned long long u64_t;

static inline u64_t spread8(u32 w)
{
	u64_t x = w;
	x = ((x << 16) | x) & 0x0000FFFF0000FFFFULL;
	x = ((x <<  8) | x) & 0x00FF00FF00FF00FFULL;
	x = ((x <<  4) | x) & 0x0F0F0F0F0F0F0F0FULL;
	return x;
}

static inline u32 nibrev32(u32 w)  /* p0..p7 -> p7..p0 */
{
	w = (w >> 16) | (w << 16);
	w = ((w >> 8) & 0x00FF00FF) | ((w << 8) & 0xFF00FF00);
	w = ((w >> 4) & 0x0F0F0F0F) | ((w << 4) & 0xF0F0F0F0);
	return w;
}

static inline void put8(unsigned char *pd, u64_t x, u64_t pal8)
{
	u64_t nz = (x | (x >> 1) | (x >> 2) | (x >> 3)) & 0x0101010101010101ULL;
	u64_t m = (nz << 8) - nz;   /* 0xFF per non-transparent pixel */
	u64_t src = x | pal8, d;
	if (m == ~0ULL) {
		__builtin_memcpy(pd, &src, 8);
		return;
	}
	__builtin_memcpy(&d, pd, 8);
	d = (d & ~m) | (src & m);
	__builtin_memcpy(pd, &d, 8);
}

/* flip: 0 normal, 1 X flip; ystep: +2 normal, -2 Y flip (addr preset).
 * y: framebuffer row of the tile's top row.  Only rows inside the band
 * [band_r0, band_r1) are visited: the range is found once per tile rather
 * than testing every row.  A tile is reported blank (callers cache that per
 * tile code) only when all 8 rows were checked; a partly clipped tile is
 * never reported blank, which only costs a redraw of nothing. */
static inline int TileRowsN64(unsigned char *pd, int addr, unsigned char pal,
			      struct PicoEState *est, int xflip, int ystep, int y)
{
	const unsigned short *vram = est->PicoMem_vram;
	u64_t pal8 = (u64_t)pal * 0x0101010101010101ULL;
	int i, stride = est->Draw2Width, blank = 1;
	int i0 = band_r0 - y, i1 = band_r1 - y;

	if (i0 > 0) {
		pd += i0 * stride;
		addr += i0 * ystep;
		blank = 0;
	} else
		i0 = 0;
	if (i1 < 8)
		blank = 0;
	else
		i1 = 8;
	for (i = i1 - i0; i > 0; i--, addr += ystep, pd += stride) {
		u32 w = *(const u32 *)(vram + addr);
		if (!w) continue;
		blank = 0;
		if (xflip) w = nibrev32(w);
		put8(pd, spread8(w), pal8);
	}
	return blank;
}

static int TileXnormYnorm(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est, int y)
{ return TileRowsN64(pd, addr, pal, est, 0, 2, y); }
static int TileXflipYnorm(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est, int y)
{ return TileRowsN64(pd, addr, pal, est, 1, 2, y); }
static int TileXnormYflip(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est, int y)
{ return TileRowsN64(pd, addr + 14, pal, est, 0, -2, y); }
static int TileXflipYflip(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est, int y)
{ return TileRowsN64(pd, addr + 14, pal, est, 1, -2, y); }

#else
static int TileXnormYnorm(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est)
{
	unsigned int pack=0; unsigned int t=0, blank = 1;
	unsigned short *vram = est->PicoMem_vram;
	int i, inc=2;

#if INTERLACE
	if (est->rendstatus & PDRAW_INTERLACE) inc = 4;
#endif
	for(i=8; i; i--, addr+=inc, pd += est->Draw2Width) {
		pack=CPU_LE2(*(u32 *)(vram+addr)); // Get 8 pixels (BE-portable)
		if(!pack) continue;

		t=pack&0x0000f000; if (t) pd[0]=(unsigned char)((t>>12)|pal);
		t=pack&0x00000f00; if (t) pd[1]=(unsigned char)((t>> 8)|pal);
		t=pack&0x000000f0; if (t) pd[2]=(unsigned char)((t>> 4)|pal);
		t=pack&0x0000000f; if (t) pd[3]=(unsigned char)((t    )|pal);
		t=pack&0xf0000000; if (t) pd[4]=(unsigned char)((t>>28)|pal);
		t=pack&0x0f000000; if (t) pd[5]=(unsigned char)((t>>24)|pal);
		t=pack&0x00f00000; if (t) pd[6]=(unsigned char)((t>>20)|pal);
		t=pack&0x000f0000; if (t) pd[7]=(unsigned char)((t>>16)|pal);
		blank = 0;
	}

	return blank; // Tile blank?
}

static int TileXflipYnorm(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est)
{
	unsigned int pack=0; unsigned int t=0, blank = 1;
	unsigned short *vram = est->PicoMem_vram;
	int i, inc=2;

#if INTERLACE
	if (est->rendstatus & PDRAW_INTERLACE) inc = 4;
#endif
	for(i=8; i; i--, addr+=inc, pd += est->Draw2Width) {
		pack=CPU_LE2(*(u32 *)(vram+addr)); // Get 8 pixels (BE-portable)
		if(!pack) continue;

		t=pack&0x000f0000; if (t) pd[0]=(unsigned char)((t>>16)|pal);
		t=pack&0x00f00000; if (t) pd[1]=(unsigned char)((t>>20)|pal);
		t=pack&0x0f000000; if (t) pd[2]=(unsigned char)((t>>24)|pal);
		t=pack&0xf0000000; if (t) pd[3]=(unsigned char)((t>>28)|pal);
		t=pack&0x0000000f; if (t) pd[4]=(unsigned char)((t    )|pal);
		t=pack&0x000000f0; if (t) pd[5]=(unsigned char)((t>> 4)|pal);
		t=pack&0x00000f00; if (t) pd[6]=(unsigned char)((t>> 8)|pal);
		t=pack&0x0000f000; if (t) pd[7]=(unsigned char)((t>>12)|pal);
		blank = 0;
	}
	return blank; // Tile blank?
}

static int TileXnormYflip(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est)
{
	unsigned int pack=0; unsigned int t=0, blank = 1;
	unsigned short *vram = est->PicoMem_vram;
	int i, inc=2;

#if INTERLACE
	if (est->rendstatus & PDRAW_INTERLACE) inc = 4, addr += 16;
#endif
	addr+=14;
	for(i=8; i; i--, addr-=inc, pd += est->Draw2Width) {
		pack=CPU_LE2(*(u32 *)(vram+addr)); // Get 8 pixels (BE-portable)
		if(!pack) continue;

		t=pack&0x0000f000; if (t) pd[0]=(unsigned char)((t>>12)|pal);
		t=pack&0x00000f00; if (t) pd[1]=(unsigned char)((t>> 8)|pal);
		t=pack&0x000000f0; if (t) pd[2]=(unsigned char)((t>> 4)|pal);
		t=pack&0x0000000f; if (t) pd[3]=(unsigned char)((t    )|pal);
		t=pack&0xf0000000; if (t) pd[4]=(unsigned char)((t>>28)|pal);
		t=pack&0x0f000000; if (t) pd[5]=(unsigned char)((t>>24)|pal);
		t=pack&0x00f00000; if (t) pd[6]=(unsigned char)((t>>20)|pal);
		t=pack&0x000f0000; if (t) pd[7]=(unsigned char)((t>>16)|pal);
		blank = 0;
	}

	return blank; // Tile blank?
}

static int TileXflipYflip(unsigned char *pd,int addr,unsigned char pal, struct PicoEState *est)
{
	unsigned int pack=0; unsigned int t=0, blank = 1;
	unsigned short *vram = est->PicoMem_vram;
	int i, inc=2;

#if INTERLACE
	if (est->rendstatus & PDRAW_INTERLACE) inc = 4, addr += 16;
#endif
	addr+=14;
	for(i=8; i; i--, addr-=inc, pd += est->Draw2Width) {
		pack=CPU_LE2(*(u32 *)(vram+addr)); // Get 8 pixels (BE-portable)
		if(!pack) continue;

		t=pack&0x000f0000; if (t) pd[0]=(unsigned char)((t>>16)|pal);
		t=pack&0x00f00000; if (t) pd[1]=(unsigned char)((t>>20)|pal);
		t=pack&0x0f000000; if (t) pd[2]=(unsigned char)((t>>24)|pal);
		t=pack&0xf0000000; if (t) pd[3]=(unsigned char)((t>>28)|pal);
		t=pack&0x0000000f; if (t) pd[4]=(unsigned char)((t    )|pal);
		t=pack&0x000000f0; if (t) pd[5]=(unsigned char)((t>> 4)|pal);
		t=pack&0x00000f00; if (t) pd[6]=(unsigned char)((t>> 8)|pal);
		t=pack&0x0000f000; if (t) pd[7]=(unsigned char)((t>>12)|pal);
		blank = 0;
	}
	return blank; // Tile blank?
}
#endif /* N64 SWAR tile rows */


// start: (tile_start<<16)|row_start, end: [same]
static void DrawWindowFull(int start, int end, int prio, struct PicoEState *est)
{
	struct PicoVideo *pvid=&est->Pico->video;
	int nametab, nametab_step, trow, tilex, blank=-1, code;
	unsigned char *scrpos = est->Draw2FB;
	int scrstart = est->Draw2Start;
	int tile_start, tile_end; // in cells

	// parse ranges
	tile_start = start>>16;
	tile_end = end>>16;
	start = start<<16>>16;
	end = end<<16>>16;

	// Find name table line:
	if (!(est->rendstatus & PDRAW_32_COLS))
	{
		nametab=(pvid->reg[3]&0x3c)<<9; // 40-cell mode
		nametab_step = 1<<6;
	}
	else
	{
		nametab=(pvid->reg[3]&0x3e)<<9; // 32-cell mode
		nametab_step = 1<<5;
		if (est->rendstatus & PDRAW_BORDER_32)
			scrpos += 32;
	}
	nametab += nametab_step*(start-scrstart);

	// check priority
	code=est->PicoMem_vram[nametab+tile_start];
	if ((code>>15) != prio) return; // hack: just assume that whole window uses same priority

	scrpos+=8*est->Draw2Width+8;
	scrpos+=8*est->Draw2Width*(start-scrstart);

	// do a window until we reach planestart row
	for(trow = start; trow < end; trow++, nametab+=nametab_step) { // current tile row
		if (!ROW_VIS(scrpos, est->Draw2Width)) { scrpos += est->Draw2Width*8; continue; }
		for (tilex=tile_start; tilex<tile_end; tilex++)
		{
			int code,addr,zero=0;
//			unsigned short *pal=NULL;
			unsigned char pal;

			code=est->PicoMem_vram[nametab+tilex];
			if (code==blank) continue;

			// Get tile address/2:
#if INTERLACE
			if (est->rendstatus & PDRAW_INTERLACE)
				addr=(code&0x3ff)<<5;
			else
#endif
				addr=(code&0x7ff)<<4;

//			pal=PicoCramHigh+((code>>9)&0x30);
			pal=(unsigned char)((code>>9)&0x30);

			switch((code>>11)&3) {
				case 0: zero=TileXnormYnorm(scrpos+(tilex<<3),addr,pal,est TILE_Y(8 + 8*(trow - scrstart))); break;
				case 1: zero=TileXflipYnorm(scrpos+(tilex<<3),addr,pal,est TILE_Y(8 + 8*(trow - scrstart))); break;
				case 2: zero=TileXnormYflip(scrpos+(tilex<<3),addr,pal,est TILE_Y(8 + 8*(trow - scrstart))); break;
				case 3: zero=TileXflipYflip(scrpos+(tilex<<3),addr,pal,est TILE_Y(8 + 8*(trow - scrstart))); break;
			}
			if(zero) blank=code; // We know this tile is blank now
		}

		scrpos += est->Draw2Width*8;
	}
}


static void DrawLayerFull(int plane, u32 *hcache, int planestart, int planeend,
			  struct PicoEState *est)
{
	struct PicoVideo *pvid=&est->Pico->video;
	static char shift[4]={5,6,5,7}; // 32,64 or 128 sized tilemaps
	int width, height, ymask, htab;
	int nametab, hscroll=0, vscroll, cells;
	unsigned char *scrpos;
	int scrstart = est->Draw2Start;
	int blank=-1, xmask, nametab_row, trow;

	// parse ranges
	cells = (planeend>>16)-(planestart>>16);
	planestart = planestart<<16>>16;
	planeend = planeend<<16>>16;

	// Work out the Tiles to draw

	htab=pvid->reg[13]<<9; // Horizontal scroll table address
//	if ( pvid->reg[11]&2)     htab+=Scanline<<1; // Offset by line
//	if ((pvid->reg[11]&1)==0) htab&=~0xf; // Offset by tile
	htab+=plane; // A or B

	if(!(pvid->reg[11]&3)) { // full screen scroll
		// Get horizontal scroll value
		hscroll=est->PicoMem_vram[htab&0x7fff];
		htab = 0; // this marks that we don't have to update scroll value
	}

	// Work out the name table size: 32 64 or 128 tiles (0-3)
	width=pvid->reg[16];
	height=(width>>4)&3; width&=3;

	xmask=(1<<shift[width ])-1; // X Mask in tiles
	ymask=(height<<5)|0x1f;     // Y Mask in tiles
	if(width == 1)   ymask&=0x3f;
	else if(width>1) ymask =0x1f;

	// Find name table:
	if (plane==0) nametab=(pvid->reg[2]&0x38)<< 9; // A
	else          nametab=(pvid->reg[4]&0x07)<<12; // B

	scrpos = est->Draw2FB;
	if ((~est->rendstatus & (PDRAW_BORDER_32|PDRAW_32_COLS)) == 0)
		scrpos += 32;
	scrpos+=8*est->Draw2Width*(planestart-scrstart);

	if((pvid->reg[11]&4)||(PicoMem.vsram[plane]&7))
		planeend++; // we (may) have vertically clipped tiles due to vscroll, so we need 1 more row
	for(trow = planestart; trow < planeend; trow++) { // current tile row
		int cellc=cells,tilex,dx,vsidx=0;

		// Get vertical scroll value:
		vscroll=PicoMem.vsram[plane];//&0x1ff;
#if VSRAM
		if ((est->rendstatus & PDRAW_32_COLS) && (pvid->reg[11]&4)) // H32 + 2-cell mode
			vscroll=PicoMem.vsram[plane+0x20];//&0x1ff;
#endif
#if INTERLACE
		if (est->rendstatus & PDRAW_INTERLACE)
			vscroll >>= 1;
#endif
		nametab_row = nametab + (((trow+(vscroll>>3))&ymask)<<shift[width]); // pointer to nametable entries for this row

		// Find the tile row in the name table
		//ts.line=(vscroll+Scanline)&ymask;
		//ts.nametab+=(ts.line>>3)<<shift[width];

		// update hscroll if needed
		if(htab) {
			int htaddr=htab+(trow<<4);
			if(trow) htaddr-=(vscroll&7)<<1;
			hscroll=est->PicoMem_vram[htaddr&0x7fff];
		}

		if (!ROW_VIS(scrpos+(8-(vscroll&7))*est->Draw2Width, est->Draw2Width)) {
			scrpos += est->Draw2Width*8;
			continue;
		}

		// Draw tiles across screen:
		tilex=(-hscroll)>>3;
		dx=((hscroll-1)&7)+1;
		if(dx != 8) cellc++, vsidx--; // have hscroll, do more cells

		for (; cellc; dx+=8,tilex++,cellc--)
		{
			int code=0,addr=0,zero=0,scroff;
//			unsigned short *pal=NULL;
			unsigned char pal;

#if VSRAM
			if ((pvid->reg[11]&4) && !(vsidx&1)) { // 2-cell mode
				vscroll=PicoMem.vsram[vsidx+plane];//&0x1ff;
#if INTERLACE
				if (est->rendstatus & PDRAW_INTERLACE)
					vscroll >>= 1;
#endif
				nametab_row = nametab + (((trow+(vscroll>>3))&ymask)<<shift[width]); // pointer to nametable entries for this row
			}
#endif
			vsidx++;

			code=est->PicoMem_vram[nametab_row+(tilex&xmask)];
			if (code==blank) continue;

			if (code>>15) { // high priority tile
				*hcache++ = code|(dx<<16)|(trow<<27); // cache it
				*hcache++ = 8-(vscroll&7); // push y-offset to tilecache
				continue;
			}

			// Get tile address/2:
#if INTERLACE
			if (est->rendstatus & PDRAW_INTERLACE)
				addr=(code&0x3ff)<<5;
			else
#endif
				addr=(code&0x7ff)<<4;

//			pal=PicoCramHigh+((code>>9)&0x30);
			pal=(unsigned char)((code>>9)&0x30);

			scroff=(8-(vscroll&7))*est->Draw2Width;
			switch((code>>11)&3) {
				case 0: zero=TileXnormYnorm(scrpos+scroff+dx,addr,pal,est TILE_Y(8*(trow - scrstart) + 8 - (vscroll&7))); break;
				case 1: zero=TileXflipYnorm(scrpos+scroff+dx,addr,pal,est TILE_Y(8*(trow - scrstart) + 8 - (vscroll&7))); break;
				case 2: zero=TileXnormYflip(scrpos+scroff+dx,addr,pal,est TILE_Y(8*(trow - scrstart) + 8 - (vscroll&7))); break;
				case 3: zero=TileXflipYflip(scrpos+scroff+dx,addr,pal,est TILE_Y(8*(trow - scrstart) + 8 - (vscroll&7))); break;
			}
			if(zero) blank=code; // We know this tile is blank now
		}

		scrpos += est->Draw2Width*8;
	}

	*hcache = 0; // terminate cache
}


static void DrawTilesFromCacheF(u32 *hc, struct PicoEState *est)
{
	u32 code;
	int addr, zero = 0, vscroll;
	unsigned int prevy=0xFFFFFFFF;
//	unsigned short *pal;
	unsigned char pal;
	short blank=-1; // The tile we know is blank
	unsigned char *scrpos = est->Draw2FB, *pd = 0;
	int scrstart = est->Draw2Start;

	if ((~est->rendstatus & (PDRAW_BORDER_32|PDRAW_32_COLS)) == 0)
		scrpos += 32;

	while((code=*hc++)) {
		int yrow;
		yrow = *hc++ - START_ROW*8;
		vscroll = yrow*est->Draw2Width;
		if((short)code == blank) continue;

		// y pos
		if(((unsigned)code>>27) != prevy) {
			prevy = (unsigned)code>>27;
			pd = scrpos + (prevy-scrstart)*est->Draw2Width*8;
		}

		yrow += (prevy-scrstart)*8;

		// Get tile address/2:
#if INTERLACE
		if (est->rendstatus & PDRAW_INTERLACE)
			addr=(code&0x3ff)<<5;
		else
#endif
			addr=(code&0x7ff)<<4;
//		pal=PicoCramHigh+((code>>9)&0x30);
		pal=(unsigned char)((code>>9)&0x30);

		switch((code>>11)&3) {
			case 0: zero=TileXnormYnorm(pd+vscroll+((code>>16)&0x1ff),addr,pal,est TILE_Y(yrow)); break;
			case 1: zero=TileXflipYnorm(pd+vscroll+((code>>16)&0x1ff),addr,pal,est TILE_Y(yrow)); break;
			case 2: zero=TileXnormYflip(pd+vscroll+((code>>16)&0x1ff),addr,pal,est TILE_Y(yrow)); break;
			case 3: zero=TileXflipYflip(pd+vscroll+((code>>16)&0x1ff),addr,pal,est TILE_Y(yrow)); break;
		}

		if(zero) blank=(short)code;
	}
}


// sx and sy are coords of virtual screen with 8pix borders on top and on left
static void DrawSpriteFull(u32 *sprite, struct PicoEState *est)
{
	int width=0,height=0;
//	unsigned short *pal=NULL;
	unsigned char pal;
	int tile,code,tdeltax,tdeltay;
	unsigned char *scrpos;
	int scrstart = est->Draw2Start;
	int sx, sy;

	sy=CPU_LE2(sprite[0]);
	height=sy>>24;
#if INTERLACE
	if (est->rendstatus & PDRAW_INTERLACE)
		sy = ((sy>>1)&0x1ff)-0x78;
	else
#endif
		sy=(sy&0x1ff)-0x78; // Y
	width=(height>>2)&3; height&=3;
	width++; height++; // Width and height in tiles

	code=CPU_LE2(sprite[1]);
	sx=((code>>16)&0x1ff)-0x78; // X

	tile=code&0x7ff; // Tile number
	tdeltax=height; // Delta to increase tile by going right
	tdeltay=1;      // Delta to increase tile by going down
	if (code&0x1000) { tile+=tdeltax-1; tdeltay=-tdeltay; } // Flip Y
	if (code&0x0800) { tile+=tdeltax*(width-1); tdeltax=-tdeltax; } // Flip X

	//delta<<=4; // Delta of address
//	pal=PicoCramHigh+((code>>9)&0x30); // Get palette pointer
	pal=(unsigned char)((code>>9)&0x30);

	// goto first vertically visible tile
	sy -= scrstart*8;
	while(sy <= 0) { sy+=8; tile+=tdeltay; height--; }

	scrpos = est->Draw2FB;
	if ((~est->rendstatus & (PDRAW_BORDER_32|PDRAW_32_COLS)) == 0)
		scrpos += 32;
	scrpos+=sy*est->Draw2Width;

	for (; height > 0; height--, sy+=8, tile+=tdeltay)
	{
		int w = width, x=sx, t=tile, s;

		if(sy >= END_ROW*8+8) return; // offscreen
		if (!ROW_VIS(scrpos, est->Draw2Width)) {
			if (scrpos >= clip_hi) return;
			scrpos += 8*est->Draw2Width;
			continue;
		}

		for (; w; w--,x+=8,t+=tdeltax)
		{
			if(x<=0)   continue;
			if(x>=328) break; // Offscreen

			t&=0x7fff; // Clip tile address
#if INTERLACE
			if (est->rendstatus & PDRAW_INTERLACE)
				s=5;
			else
#endif
				s=4;
			switch((code>>11)&3) {
				case 0: TileXnormYnorm(scrpos+x,t<<s,pal,est TILE_Y(sy)); break;
				case 1: TileXflipYnorm(scrpos+x,t<<s,pal,est TILE_Y(sy)); break;
				case 2: TileXnormYflip(scrpos+x,t<<s,pal,est TILE_Y(sy)); break;
				case 3: TileXflipYflip(scrpos+x,t<<s,pal,est TILE_Y(sy)); break;
			}
		}

		scrpos+=8*est->Draw2Width;
	}
}
#endif


/* select the visible sprites of one priority (in table order); returns count */
static int CollectSpritesFull(int prio, int maxwidth, struct PicoEState *est, u32 **sprites)
{
	struct PicoVideo *pvid=&est->Pico->video;
	int table=0,maskrange=0;
	int i,u,link=0;
	int y_min=START_ROW*8, y_max=END_ROW*8; // for a simple sprite masking
	int max_sprites = !(est->rendstatus & PDRAW_32_COLS) ? 80 : 64;

	if (est->rendstatus & PDRAW_30_ROWS)
		y_min += 8, y_max += 8;

	table=pvid->reg[5]&0x7f;
	if (!(est->rendstatus & PDRAW_32_COLS)) table&=0x7e; // Lowest bit 0 in 40-cell mode
	table<<=8; // Get sprite table address/2

	for (i = u = 0; u < max_sprites && link < max_sprites; u++)
	{
		u32 *sprite=NULL;
		int code, code2, sx, sy, height;

		sprite=(u32 *)(est->PicoMem_vram+((table+(link<<2))&0x7ffc)); // Find sprite

		// get sprite info
		code = CPU_LE2(sprite[0]);

		// check if it is not hidden vertically
#if INTERLACE
		if (est->rendstatus & PDRAW_INTERLACE)
			sy = ((code>>1)&0x1ff)-0x80;
		else
#endif
			sy = (code&0x1ff)-0x80;
		height = (((code>>24)&3)+1)<<3;
		if(sy+height <= y_min || sy > y_max) goto nextsprite;

		// masking sprite?
		code2=CPU_LE2(sprite[1]);
		sx = (code2>>16)&0x1ff;
		if(!sx) {
			int to = sy+height; // sy ~ from
			if(maskrange) {
				// try to merge with previous range
				if((maskrange>>16)+1 >= sy && (maskrange>>16) <= to && (maskrange&0xffff) < sy) sy = (maskrange&0xffff);
				else if((maskrange&0xffff)-1 <= to && (maskrange&0xffff) >= sy && (maskrange>>16) > to) to = (maskrange>>16);
			}
			// support only very simple masking (top and bottom of screen)
			if(sy <= y_min && to+1 > y_min) y_min = to+1;
			else if(to >= y_max && sy-1 < y_max) y_max = sy-1;
			else maskrange=sy|(to<<16);

			goto nextsprite;
		}

		// priority
		if(((code2>>15)&1) != prio) goto nextsprite; // wrong priority

		// check if sprite is not hidden horizontally
		sx -= 0x78; // Get X coordinate + 8
		if(sx <= -8*3 || sx >= maxwidth) goto nextsprite;

		// sprite is good, save it's index
		sprites[i++]=sprite;

		nextsprite:
		// Find next sprite
		link=(code>>16)&0x7f;
		if(!link) break; // End of sprites
	}
	return i;
}

static void DrawAllSpritesFull(int prio, int maxwidth, struct PicoEState *est)
{
	u32 *sprites[80]; // Sprites
	int i = CollectSpritesFull(prio, maxwidth, est, sprites);

	// Go through sprites backwards:
	for (i--; i >= 0; i--)
	{
		DrawSpriteFull(sprites[i], est);
	}
}


#ifdef N64_RSP_VDP
/* ---- RSP mode: command lists ----
 * Each layer is walked once over the whole frame; every tile becomes one
 * command per 8-line band it touches (a tile row at an unaligned y touches
 * two), appended to that band's "lo" or "hi" list by priority.  The RSP
 * draws a band's lo list, then its hi list.  Appending in the order
 * plane B, plane A, window, low sprites, high sprites gives
 *   lo: B lo, A lo, window (if low), sprites lo
 *   hi: B hi, A hi, window (if high), sprites hi
 * which is the order in which the CPU renderer draws (DrawDisplayFull),
 * so every pixel ends up the same.  Command (see rsp_vdp.S): bits 0-8 x,
 * 9-12 yrel = tile top - band top (-7..7), 13-27 = bits 0-14 of the VDP
 * name / sprite attribute word (tile, x flip, y flip, palette). */
#define RL_BANDS (END_ROW - START_ROW)  /* 8-line bands, framebuffer rows 8.. */
#define RL_CAP   512                    /* commands per list (LIST_BYTES/4) */
static struct {
	u16 cnt[64];                        /* [band*2 + hi] */
	u32 cmd[RL_BANDS * 2][RL_CAP];
} rl __attribute__((aligned(16)));
static u32 *rl_wp[RL_BANDS * 2];        /* append pointers */
static u32 rl_sink[64];                 /* writes for bands off the screen */
static int rl_ovf;

/* lists of the (up to) two bands that the tile row at framebuffer row y
 * touches, the yrel bits for each; overflow if one of them can't take n */
#define RL_ROW(y, hi, n, p0, p1, y0, y1) \
	int b_ = ((y) - 8) >> 3, r_ = ((y) - 8) & 7; \
	u32 y0 = (u32)r_ << 9, y1 = (u32)((r_ - 8) & 15) << 9; \
	u32 *p0 = rl_sink, *p1 = rl_sink; \
	if ((unsigned)b_ < RL_BANDS) { \
		p0 = rl_wp[2*b_ + (hi)]; \
		if (p0 + (n) > rl.cmd[2*b_ + (hi)] + RL_CAP) { rl_ovf = 1; return; } \
	} \
	if (r_ && (unsigned)(b_ + 1) < RL_BANDS) { \
		p1 = rl_wp[2*b_ + 2 + (hi)]; \
		if (p1 + (n) > rl.cmd[2*b_ + 2 + (hi)] + RL_CAP) { rl_ovf = 1; return; } \
	}
#define RL_ROW_END(hi, p0, p1) \
	if ((unsigned)b_ < RL_BANDS) rl_wp[2*b_ + (hi)] = p0; \
	if (r_ && (unsigned)(b_ + 1) < RL_BANDS) rl_wp[2*b_ + 2 + (hi)] = p1;

/* DrawLayerFull, as command lists */
static void EmitLayer(int plane, int planestart, int planeend, struct PicoEState *est)
{
	struct PicoVideo *pvid = &est->Pico->video;
	static const char shift[4] = {5,6,5,7};
	const unsigned short *vram = est->PicoMem_vram;
	int width, height, ymask, xmask, htab, nametab, hscroll = 0, vscroll, cells, trow;
	int scrstart = est->Draw2Start, fb_end = 8 + (END_ROW-START_ROW)*8;
	u32 xoff = (~est->rendstatus & (PDRAW_BORDER_32|PDRAW_32_COLS)) == 0 ? 32 : 0;

	cells = (planeend>>16)-(planestart>>16);
	planestart = planestart<<16>>16;
	planeend = planeend<<16>>16;

	htab = (pvid->reg[13]<<9) + plane;
	if (!(pvid->reg[11]&3)) {      /* full screen scroll */
		hscroll = vram[htab&0x7fff];
		htab = 0;
	}
	width = pvid->reg[16];
	height = (width>>4)&3; width &= 3;
	xmask = (1<<shift[width])-1;
	ymask = (height<<5)|0x1f;
	if (width == 1)   ymask &= 0x3f;
	else if (width>1) ymask = 0x1f;
	if (plane==0) nametab = (pvid->reg[2]&0x38)<< 9; /* A */
	else          nametab = (pvid->reg[4]&0x07)<<12; /* B */

	vscroll = PicoMem.vsram[plane];
	if ((pvid->reg[11]&4) || (vscroll&7))
		planeend++;
	for (trow = planestart; trow < planeend; trow++) {
		int nametab_row = nametab + (((trow+(vscroll>>3))&ymask)<<shift[width]);
		int y = 8*(trow - scrstart) + 8 - (vscroll&7);
		int tilex, dx, cellc = cells;

		if (htab) {
			int htaddr = htab+(trow<<4);
			if (trow) htaddr -= (vscroll&7)<<1;
			hscroll = vram[htaddr&0x7fff];
		}
		if (y <= 0 || y >= fb_end)
			continue;
		tilex = (-hscroll)>>3;
		dx = ((hscroll-1)&7)+1;
		if (dx != 8) cellc++;
		{
			/* the (up to) two bands this tile row touches, lo and hi lists */
			int b = (y - 8) >> 3, r = (y - 8) & 7;
			u32 y0 = (u32)r << 9, y1 = (u32)((r - 8) & 15) << 9;
			u32 *l0 = rl_sink, *h0 = rl_sink, *l1 = rl_sink, *h1 = rl_sink;
			int v0 = (unsigned)b < RL_BANDS, v1 = r && (unsigned)(b + 1) < RL_BANDS;
			if (v0) {
				l0 = rl_wp[2*b]; h0 = rl_wp[2*b + 1];
				if (l0 + cellc > rl.cmd[2*b] + RL_CAP || h0 + cellc > rl.cmd[2*b + 1] + RL_CAP)
					{ rl_ovf = 1; return; }
			}
			if (v1) {
				l1 = rl_wp[2*b + 2]; h1 = rl_wp[2*b + 3];
				if (l1 + cellc > rl.cmd[2*b + 2] + RL_CAP || h1 + cellc > rl.cmd[2*b + 3] + RL_CAP)
					{ rl_ovf = 1; return; }
			}
			for (; cellc; dx += 8, tilex++, cellc--) {
				u32 code = vram[nametab_row + (tilex&xmask)];
				u32 c = (xoff + dx) | (code & 0x7fff) << 13;
				if (code & 0x8000) { *h0++ = c | y0; *h1++ = c | y1; }
				else               { *l0++ = c | y0; *l1++ = c | y1; }
			}
			if (v0) { rl_wp[2*b] = l0; rl_wp[2*b + 1] = h0; }
			if (v1) { rl_wp[2*b + 2] = l1; rl_wp[2*b + 3] = h1; }
		}
	}
}

/* DrawWindowFull (both priority passes), as command lists */
static void EmitWindow(int start, int end, struct PicoEState *est)
{
	struct PicoVideo *pvid = &est->Pico->video;
	const unsigned short *vram = est->PicoMem_vram;
	int nametab, nametab_step, trow, tilex, hi;
	int scrstart = est->Draw2Start;
	int tile_start = start>>16, tile_end = end>>16;
	u32 xoff = 8;

	start = start<<16>>16;
	end = end<<16>>16;
	if (!(est->rendstatus & PDRAW_32_COLS)) {
		nametab = (pvid->reg[3]&0x3c)<<9; /* 40-cell mode */
		nametab_step = 1<<6;
	} else {
		nametab = (pvid->reg[3]&0x3e)<<9; /* 32-cell mode */
		nametab_step = 1<<5;
		if (est->rendstatus & PDRAW_BORDER_32)
			xoff += 32;
	}
	nametab += nametab_step*(start-scrstart);
	/* the whole window is drawn in the pass of its first tile's priority */
	hi = vram[nametab+tile_start] >> 15;

	for (trow = start; trow < end; trow++, nametab += nametab_step) {
		int b = trow - scrstart, i = 2*b + hi;   /* tile rows are band aligned */
		u32 *p;
		if ((unsigned)b >= RL_BANDS)
			continue;
		p = rl_wp[i];
		if (p + (tile_end - tile_start) > rl.cmd[i] + RL_CAP) { rl_ovf = 1; return; }
		for (tilex = tile_start; tilex < tile_end; tilex++)
			*p++ = (xoff + (tilex<<3)) | (u32)(vram[nametab+tilex] & 0x7fff) << 13;
		rl_wp[i] = p;
	}
}

/* DrawSpriteFull, as command lists */
static void EmitSprite(u32 *sprite, int hi, struct PicoEState *est)
{
	int width, height, tile, code, tdeltax, tdeltay, sx, sy;
	int scrstart = est->Draw2Start;
	u32 xoff = (~est->rendstatus & (PDRAW_BORDER_32|PDRAW_32_COLS)) == 0 ? 32 : 0;
	u32 cbits;

	sy = CPU_LE2(sprite[0]);
	height = sy>>24;
	sy = (sy&0x1ff)-0x78; /* Y */
	width = (height>>2)&3; height &= 3;
	width++; height++;    /* width and height in tiles */

	code = CPU_LE2(sprite[1]);
	sx = ((code>>16)&0x1ff)-0x78; /* X */
	cbits = (u32)(code & 0x7800) << 13;   /* flips, palette */

	tile = code&0x7ff;
	tdeltax = height;
	tdeltay = 1;
	if (code&0x1000) { tile+=tdeltax-1; tdeltay=-tdeltay; } /* flip Y */
	if (code&0x0800) { tile+=tdeltax*(width-1); tdeltax=-tdeltax; } /* flip X */

	sy -= scrstart*8;
	while (sy <= 0) { sy+=8; tile+=tdeltay; height--; }

	for (; height > 0; height--, sy+=8, tile+=tdeltay) {
		int w = width, x = sx, t = tile;
		if (sy >= END_ROW*8+8) return; /* offscreen */
		{
			RL_ROW(sy, hi, 4, p0, p1, y0, y1)
			for (; w; w--, x+=8, t+=tdeltax) {
				u32 c;
				if (x<=0)   continue;
				if (x>=328) break;
				c = (xoff + x) | (u32)(t & 0x7ff) << 13 | cbits;
				*p0++ = c | y0;
				*p1++ = c | y1;
			}
			RL_ROW_END(hi, p0, p1)
		}
	}
}

/* DrawDisplayFull's layer sequence as RSP command lists, then queue the
 * render.  Returns 0 if a list overflowed (nothing queued). */
static int EmitDisplayRSP(int hvwin, int planestart, int planeend, int winstart,
                          int winend, int scrstart, int scrend, int maxw, int maxcolc)
{
	struct PicoEState *est = &Pico.est;
	struct PicoVideo *pvid = &est->Pico->video;
	u32 *spr[80];
	int i, n;

	/* the RSP copies VRAM while the lists are built */
	rsp_vdp_snapshot(est->PicoMem_vram);
	for (i = 0; i < RL_BANDS * 2; i++)
		rl_wp[i] = rl.cmd[i];
	rl_ovf = 0;

	if (!(pvid->debug_p & PVD_KILL_B))
		EmitLayer(1, scrstart, (maxcolc<<16)|scrend, est);
	if (!(pvid->debug_p & PVD_KILL_A)) switch (hvwin) {
		case 4: /* fullscreen window */
			EmitWindow(scrstart, (maxcolc<<16)|scrend, est);
			break;
		case 3: /* plane A and both v and h windows */
			EmitLayer(0, planestart, planeend, est);
			EmitWindow( winstart&~0xff0000, (winend&~0xff0000)|(maxcolc<<16), est); /* h */
			EmitWindow((winstart&~0xff)|scrstart, (winend&~0xff)|scrend, est);    /* v */
			break;
		case 2:
		case 1: /* window vertical XOR horizontal */
			EmitLayer(0, planestart, planeend, est);
			EmitWindow(winstart, winend, est);
			break;
		default: /* fullscreen plane A */
			EmitLayer(0, scrstart, (maxcolc<<16)|scrend, est);
			break;
	}
	n = (pvid->debug_p & PVD_KILL_S_LO) ? 0 : CollectSpritesFull(0, maxw, est, spr);
	while (n > 0)
		EmitSprite(spr[--n], 0, est);
	n = (pvid->debug_p & PVD_KILL_S_HI) ? 0 : CollectSpritesFull(1, maxw, est, spr);
	while (n > 0)
		EmitSprite(spr[--n], 1, est);
	if (rl_ovf)
		return 0;

	for (i = 0; i < RL_BANDS * 2; i++) {
		n = rl_wp[i] - rl.cmd[i];
		rl.cnt[i] = n;
		if (n) {
			data_cache_hit_writeback(rl.cmd[i], n * 4);
			if ((unsigned)n > draw2_rsp_maxlist) draw2_rsp_maxlist = n;
		}
	}
	data_cache_hit_writeback(rl.cnt, sizeof(rl.cnt));
	/* the RSP writes framebuffer rows 8.. by DMA: no dirty CPU copy of them
	 * (from a frame drawn on the CPU) may be written back over its output
	 * later.  Normally there is none: the platform wrote that frame back
	 * to display it. */
	if (draw2_fb_dirty) {
		data_cache_hit_writeback_invalidate(est->Draw2FB, (8 + RL_BANDS * 8 + 8) * est->Draw2Width);
		draw2_fb_dirty = 0;
	}
	rsp_vdp_render(&rl, RL_BANDS, pvid->reg[7], est->Draw2FB + 8 * est->Draw2Width + 8);
	rsp_took = 1;
#ifdef RSP_VDP_SYNC
	rsp_vdp_wait();
#endif
	return 1;
}
#endif /* N64_RSP_VDP */

#ifndef _ASM_DRAW_C
static void BackFillRows(unsigned char *dst, int reg7, int lwidth, int y0, int y1)
{
	unsigned int back;
	int i;

#ifdef N64
	/* The fill covers the whole render area, which is 8x the 8KB
	 * direct-mapped D-cache: every line it touches is a write miss, and a
	 * plain store miss first READS the old line from RDRAM, only to
	 * overwrite all of it.  CACHE 0x0D (Create Dirty Exclusive) claims the
	 * line without that read.  It is only safe when every byte of each
	 * claimed 16-byte line is then written, so fill the 224 rows as one
	 * contiguous block, left margins included (margins are never
	 * displayed and tiles overdraw them anyway). */
	{
		unsigned char *p = dst + y0*lwidth, *end = dst + y1*lwidth;
		if (((((uintptr_t)p) | ((uintptr_t)end)) & 15) == 0 &&
		    ((uintptr_t)p >> 29) == 4) {   /* cached KSEG0 only */
			unsigned long long b8 = (reg7 & 0x3f) * 0x0101010101010101ULL;
			for (; p < end; p += 16) {
				__asm__ volatile ("cache 0x0d, 0(%0)" :: "r"(p) : "memory");
				((unsigned long long *)p)[0] = b8;
				((unsigned long long *)p)[1] = b8;
			}
			return;
		}
	}
#endif

	// Start with a background color:
	back=reg7&0x3f;
	back|=back<<8;
	back|=back<<16;

	for (i = y0, dst += y0*lwidth; i < y1; i++, dst += lwidth)
		memset32(dst+8, back, 320/4);
}

static void BackFillFull(unsigned char *dst, int reg7, int lwidth)
{
	BackFillRows(dst, reg7, lwidth, 8, 8 + (END_ROW-START_ROW)*8);
}
#endif

static void DrawDisplayFull(void)
{
	struct PicoEState *est = &Pico.est;
	struct PicoVideo *pvid=&est->Pico->video;
	int win, edge=0, hvwin=0; // LSb->MSb: hwin&plane, vwin&plane, full
	int scrstart=START_ROW, scrend = END_ROW; // our render area
	int planestart, planeend; // plane A start/end when window shares display with plane A (in tile rows or columns)
	int winstart, winend;     // same for window
	int maxw, maxcolc; // max width and col cells

	if(est->rendstatus & PDRAW_32_COLS) {
		maxw = 264; maxcolc = 32;
	} else {
		maxw = 328; maxcolc = 40;
	}
	if(est->rendstatus & PDRAW_30_ROWS) {
		// In 240 line mode, the top and bottom 8 lines are omitted
		// since this renderer always renders 224 lines
		scrstart ++, scrend ++;
	}
	est->Draw2Start = scrstart;

	planestart = scrstart, planeend = scrend;
	winstart = scrstart, winend = scrend;

	// horizontal window?
	if ((win=pvid->reg[0x12]))
	{
		hvwin=1; // hwindow shares display with plane A
		edge=win&0x1f;
		if(win == 0x80) {
			// fullscreen window
			hvwin=4;
		} else if(win < 0x80) {
			// window on the top
			     if(edge <= scrstart) hvwin=0; // window not visible in our drawing region
			else if(edge >= scrend)   hvwin=4;
			else planestart = winend = edge;
		} else if(win > 0x80) {
			// window at the bottom
			if(edge >= scrend) hvwin=0;
			else planeend = winstart = edge;
		}
	}

	// check for vertical window, but only if win is not fullscreen
	if (hvwin != 4)
	{
		win=pvid->reg[0x11];
		edge=win&0x1f;
		if (win&0x80) {
			if(!edge) hvwin=4;
			else if(edge < (maxcolc>>1)) {
				// window is on the right
				hvwin|=2;
				planeend|=edge<<17;
				winstart|=edge<<17;
				winend|=maxcolc<<16;
			}
		} else {
			if(edge >= (maxcolc>>1)) hvwin=4;
			else if(edge) {
				// window is on the left
				hvwin|=2;
				winend|=edge<<17;
				planestart|=edge<<17;
				planeend|=maxcolc<<16;
			}
		}
	}

	if (hvwin==1) { winend|=maxcolc<<16; planeend|=maxcolc<<16; }

#ifdef N64_RSP_VDP
	if (draw2_rsp) {
		if (EmitDisplayRSP(hvwin, planestart, planeend, winstart, winend,
		                   scrstart, scrend, maxw, maxcolc))
			return;
		draw2_rsp_ovf++;   /* lists full: draw this frame on the CPU */
	}
#endif

#if BANDED
	u32 *spr_lo[80], *spr_hi[80];
	int n_lo = (pvid->debug_p & PVD_KILL_S_LO) ? 0 : CollectSpritesFull(0, maxw, est, spr_lo);
	int n_hi = (pvid->debug_p & PVD_KILL_S_HI) ? 0 : CollectSpritesFull(1, maxw, est, spr_hi);
	int band_y, fb_end = 8 + (END_ROW-START_ROW)*8, band_h = DRAW2_BAND;
	for (band_y = 8; band_y < fb_end; band_y += band_h) {
	int band_y1 = band_y + band_h < fb_end ? band_y + band_h : fb_end;
	int si;
	clip_lo = est->Draw2FB + band_y  * est->Draw2Width;
	clip_hi = est->Draw2FB + band_y1 * est->Draw2Width;
	band_r0 = band_y;
	band_r1 = band_y1;
	BackFillRows(est->Draw2FB, pvid->reg[7], est->Draw2Width, band_y, band_y1);
#endif
	HighCache2A[0] = HighCache2B[0] = 0;
	PROF2_START();
	if (!(pvid->debug_p & PVD_KILL_B))
		DrawLayerFull(1, HighCache2B, scrstart, (maxcolc<<16)|scrend, est);
	if (!(pvid->debug_p & PVD_KILL_A)) switch (hvwin)
	{
		case 4:
		// fullscreen window
		DrawWindowFull(scrstart, (maxcolc<<16)|scrend, 0, est);
		break;

		case 3:
		// we have plane A and both v and h windows
		DrawLayerFull(0, HighCache2A, planestart, planeend, est);
		DrawWindowFull( winstart&~0xff0000, (winend&~0xff0000)|(maxcolc<<16), 0, est); // h
		DrawWindowFull((winstart&~0xff)|scrstart, (winend&~0xff)|scrend, 0, est);    // v
		break;

		case 2:
		case 1:
		// both window and plane A visible, window is vertical XOR horizontal
		DrawLayerFull(0, HighCache2A, planestart, planeend, est);
		DrawWindowFull(winstart, winend, 0, est);
		break;

		default:
		// fullscreen plane A
		DrawLayerFull(0, HighCache2A, scrstart, (maxcolc<<16)|scrend, est);
		break;
	}
	PROF2_LAY();
#if BANDED
	for (si = n_lo - 1; si >= 0; si--)
		DrawSpriteFull(spr_lo[si], est);
#else
	if (!(pvid->debug_p & PVD_KILL_S_LO))
		DrawAllSpritesFull(0, maxw, est);
#endif
	PROF2_SPR();

	if (HighCache2B[0]) DrawTilesFromCacheF(HighCache2B, est);
	if (HighCache2A[0]) DrawTilesFromCacheF(HighCache2A, est);
	PROF2_LAY();
	if (!(pvid->debug_p & PVD_KILL_A)) switch (hvwin)
	{
		case 4:
		// fullscreen window
		DrawWindowFull(scrstart, (maxcolc<<16)|scrend, 1, est);
		break;

		case 3:
		// we have plane A and both v and h windows
		DrawWindowFull( winstart&~0xff0000, (winend&~0xff0000)|(maxcolc<<16), 1, est); // h
		DrawWindowFull((winstart&~0xff)|scrstart, (winend&~0xff)|scrend, 1, est);    // v
		break;

		case 2:
		case 1:
		// both window and plane A visible, window is vertical XOR horizontal
		DrawWindowFull(winstart, winend, 1, est);
		break;
	}
#if BANDED
	for (si = n_hi - 1; si >= 0; si--)
		DrawSpriteFull(spr_hi[si], est);
	PROF2_SPR();
	}  /* band loop */
	clip_lo = (unsigned char *)0;
	clip_hi = (unsigned char *)~(uintptr_t)0;
	band_r0 = 0;
	band_r1 = 1 << 30;
#else
	if (!(pvid->debug_p & PVD_KILL_S_HI))
		DrawAllSpritesFull(1, maxw, est);
	PROF2_SPR();
#endif
}


PICO_INTERNAL void PicoFrameFull(void)
{
	pprof_start(draw);
#ifdef N64_RSP_VDP
	rsp_vdp_wait();   /* previous RSP render: framebuffer and stream are reused */
#endif

	// prepare cram?
	if (PicoPrepareCram) PicoPrepareCram();
#ifdef N64_RSP_VDP
	rsp_took = 0;
#endif

	// Draw screen:
#if BANDED
	if (Pico.video.reg[1] & 0x40)
		DrawDisplayFull();   /* fills the background band by band */
	else
		BackFillFull(Pico.est.Draw2FB, Pico.video.reg[7], Pico.est.Draw2Width);
#else
	BackFillFull(Pico.est.Draw2FB, Pico.video.reg[7], Pico.est.Draw2Width);
	if (Pico.video.reg[1] & 0x40)
		DrawDisplayFull();
#endif

#if defined(N64_RSP_VDP) && defined(RSP_VDP_CHECK)
	/* verification: render the same frame on the CPU and compare */
	if (draw2_rsp && (Pico.video.reg[1] & 0x40)) {
		static unsigned char shadow[328 * 240] __attribute__((aligned(16)));
		static int reported;
		unsigned char *fb = Pico.est.Draw2FB;
		int y, x, n = 0;
		rsp_vdp_wait();
		data_cache_hit_invalidate(fb + 8 * 328, 224 * 328);
		Pico.est.Draw2FB = shadow;
		draw2_rsp = 0;
		DrawDisplayFull();
		draw2_rsp = 1;
		Pico.est.Draw2FB = fb;
		for (y = 8; y < 232 && n < 12; y++)
			for (x = 8; x < 328 && n < 12; x++)
				if (fb[y * 328 + x] != shadow[y * 328 + x]) {
					if (reported < 40)
						{ extern void n64_logf(const char *, ...); n64_logf("[VCHK] f=%u y=%d x=%d cpu=%02x rsp=%02x\n", Pico.m.frame_count,
						       y, x, shadow[y * 328 + x], fb[y * 328 + x]); }
					n++; reported++;
				}
	}
#endif
#ifdef N64_RSP_VDP
	if (!rsp_took)
		draw2_fb_dirty = 1;
	if (Pico.est.rendstatus & PDRAW_30_ROWS)
		draw2_fb_dirty = 1;
#endif
	// clear top and bottom 8 lines in 240 mode, since draw2 only does 224
	if (Pico.est.rendstatus & PDRAW_30_ROWS) {
		unsigned char *pd = Pico.est.Draw2FB;
		int i;

		for (i = 8; i > 0; i--, pd += Pico.est.Draw2Width)
			memset32((int *)pd, 0xe0e0e0e0, 328/4);
		pd += Pico.est.Draw2Width*(END_ROW-START_ROW)*8;
		for (i = 8; i > 0; i--, pd += Pico.est.Draw2Width)
			memset32((int *)pd, 0xe0e0e0e0, 328/4);
	}

	pprof_end(draw);
}

void PicoDraw2SetOutBuf(void *dest, int incr)
{
	if (dest) {
		Pico.est.Draw2FB = dest;
		Pico.est.Draw2Width = incr;
	} else {
		Pico.est.Draw2FB = PicoDraw2FB_;
		Pico.est.Draw2Width = LINE_WIDTH;
	}
}

void PicoDraw2Init(void)
{
	PicoDraw2SetOutBuf(NULL, 0);
}
