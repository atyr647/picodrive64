/*
 * PicoDrive N64 - Standalone main with embedded ROM
 * 16-bit renderer + optimized BGR555->RGBA5551 blit
 */
/* Undef NDEBUG so libdragon's debugf() + debug_init_usblog() are real
 * (not compiled to no-ops). Must come before any libdragon include. */
#ifdef NDEBUG
# undef NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pico/pico_int.h>
#include <pico/sound/ym2612.h>

#include "../common/input_pico.h"
#include "n64.h"
#include "embedded_rom.h"

#include <rdpq.h>
#include <rdpq_attach.h>
#include <rdpq_mode.h>
#include <rdpq_tex.h>
#include "rsp_audio.h"
#include "rsp_fm.h"
#include "rsp_vdp.h"
#include "emux.h"
#ifdef DRC_68K
#include "../../cpu/drc/drc68k.h"
#endif

/* Profiling counters (written by pico_cmn.c and draw.c, read here) */
unsigned int __attribute__((used)) prof_68k_ticks = 0;
unsigned int __attribute__((used)) prof_vdp_ticks = 0;
unsigned int __attribute__((used)) prof_vdp_layer_ticks = 0;
unsigned int __attribute__((used)) prof_vdp_sprite_ticks = 0;
unsigned int __attribute__((used)) prof_vdp_final_ticks = 0;

/* DAC activity counter: incremented on every write to YM2612 reg 0x2a.
 * We use it to decide whether to skip Z80 on skip frames — games often
 * leave dacen=1 permanently even when no voice is playing, so dacen
 * alone isn't a reliable signal.  Actual 0x2a writes are. */
unsigned int __attribute__((used)) ym_dac_writes = 0;

/* Per-frame perf output -- the "[F] ..." debug line every frame and the
 * on-screen "e.. d.. 68k.. V.. S.." text -- costs ~1 ms/frame (measured on
 * ares; more on hardware, where debugf goes over USB), so it is opt-in:
 *   make N64_EMBEDDED_ROM=1 EXTRA_CFLAGS=-DN64_PERF_HUD
 * The once-per-second [SEC] summary is always on. */

/* Tile renderer selector: 0 = draw2.c CPU path (proven, current default),
 * 1 = draw_rdp.c RDP path (in development).  Toggle via menu or define
 * to A/B test while the RDP renderer matures.  For quick command-line
 * toggling use `EXTRA_CFLAGS=-DN64_RDP_TILES_ON=1`. */
#ifndef N64_RDP_TILES_ON
#define N64_RDP_TILES_ON 0
#endif
int __attribute__((used)) n64_use_rdp_tiles = N64_RDP_TILES_ON;


/* Globals expected by PicoDrive core */
char **g_argv;
void *g_screen_ptr;
int g_screen_width  = 320;
int g_screen_height = 240;
int g_screen_ppitch = 320;

static uint8_t __attribute__((aligned(16))) screen_buffer[328 * 240];
#define SCR_PITCH 328

/* Audio: PicoDrive writes 16-bit PCM here each frame.
 * Mono at 11025 Hz = minimum FM synthesis overhead. */
#define SND_RATE 11025
static short __attribute__((aligned(8))) snd_buffer[SND_RATE / 50 + 16];
/* Upmix buffer: mono -> stereo for libdragon (which requires stereo) */
/* ---- Developer config ----
 * Bench/trace support is compiled into every build so that the code (and,
 * through LTO, the layout of everything else) is identical to the release
 * ROM; it only runs if the ROM carries a "*.pdcfg" rompak file, which
 * platform/n64/tools/make_dev_rom.sh adds when packaging a dev ROM from
 * the same ELF.  A normal ROM has none, so none of it ever runs.  The
 * trace/miss-histogram hooks are ares emux instructions (reserved on real
 * hardware): only enable those for ares. */
#define DEV_BENCH     1   /* scripted input, [BENCH] timing + state hashes */
#define DEV_TRACE     2   /* ares (patched): I/D trace of a frame window */
#define DEV_MISSHIST  4   /* ares (patched): cache-miss histograms */
static struct {
	uint32_t flags, trace_start, trace_frames;
	uint32_t input;			/* benchmark input: 0 Gley Lancer script, 1 none */
	uint32_t bench_start, bench_end;	/* measured window (0: defaults) */
	uint32_t reserved[2];
} devcfg;
extern uint32_t rompak_search_ext(const char *ext, size_t *size);
static void devcfg_load(void)
{
	size_t sz = 0;
	uint32_t a = rompak_search_ext(".pdcfg", &sz);
	if (!a || sz < 16)
		return;
	devcfg.flags = io_read(a);
	devcfg.trace_start = io_read(a + 4);
	devcfg.trace_frames = io_read(a + 8);
	if (sz >= 24) {
		devcfg.input = io_read(a + 12);
		devcfg.bench_start = io_read(a + 16);
		devcfg.bench_end = io_read(a + 20);
	}
}
static inline void dev_xprof(unsigned int code)
{
	/* ares emux XPROF $4, code (6/7 miss histogram, 8/9 trace) */
	register unsigned int a0 __asm__("$4") = 0;
	switch (code) {
	case 6: __asm__ volatile(".word 0x424001a8" :: "r"(a0) : "memory"); break;
	case 7: __asm__ volatile(".word 0x424001e8" :: "r"(a0) : "memory"); break;
	case 8: __asm__ volatile(".word 0x42400228" :: "r"(a0) : "memory"); break;
	case 9: __asm__ volatile(".word 0x42400268" :: "r"(a0) : "memory"); break;
	}
}

static short __attribute__((aligned(8))) snd_stereo[2 * (SND_RATE / 50 + 16)];

/* Audio health tracking.
 *   total_samples   - samples pushed this second (compared to SND_RATE for drift)
 *   blocked_count   - times audio buffer was full at push time (good: rate limited)
 *   underrun_count  - times audio buffer drained below 1 buffer (BAD: distortion)
 *   latest_fill     - most recent "buffers in flight" value (for per-frame output)
 *
 * libdragon internally has AUDIO_NUM_BUFFERS buffers.  audio_can_write()
 * returns how many are currently free/drained.  0 = all full (blocking push).
 * Full count == NUM_BUFFERS means the DMA drained everything while we weren't
 * feeding it = underrun = audible clicks/pops. */
static unsigned int snd_total_samples = 0;
static unsigned int snd_blocked_count = 0;
static unsigned int snd_underrun_count = 0;
static unsigned int snd_latest_fill = 0;  /* buffers in flight when we pushed */

#define AUDIO_NUM_BUFFERS 4

static void write_sound(int len)
{
	int nsamples = len / 2;
	if (nsamples <= 0) return;
	for (int i = 0; i < nsamples; i++) {
		snd_stereo[i*2]   = snd_buffer[i];
		snd_stereo[i*2+1] = snd_buffer[i];
	}
	int free_bufs = audio_can_write();
	if (free_bufs <= 0)
		snd_blocked_count++;
	/* Underrun: hardware has fully drained - playback gap audible. */
	if (free_bufs >= AUDIO_NUM_BUFFERS)
		snd_underrun_count++;
	/* In-flight = total - free (for reporting). */
	snd_latest_fill = (unsigned)(AUDIO_NUM_BUFFERS - free_bufs);
	snd_total_samples += nsamples;
#ifdef N64_BENCH
	if (devcfg.flags & DEV_BENCH) {
		extern uint32_t bench_audio_hash; extern unsigned int bench_audio_n;
		const uint8_t *b = (const uint8_t *)snd_buffer;
		for (int i = 0; i < nsamples * 2; i++)
			bench_audio_hash = (bench_audio_hash ^ b[i]) * 16777619u;
		bench_audio_n += nsamples;
#ifdef BENCH_AUDIO_TRACE
		{ uint32_t fh = 2166136261u;
		  for (int i = 0; i < nsamples * 2; i++) fh = (fh ^ b[i]) * 16777619u;
		  debugf("[AH] %u %u %08lx\n", Pico.m.frame_count, nsamples, (unsigned long)fh); }
#endif
	}
#endif
	/* Never wait on the audio DAC in benchmark mode: measure raw
	 * throughput even when emulation runs faster than real time. */
	if (devcfg.flags & DEV_BENCH) {
		if (free_bufs > 0)
			audio_push(snd_stereo, nsamples, false);
	} else
		audio_push(snd_stereo, nsamples, true);
}

#ifndef N64_PERF_HUD
/* CPU-path blit completion, from the RDP SYNC_FULL interrupt */
static volatile int blit_busy;
static void blit_done(void *fb)
{
	display_show((surface_t *)fb);
	blit_busy = 0;
}
#endif

#ifdef N64_BENCH
/* ---- Deterministic benchmark mode (make ... EXTRA_CFLAGS=-DN64_BENCH) ----
 *
 * Input is scripted from the EMULATED frame number, so every build sees
 * the same game frames, and the cost of the window [BENCH_START,BENCH_END)
 * is directly comparable between builds.  At BENCH_END it prints the N64
 * time per emulated frame (whole main loop: emulation + blit + audio +
 * input) and a hash of 68k RAM + VRAM: identical hashes mean an
 * optimisation did not change emulation results.  Run it headless with
 * platform/n64/tools/ares_profile.sh. */
#ifndef BENCH_PLAY
#define BENCH_PLAY   500    /* last Start tap at f480; gameplay begins ~f560 */
#endif
#ifndef BENCH_START
#define BENCH_START  900
#endif
#ifndef BENCH_END
#define BENCH_END    3300
#endif

uint32_t bench_audio_hash = 2166136261u; unsigned int bench_audio_n;
static char bench_res[9][44];   /* result screen lines (dev ROM) */
uint32_t bench_fb_hash = 2166136261u; unsigned int bench_fb_n;
unsigned int bench_z80_ticks, bench_z80_calls, bench_fmwait_ticks, bench_blitwait_ticks, bench_fmwait_skip;

static unsigned int bench_start = BENCH_START, bench_end = BENCH_END;

static unsigned int bench_pad(unsigned int f)
{
	unsigned int pad = 0;
	if (devcfg.input == 1)		/* no input: e.g. measure an attract-mode demo */
		return 0;
	if (f < BENCH_PLAY) {
		/* tap Start (and C/A to confirm menus) for 6 frames every 40 */
		if (f >= 120 && (f % 40) < 6)
			pad = (1 << GBTN_START) | (((f / 40) & 1) ? (1 << GBTN_C) : 0);
	} else {
		/* "play": autofire B, weave up/down, drift left/right */
		if ((f & 7) < 4) pad |= 1 << GBTN_B;
		if ((f % 240) < 90)        pad |= 1 << GBTN_UP;
		else if ((f % 240) >= 120 && (f % 240) < 210) pad |= 1 << GBTN_DOWN;
		if ((f % 600) < 150)       pad |= 1 << GBTN_RIGHT;
		else if ((f % 600) >= 300 && (f % 600) < 450) pad |= 1 << GBTN_LEFT;
	}
	return pad;
}

#ifdef N64_EMUX
int emux_in68k;
unsigned int emux_fame_calls, emux_fame_instrs;
#endif
#ifdef FAME_PCHIST
unsigned int m68k_pchist[0x44000];   /* 4-byte buckets: ROM 1MB, RAM 64KB */
static void m68k_pchist_dump(void)
{
	unsigned long long tot = 0;
	for (int i = 0; i < 0x44000; i++) tot += m68k_pchist[i];
	unsigned int thr = (unsigned int)(tot / 2000);
	debugf("[MHBEGIN] total=%llu thr=%u\n", tot, thr);
	for (int i = 0; i < 0x44000; i++)
		if (m68k_pchist[i] > thr)
			debugf("[MH] %06x %u\n", i < 0x40000 ? i * 4 : 0xff0000 + (i - 0x40000) * 4, m68k_pchist[i]);
	debugf("[MHEND]\n");
}
#endif

#ifdef Z80_PCHIST
unsigned int z80_pchist[0x2000];
static void z80_pchist_dump(void)
{
	unsigned int tot = 0;
	for (int i = 0; i < 0x2000; i++) tot += z80_pchist[i];
	debugf("[ZHBEGIN] total=%u\n", tot);
	for (int i = 0; i < 0x2000; i++)
		if (z80_pchist[i]) debugf("[ZH] %04x %u\n", i, z80_pchist[i]);
	debugf("[ZHEND]\n");
	for (int i = 0; i < 0x2000; i += 32) {
		char ln[80]; int n = 0;
		for (int j = 0; j < 32; j++) n += sprintf(ln + n, "%02x", PicoMem.zram[i + j]);
		debugf("[ZRAM] %04x %s\n", i, ln);
	}
}
#endif

#ifdef N64_PROF_SAMPLE
/* Statistical PC sampler: a continuous timer interrupt reads the PC the
 * main code was interrupted at (EPC, saved by libdragon's inthandler.S in
 * the frame interrupt_exception_frame points to) and bumps a 16-byte
 * bucket.  Dumped at BENCH_END as "[PS] addr count" lines; symbolize on
 * the host with platform/n64/tools/prof_report.sh. */
#include <timer.h>
extern char __text_start[], __text_end[];
extern void *interrupt_exception_frame;
#define PS_SHIFT 4
#define PS_EPC_OFF (32 + 32*8 + 8 + 8 + 4 + 4)   /* STACK_EPC in inthandler.S */
static uint32_t *ps_hist;
static unsigned int ps_nbuckets, ps_total, ps_outside;
static timer_link_t *ps_timer;

static void ps_tick(int ovfl)
{
	(void)ovfl;
	uint32_t epc = *(uint32_t *)((char *)interrupt_exception_frame + PS_EPC_OFF);
	uint32_t off = epc - (uint32_t)__text_start;
	ps_total++;
	if (off < (uint32_t)(__text_end - __text_start))
		ps_hist[off >> PS_SHIFT]++;
	else
		ps_outside++;
}

static void ps_start(void)
{
	ps_nbuckets = ((__text_end - __text_start) >> PS_SHIFT) + 1;
	ps_hist = calloc(ps_nbuckets, sizeof(uint32_t));
	timer_init();   /* refcounted: safe if already initialised */
	if (ps_hist)
		ps_timer = new_timer(TIMER_TICKS(213), TF_CONTINUOUS, ps_tick); /* ~4.7kHz */
}

static void ps_stop_and_dump(void)
{
	if (!ps_timer) return;
	delete_timer(ps_timer);
	ps_timer = NULL;
	debugf("[PSBEGIN] total=%u outside=%u text=%08lx shift=%d\n", ps_total, ps_outside,
	       (unsigned long)(uint32_t)__text_start, PS_SHIFT);
	for (unsigned int i = 0; i < ps_nbuckets; i++)
		if (ps_hist[i])
			debugf("[PS] %08lx %lu\n",
			       (unsigned long)((uint32_t)__text_start + (i << PS_SHIFT)),
			       (unsigned long)ps_hist[i]);
	debugf("[PSEND]\n");
}
#endif

static uint32_t bench_hash(const void *p, unsigned int len, uint32_t h)
{
	const uint8_t *b = p;
	for (unsigned int i = 0; i < len; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}
#endif

/* Frame skip: 0=none, 1=skip 1, 2=skip 2 */
static int frame_count = 0;
#define FRAME_SKIP 1

/* Precomputed BGR555 -> RGBA5551 lookup table (32768 entries = 64KB) */
static uint16_t bgr555_to_rgba5551[32768];

static void init_color_lut(void)
{
	for (int i = 0; i < 32768; i++) {
		uint16_t r = (i      ) & 0x1f;
		uint16_t g = (i >>  5) & 0x1f;
		uint16_t b = (i >> 10) & 0x1f;
		bgr555_to_rgba5551[i] = (r << 11) | (g << 6) | (b << 1) | 1;
	}
}

/* Fast blit using LUT - one table lookup per pixel, no math */
/* RGBA5551 palette cache for 8-bit alt renderer */
static uint16_t pal_rgba5551[256];

static void update_palette(void)
{
	unsigned short *src = Pico.est.HighPal;
	for (int i = 0; i < 256; i++) {
		uint16_t c = src[i];
		uint16_t r = (c      ) & 0x1f;
		uint16_t g = (c >>  5) & 0x1f;
		uint16_t b = (c >> 10) & 0x1f;
		pal_rgba5551[i] = (r << 11) | (g << 6) | (b << 1) | 1;
	}
}

static void blit_frame(surface_t *fb)
{
	uint16_t *dst = (uint16_t *)fb->buffer;
	int h = g_screen_height;
	int w = g_screen_width;
	int y_off = (240 - h) / 2;

	if (y_off > 0)
		memset(dst, 0, 320 * 240 * 2);

	/* Direct 8-bit HighCol -> RGBA5551 via palette.
	 * HighCol is the per-scanline 8-bit indexed buffer (328 bytes wide,
	 * 8-pixel left margin). Each byte is a palette index.
	 * We skip FinalizeLine entirely (saves 38% of VDP time).
	 * HighCol pointer advances per line during rendering, so we use
	 * the base from est and compute per-line offset. */
	/* BGR555 -> RGBA5551 via LUT */
	uint16_t *src = (uint16_t *)(void *)screen_buffer;
	for (int y = 0; y < h; y++) {
		uint16_t *s = &src[y * 320];
		uint16_t *d = &dst[(y + y_off) * 320];
		for (int x = 0; x < w; x += 4) {
			d[x]   = bgr555_to_rgba5551[s[x]   & 0x7fff];
			d[x+1] = bgr555_to_rgba5551[s[x+1] & 0x7fff];
			d[x+2] = bgr555_to_rgba5551[s[x+2] & 0x7fff];
			d[x+3] = bgr555_to_rgba5551[s[x+3] & 0x7fff];
		}
	}
}

/* writable copy of the embedded ROM, see main() */
static unsigned char n64_rom_buf[EMBEDDED_ROM_SIZE + 4] __attribute__((aligned(16)));

int main(int argc, char *argv[])
{
	unsigned char *rom_copy;

	g_argv = argv;
	g_screen_ptr = screen_buffer;

	debug_init_usblog();
	/* Also log to an IS-Viewer if one is present: emulators (ares prints
	 * it to its terminal) -- lets the profiler run headless.  Probes and
	 * does nothing if there's no IS-Viewer. */
	debug_init_isviewer();
	devcfg_load();
	if (devcfg.flags)
		debugf("[DEVCFG] flags=%lx trace=%lu+%lu input=%lu bench=%lu-%lu\n", (unsigned long)devcfg.flags,
		       (unsigned long)devcfg.trace_start, (unsigned long)devcfg.trace_frames,
		       (unsigned long)devcfg.input, (unsigned long)devcfg.bench_start, (unsigned long)devcfg.bench_end);
#ifdef N64_BENCH
	if (devcfg.bench_start && devcfg.bench_end > devcfg.bench_start) {
		bench_start = devcfg.bench_start;
		bench_end = devcfg.bench_end;
	}
#endif
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
	rdpq_init();
	audio_init(SND_RATE, 4);
	rsp_audio_init();  /* Register RSP audio overlay with rspq */
	rsp_fm_init();     /* Register RSP FM synthesis overlay */
#ifdef N64_RSP_VDP
	rsp_vdp_init();    /* RSP tile renderer, fed by draw2 */
	{ extern int draw2_rsp; draw2_rsp = 1; }
#endif
	joypad_init();

	/* Boot message */
	console_init();
	console_set_render_mode(RENDER_MANUAL);
	printf("\n\n  PicoDrive64\n\n");
	printf("  ROM: %s (%d KB)\n", EMBEDDED_ROM_NAME, EMBEDDED_ROM_SIZE / 1024);
	printf("  RAM: %d KB\n", get_memory_size() / 1024);
	printf("  Initializing...\n");
	console_render();

	PicoInit();
	init_color_lut();

	/* Enable sound: FM + Z80 + mono at 11025 Hz.
	 * FM uses decomposed 1KB tables (L1 cache friendly) instead
	 * of the default 208KB ym_tl_tab. */
	PicoIn.opt  = POPT_EN_FM | POPT_EN_Z80;
	PicoIn.opt |= POPT_DIS_VDP_FIFO;
	PicoIn.opt |= POPT_DIS_SPRITE_LIM;
	/* Enable 68K idle detection - skips cycles in idle loops */
	PicoIn.sndRate = SND_RATE;
	PicoIn.sndOut = snd_buffer;
	PicoIn.writeSound = write_sound;

	/* Writable ROM copy (PicoDrive patches idle loops into it).  A static
	 * .bss object rather than malloc: its D-cache position is then set by
	 * platform/n64/layout.ld like any other hot data, instead of following
	 * wherever the heap starts. */
	rom_copy = n64_rom_buf;
	memcpy(rom_copy, embedded_rom_data, EMBEDDED_ROM_SIZE);

	if (PicoCartInsert(rom_copy, EMBEDDED_ROM_SIZE, NULL)) {
		printf("  ERROR: PicoCartInsert failed!\n");
		console_render();
		for (;;) {}
	}

	PicoPower();
	PicoReset();
	PicoLoopPrepare();

	/* Alt renderer (draw2.c / PicoFrameFull) for ~15-20% perf win.
	 * Known to miss per-line / per-frame features that Gleylancer needs
	 * - capturing screenshots now to diagnose which ones before we
	 *   selectively add them back to draw2.c.
	 *
	 * Important: do NOT call PicoDrawSetOutBuf - we don't want the
	 * per-scanline renderer to also write to screen_buffer.  It would
	 * fight with draw2.c when VRAM changes mid-frame trigger
	 * PicoVideoSync (per-scanline + alt-renderer both writing to the
	 * same buffer at slightly different layouts = visible ghosting). */
	PicoDrawSetOutFormat(PDF_8BIT, 0);
	PicoDraw2SetOutBuf(screen_buffer, SCR_PITCH);
	PicoIn.opt |= POPT_ALT_RENDERER;

	printf("  Running!\n");
	console_render();
	for (volatile int i = 0; i < 2000000; i++) {}

	console_close();
	display_close();
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);

	/* FPS counter + profiling */
	int fps_count = 0;
	int fps_display = 0;
	unsigned int fps_timer = timer_ticks();
#ifdef N64_PERF_HUD
	char fps_buf[40];
#endif

	/* Detailed profiling accumulators (ticks per second, for on-screen) */
	unsigned int prof_68k_acc = 0, prof_vdp_acc = 0;
	unsigned int prof_snd_acc = 0;
	unsigned int prof_frame_acc = 0;
	int prof_68k_pct = 0, prof_vdp_pct = 0, prof_snd_pct = 0;

	/* Target frame time at 60 FPS for percentage calculations.
	 * timer_ticks runs at COUNTS_PER_SECOND, so one 60Hz frame is
	 * COUNTS_PER_SECOND/60 ticks. Values >100% mean the frame
	 * overran its budget (we can't hit 60 FPS). */
	const unsigned int target_frame_ticks = TIMER_TICKS(1000000 / 60);

	/* Frametime min/max tracking over the 1-second window. */
	unsigned int ft_min_us = 0, ft_max_us = 0;

	/* Main loop - pipelined: RDP blit runs in parallel with skip frame */
	surface_t *pending_fb = NULL;

	for (;;) {
		PicoIn.skipFrame = (frame_count < FRAME_SKIP) ? 1 : 0;
#ifndef N64_PERF_HUD
		/* The blit queued on the last rendered frame reads screen_buffer
		 * and the palette; don't draw into them until the RDP is done.
		 * (Practically never waits: a skip frame ran in between.) */
		if (!PicoIn.skipFrame && blit_busy) {
#ifdef N64_BENCH
			unsigned int _bt = timer_ticks();
			while (blit_busy) {}
			bench_blitwait_ticks += timer_ticks() - _bt;
#else
			while (blit_busy) {}
#endif
		}
#endif

		prof_68k_ticks = prof_vdp_ticks = 0;
		prof_vdp_layer_ticks = prof_vdp_sprite_ticks = prof_vdp_final_ticks = 0;
		unsigned int t0 = timer_ticks();

		/* Skip Z80 on skip frames to save ~10 FPS -- but only when no
		 * DAC voice is actively playing.  Many sound drivers keep
		 * dacen=1 permanently once any voice has played, so dacen is
		 * not a reliable signal.  Instead track actual 0x2a writes:
		 * if the game wrote DAC samples in the last few frames, voice
		 * is live and Z80 must run every frame.  sticky_dac decays to
		 * 0 a few frames after writes stop, at which point Z80 can
		 * skip again.  (Missing the very first frame of a new voice
		 * burst is inaudible — one frame @60fps is 16 ms.) */
		static unsigned int prev_dac_writes = 0;
		static unsigned int sticky_dac = 0;
		if (ym_dac_writes != prev_dac_writes) {
			sticky_dac = 4;        /* keep Z80 live for 4 more frames */
			prev_dac_writes = ym_dac_writes;
		} else if (sticky_dac) {
			sticky_dac--;
		}

		unsigned int saved_opt = PicoIn.opt;
		if (PicoIn.skipFrame && sticky_dac == 0)
			PicoIn.opt &= ~POPT_EN_Z80;

		/* RDP tile path: acquire + attach the display framebuffer
		 * BEFORE PicoFrame so PicoFrameFullRDP can issue rdpq
		 * commands into it.  The CPU path keeps the existing
		 * post-PicoFrame attach+blit flow below. */
		surface_t *rdp_fb = NULL;
		if (n64_use_rdp_tiles && !PicoIn.skipFrame) {
			/* Finish previous frame's pending blit first so the
			 * display_get() below can hand us a free buffer. */
			if (pending_fb) {
				rspq_wait();
#ifdef N64_PERF_HUD
				graphics_set_color(
					graphics_make_color(0xFF,0xFF,0xFF,0xFF),
					graphics_make_color(0,0,0,0xFF));
				sprintf(fps_buf, "e%d d%d 68k%d V%d S%d",
					fps_display, fps_display / (FRAME_SKIP + 1),
					prof_68k_pct, prof_vdp_pct, prof_snd_pct);
				graphics_draw_text(pending_fb, 4, 4, fps_buf);
#endif
				display_show(pending_fb);
				pending_fb = NULL;
			}
			rdp_fb = display_get();
			if (rdp_fb && rdp_fb->buffer) {
				if (Pico.m.dirtyPal) {
					PicoDrawUpdateHighPal();
					update_palette();
				}
				rdpq_attach(rdp_fb, NULL);
				rdpq_set_mode_standard();
				rdpq_mode_filter(FILTER_POINT);
				rdpq_mode_tlut(TLUT_RGBA16);
			} else {
				rdp_fb = NULL;
			}
		}

		PicoFrame();
		PicoIn.opt = saved_opt;

		/* If we attached for the RDP path, detach now and queue the
		 * fb for display on the next REND frame -- mirrors the CPU
		 * path's pending_fb pipeline so timing stays identical. */
		if (rdp_fb) {
			rdpq_detach();
			pending_fb = rdp_fb;
		}
		/* During skip frames, the RDP blit from the previous render
		 * frame runs in parallel with PicoFrame(). Skip frames don't
		 * touch screen_buffer (no VDP), so no data race. */
		unsigned int t1 = timer_ticks();
		fps_count++;

		/* Per-frame profile line: instantaneous FPS, frametime, + budget %.
		 * tot>100% on any category means that category alone cannot hit 60 FPS. */
		unsigned int now = t1;
		unsigned int frame_time = t1 - t0;
		unsigned int snd_time = 0;
		if (frame_time > prof_68k_ticks + prof_vdp_ticks)
			snd_time = frame_time - prof_68k_ticks - prof_vdp_ticks;

		/* frame_time_us for ms conversion and instantaneous FPS. */
		unsigned int frame_us = (unsigned int)TIMER_MICROS_LL(frame_time);
		unsigned int inst_fps_x10 = frame_us ? (unsigned int)(10000000ULL / frame_us) : 0;

		/* Percentages of 60Hz frame budget (x100 for 2-decimal print). */
		unsigned int cpu_pct100 = (uint64_t)prof_68k_ticks * 10000 / target_frame_ticks;
		unsigned int draw_pct100 = (uint64_t)prof_vdp_ticks * 10000 / target_frame_ticks;
		unsigned int snd_pct100 = (uint64_t)snd_time * 10000 / target_frame_ticks;
		unsigned int total_pct100 = (uint64_t)frame_time * 10000 / target_frame_ticks;

		/* VDP sub-breakdown: L (layers) / S (sprites) / F (finalize) as
		 * percentages of VDP total.  Useful to target specific hot paths. */
		unsigned int vt = prof_vdp_layer_ticks + prof_vdp_sprite_ticks
		                + prof_vdp_final_ticks;
		unsigned int lp = vt ? (unsigned)((uint64_t)prof_vdp_layer_ticks*100/vt) : 0;
		unsigned int sp = vt ? (unsigned)((uint64_t)prof_vdp_sprite_ticks*100/vt) : 0;
		unsigned int fp = vt ? (unsigned)((uint64_t)prof_vdp_final_ticks*100/vt) : 0;

		unsigned int ft_ms_x10 = frame_us / 100;
		/* kind: SKIP frames run only CPU+audio (no VDP). RENDER frames
		 * do the full scanline pipeline. They alternate with FRAME_SKIP=1. */
		const char *kind = PicoIn.skipFrame ? "SKIP" : "REND";

#ifdef N64_PERF_HUD
		debugf("[F] %s ft=%u.%ums cpu:%u vdp:%u[L%uS%uF%u] snd:%u tot:%u aud:%s PC:%06x\n",
			kind,
			ft_ms_x10/10, ft_ms_x10%10,
			cpu_pct100/100,
			draw_pct100/100, lp, sp, fp,
			snd_pct100/100,
			total_pct100/100,
			(snd_latest_fill == 0) ? "drain"
				: (snd_latest_fill >= AUDIO_NUM_BUFFERS) ? "full" : "ok",
			(unsigned)SekPc & 0xFFFFFF);
#else
		(void)kind; (void)lp; (void)sp; (void)fp; (void)cpu_pct100; (void)draw_pct100;
		(void)snd_pct100; (void)total_pct100; (void)ft_ms_x10;
#endif
		(void)inst_fps_x10;

		/* Accumulate for per-second summary. */
		prof_frame_acc += frame_time;
		prof_68k_acc += prof_68k_ticks;
		prof_vdp_acc += prof_vdp_ticks;
		prof_snd_acc += snd_time;
		if (frame_us > ft_max_us) ft_max_us = frame_us;
		if (frame_us < ft_min_us || ft_min_us == 0) ft_min_us = frame_us;

		if (TICKS_TO_MS(now - fps_timer) >= 1000) {
			fps_display = fps_count;
			fps_count = 0;
			if (prof_frame_acc > 0) {
				prof_68k_pct = (int)((uint64_t)prof_68k_acc * 100 / prof_frame_acc);
				prof_vdp_pct = (int)((uint64_t)prof_vdp_acc * 100 / prof_frame_acc);
				prof_snd_pct = (int)((uint64_t)prof_snd_acc * 100 / prof_frame_acc);
			}
			/* Fixed drift calc: signed subtraction (avoids unsigned
			 * underflow when we're slow, which was printing +3e7%). */
			int32_t expected = SND_RATE;
			int32_t actual = (int32_t)snd_total_samples;
			int drift_x100 = (int)((int64_t)(actual - expected) * 10000 / expected);

			unsigned int frames_safe = fps_display ? fps_display : 1;
			unsigned int ft_avg_us = (unsigned int)(TIMER_MICROS_LL(prof_frame_acc) / frames_safe);
			unsigned int avg_ms_x10 = ft_avg_us / 100;
			unsigned int min_ms_x10 = ft_min_us / 100;
			unsigned int max_ms_x10 = ft_max_us / 100;

			/* fps_display = PicoFrame calls/sec (emulator tick rate).
			 * Actual displayed frames/sec is lower because of FRAME_SKIP:
			 * we only blit every (FRAME_SKIP+1)th frame. With FRAME_SKIP=1,
			 * disp = emu/2.  emu_speed is emu rate as % of native 60Hz. */
			int disp_fps_x10 = (fps_display * 10) / (FRAME_SKIP + 1);
			int emu_speed_pct = fps_display * 100 / 60;

			debugf("[SEC] emu_fps=%d disp_fps=%d.%d speed=%d%% "
			       "ft_avg=%u.%ums ft_min=%u.%u ft_max=%u.%u "
			       "audio=%d/%u(%+d.%02d%%) blk=%u undr=%u\n",
				fps_display, disp_fps_x10/10, disp_fps_x10%10, emu_speed_pct,
				avg_ms_x10/10, avg_ms_x10%10,
				min_ms_x10/10, min_ms_x10%10,
				max_ms_x10/10, max_ms_x10%10,
				actual, (unsigned)expected,
				drift_x100/100, (drift_x100<0?-drift_x100:drift_x100)%100,
				snd_blocked_count, snd_underrun_count);

#ifdef DRC_68K
			{
				/* 68k DRC coverage, per-second deltas.  The number that
				 * matters is the cycle split, not call counts: DRC runs one
				 * block per call but a FAME fallback runs the rest of the
				 * slice.  "top" = opcodes (low 6 bits dropped) whose blocks
				 * were entered most often via FAME; FULL = compile failed
				 * because the block table / code cache was full. */
				static unsigned int p_cd, p_cf, p_bx, p_cl, p_cfail, p_full, p_short;
				static unsigned int p_hist[DRC68K_OPHIST_FULL + 1];
				unsigned int d_cd = drc68k.cyc_drc - p_cd;
				unsigned int d_cf = drc68k.cyc_fame - p_cf;
				unsigned int d_bx = drc68k.blocks_executed - p_bx;
				unsigned int d_cl = drc68k.calls_fame - p_cl;
				unsigned int d_fail = drc68k.compile_fail - p_cfail;
				unsigned int d_full = drc68k.fail_full - p_full;
				unsigned int d_short = drc68k.fail_short - p_short;
				p_cd = drc68k.cyc_drc;   p_cf = drc68k.cyc_fame;
				p_bx = drc68k.blocks_executed; p_cl = drc68k.calls_fame;
				p_cfail = drc68k.compile_fail; p_full = drc68k.fail_full;
				p_short = drc68k.fail_short;

				unsigned int tot_c = d_cd + d_cf;
				unsigned int drc_pct10 = tot_c ?
					(unsigned int)((uint64_t)d_cd * 1000 / tot_c) : 0;

				int top_i[3] = { -1, -1, -1 };
				unsigned int top_v[3] = { 0, 0, 0 };
				for (int i = 0; i <= DRC68K_OPHIST_FULL; i++) {
					unsigned int d = drc68k.op_hist[i] - p_hist[i];
					p_hist[i] = drc68k.op_hist[i];
					for (int k = 0; k < 3 && d; k++) {
						if (d > top_v[k]) {
							for (int m = 2; m > k; m--) {
								top_v[m] = top_v[m-1]; top_i[m] = top_i[m-1];
							}
							top_v[k] = d; top_i[k] = i;
							break;
						}
					}
				}
				char top[72];
				int n = 0;
				top[0] = 0;
				for (int k = 0; k < 3 && top_i[k] >= 0; k++) {
					if (top_i[k] == DRC68K_OPHIST_FULL)
						n += snprintf(top + n, sizeof(top) - n, " FULL:%u", top_v[k]);
					else
						n += snprintf(top + n, sizeof(top) - n, " %04x:%u",
						              top_i[k] << 6, top_v[k]);
				}

				debugf("[DRC] cyc drc=%u.%u%% (%u/%u) calls drc=%u fame=%u "
				       "blocks=%d/%d cache=%uKB cfail=%u full=%u short=%u top:%s\n",
					drc_pct10 / 10, drc_pct10 % 10, d_cd, d_cf,
					d_bx, d_cl,
					drc68k.block_count, DRC68K_BLOCK_MAX,
					(unsigned int)((drc68k.cache_ptr - drc68k.cache) / 1024),
					d_fail, d_full, d_short, top);
			}
#endif

			prof_68k_acc = prof_vdp_acc = prof_snd_acc = prof_frame_acc = 0;
			snd_total_samples = 0;
			snd_blocked_count = 0;
			snd_underrun_count = 0;
			ft_min_us = ft_max_us = 0;
			fps_timer = now;
		}

		if (++frame_count > FRAME_SKIP) {
			frame_count = 0;

			/* RDP tile path handles its own attach/detach/pending_fb
			 * above (before and after PicoFrame).  Skip the CPU CI8
			 * blit path entirely in that case. */
			if (n64_use_rdp_tiles)
				goto post_render;

#ifdef N64_PERF_HUD
			/* Finish previous RDP blit (ran during skip frame), draw the
			 * HUD text into it and show it.  rspq_wait() also waits for
			 * the FM job queued this frame; the non-HUD path avoids that. */
			if (pending_fb) {
				rspq_wait();
				graphics_set_color(
					graphics_make_color(0xFF,0xFF,0xFF,0xFF),
					graphics_make_color(0,0,0,0xFF));
				/* e%d = emulation fps (PicoFrame/sec), d%d = display fps. */
				sprintf(fps_buf, "e%d d%d 68k%d V%d S%d",
					fps_display, fps_display / (FRAME_SKIP + 1),
					prof_68k_pct, prof_vdp_pct, prof_snd_pct);
				graphics_draw_text(pending_fb, 4, 4, fps_buf);
				display_show(pending_fb);
				pending_fb = NULL;
			}
#endif

			/* Start new RDP blit (non-blocking) */
			surface_t *fb = display_get();
			if (fb && fb->buffer) {
				int h = g_screen_height;
				int w = g_screen_width;
				int y_off = (240 - h) / 2;

				if (Pico.m.dirtyPal) {
					PicoDrawUpdateHighPal();
					update_palette();
				}

#ifdef BENCH_FBHASH
				{   /* verification builds: hash every rendered frame's visible pixels */
					extern uint32_t bench_fb_hash; extern unsigned int bench_fb_n;
#ifdef N64_RSP_VDP
					rsp_vdp_wait();
					data_cache_hit_writeback_invalidate(screen_buffer + 8*SCR_PITCH, 224*SCR_PITCH);
#endif
					const uint8_t *row = screen_buffer + 8*SCR_PITCH + 8;
#ifdef FBHASH_LOG
					uint32_t fh = 2166136261u;
					const uint8_t *r2 = row;
					for (int yy = 0; yy < h; yy++, r2 += SCR_PITCH)
						for (int xx = 0; xx < w; xx++)
							fh = (fh ^ r2[xx]) * 16777619u;
					debugf("[FH] %u %08lx\n", Pico.m.frame_count, (unsigned long)fh);
#endif
					for (int yy = 0; yy < h; yy++, row += SCR_PITCH)
						for (int xx = 0; xx < w; xx++)
							bench_fb_hash = (bench_fb_hash ^ row[xx]) * 16777619u;
					bench_fb_n++;
				}
#endif
				/* the RDP reads rows 8..8+h: write back what the CPU drew
				 * (with the RSP renderer usually nothing: it DMAs rows) */
				unsigned wb_rows = 8 + h < 240 ? 8 + h : 240;
				unsigned wb_bytes = (SCR_PITCH * wb_rows + 15) & ~15;
#ifdef N64_RSP_VDP
				{ extern int draw2_fb_dirty;
				if (draw2_fb_dirty) {
					data_cache_hit_writeback(screen_buffer, wb_bytes);
					draw2_fb_dirty = 0;
				} }
#else
				data_cache_hit_writeback(screen_buffer, wb_bytes);
#endif
				data_cache_hit_writeback(pal_rgba5551, sizeof(pal_rgba5551));

				/* draw2.c (alt renderer) writes into buffer starting at
				 * row 8 (it reserves an 8-row top margin that BackFillFull
				 * skips past).  Both BackFillFull and DrawSpriteFull assume
				 * this layout.  Read RDP source from that offset so visible
				 * content lands correctly.
				 *   +8 bytes     = skip HighCol left margin (8 pixels)
				 *   +8*SCR_PITCH = skip draw2 top margin (8 rows) */
				surface_t ci8_surf = surface_make(
					screen_buffer + 8*SCR_PITCH + 8,
					FMT_CI8, w, h, SCR_PITCH);

				/* The blit is the same every frame (same source, palette
				 * and geometry), so it is recorded once as an rspq block
				 * and each frame queues a single call.  Written out every
				 * frame, its ~700 words of rdpq commands overflowed rspq's
				 * 2 x 2KB command ring while the RSP was still busy with
				 * this frame's tile render, and the CPU sat waiting for
				 * the render to finish. */
				static rspq_block_t *blit_block;
				static int blit_w, blit_h, blit_y;
				if (!blit_block || w != blit_w || h != blit_h || y_off != blit_y) {
					if (blit_block) {
						rspq_wait();   /* not in use any more */
						rspq_block_free(blit_block);
					}
					rspq_block_begin();
					rdpq_set_mode_standard();
					rdpq_mode_filter(FILTER_POINT);
					rdpq_mode_tlut(TLUT_RGBA16);
					rdpq_tex_upload_tlut(pal_rgba5551, 0, 256);
					rdpq_tex_blit(&ci8_surf, 0, y_off, NULL);
					blit_block = rspq_block_end();
					blit_w = w; blit_h = h; blit_y = y_off;
				}
				rdpq_attach(fb, NULL);
				rspq_block_run(blit_block);

#ifdef RDP_TILES_DEMO
				{
					/* Proof-of-concept: draw Genesis tile 0 at
					 * (160,120) via CI4 TLUT through the RDP.
					 * Validates tile+TLUT plumbing on hardware
					 * before we rewire the main loop. */
					static uint16_t demo_tlut[64] __attribute__((aligned(8)));
					extern void rdp_tiles_build_tlut(uint16_t *);
					extern void rdp_tile_draw_demo(unsigned, int, int, int, const uint16_t *);
					rdp_tiles_build_tlut(demo_tlut);
					data_cache_hit_writeback(demo_tlut, sizeof(demo_tlut));
					rdp_tile_draw_demo(0, 160, 120, 0, demo_tlut);
				}
#endif

#ifdef N64_PERF_HUD
				rdpq_detach();  /* Non-blocking! RDP runs during next skip frame */
				pending_fb = fb;
#else
				/* Non-blocking: the RDP interrupt shows the frame when the
				 * blit is done (display_get() has 3 buffers to hand out). */
				blit_busy = 1;
				rdpq_detach_cb(blit_done, fb);
#endif
			}
post_render: ;
		}

		joypad_poll();
		joypad_buttons_t btns = joypad_get_buttons(JOYPAD_PORT_1);
		joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);
		unsigned int pad = 0;

		if (btns.d_up || inputs.stick_y > 20)   pad |= 1 << GBTN_UP;
		if (btns.d_down || inputs.stick_y < -20) pad |= 1 << GBTN_DOWN;
		if (btns.d_left || inputs.stick_x < -20) pad |= 1 << GBTN_LEFT;
		if (btns.d_right || inputs.stick_x > 20) pad |= 1 << GBTN_RIGHT;
		if (btns.a)     pad |= 1 << GBTN_B;
		if (btns.b)     pad |= 1 << GBTN_C;
		if (btns.z)     pad |= 1 << GBTN_A;
		if (btns.start) pad |= 1 << GBTN_START;
		if (btns.l)     pad |= 1 << GBTN_X;
		if (btns.r)     pad |= 1 << GBTN_Z;
		if (btns.c_right) pad |= 1 << GBTN_Y;

		PicoIn.pad[0] = pad;

#ifdef N64_BENCH
		if (devcfg.flags & DEV_BENCH) {
			static unsigned int b_last, b_frames;
			static uint64_t b_ticks, b_68k, b_vdp;
			static unsigned int z0t, z0c, fw0, bw0, fs0;
			unsigned int f = Pico.m.frame_count;
			unsigned int tnow = timer_ticks();

			PicoIn.pad[0] = bench_pad(f);
			if ((f % 60) == 0)
				debugf("[BF] f=%u\n", f);
			if (f > bench_start && f <= bench_end) {
				b_ticks += tnow - b_last;
				b_68k += prof_68k_ticks;
				b_vdp += prof_vdp_ticks;
				b_frames++;
			}
			b_last = tnow;
#ifdef N64_PROF_SAMPLE
			if (f == bench_start)
				ps_start();
#endif
			if (devcfg.flags & DEV_TRACE) {
				if (f == devcfg.trace_start) dev_xprof(8);
				if (f == devcfg.trace_start + devcfg.trace_frames) { dev_xprof(9); debugf("[TRACE] done\n"); }
			}
			if (devcfg.flags & DEV_MISSHIST) {
				if (f == devcfg.trace_start) dev_xprof(6);
				if (f == devcfg.trace_start + devcfg.trace_frames) { dev_xprof(7); debugf("[MISSHIST] dumped\n"); }
			}
#ifdef N64_EMUX
			if (f == bench_start) {
				/* sub-slots ran since boot: zero them (none is running
				 * between frames) so they cover the same window as slot 0 */
				extern unsigned int emux_fame_calls;
				for (int k = 1; k < 10; k++) emux_clear(k);
				emux_fame_calls = 0; { extern unsigned int emux_fame_instrs; emux_fame_instrs = 0; }
				EMUX_START(0);
			}
			if (f == bench_end) {
				EMUX_STOP(0);
				static const char *nm[10] = { "all", "68k", "z80", "vdp", "snd",
					"fame", "vdpw", "vdpr", "dma", "z80in68k" };
				extern unsigned int emux_fame_calls;
				{ extern unsigned int emux_fame_instrs;
				debugf("[EMUX] fame calls/frame=%u instrs/frame=%u\n", emux_fame_calls / (bench_end - bench_start), emux_fame_instrs / (bench_end - bench_start)); }
				for (int k = 0; k < 10; k++)
					debugf("[EMUX] %-3s cyc/frame=%lu  I$miss/frame=%lu  D$miss/frame=%lu  D$wb/frame=%lu  I$hit/frame=%lu\n",
						nm[k],
						(unsigned long)(emux_profread(k, 0x0000) / (bench_end - bench_start)),
						(unsigned long)(emux_profread(k, 0x0011) / (bench_end - bench_start)),
						(unsigned long)(emux_profread(k, 0x0021) / (bench_end - bench_start)),
						(unsigned long)(emux_profread(k, 0x0022) / (bench_end - bench_start)),
						(unsigned long)(emux_profread(k, 0x0010) / (bench_end - bench_start)));
			}
#endif
			if (f == bench_start) {
				z0t = bench_z80_ticks; z0c = bench_z80_calls;
				fw0 = bench_fmwait_ticks; bw0 = bench_blitwait_ticks; fs0 = bench_fmwait_skip;
			}
#ifdef Z80_PCHIST
			if (f == bench_start)
				memset(z80_pchist, 0, sizeof(z80_pchist));
#endif
#ifdef FAME_PCHIST
			if (f == bench_start)
				memset(m68k_pchist, 0, sizeof(m68k_pchist));
#endif
#ifdef BENCH_EG_TRACE
			if (f >= bench_start && f <= bench_end) {
				uint32_t eh = 2166136261u;
				for (int c = 0; c < 6; c++) {
					for (int k = 0; k < 4; k++) {
						FM_SLOT *sl = &ym2612.CH[c].SLOT[k];
						uint32_t v[3] = { (uint32_t)sl->volume, sl->state, sl->vol_out };
						eh = bench_hash(v, sizeof(v), eh);
					}
					uint32_t u = ym2612.CH[c].upd_cnt & 3;
					eh = bench_hash(&u, 4, eh);
				}
				uint32_t g[2] = { ym2612.OPN.eg_cnt, ym2612.OPN.eg_timer };
				eh = bench_hash(g, sizeof(g), eh);
				debugf("[EG] %u %08lx ssg=%08lx fl=%x slots=%06lx\n", f, (unsigned long)eh, (unsigned long)ym2612.ssg_mask, ym2612.OPN.ST.flags, (unsigned long)ym2612.slot_mask);
			}
#endif
			if (f == bench_end || f == bench_end + 1)
				debugf("[AUDIO] f=%u samples=%u hash=%08lx fbframes=%u fbhash=%08lx\n", f, bench_audio_n,
				       (unsigned long)bench_audio_hash, bench_fb_n, (unsigned long)bench_fb_hash);
			if (f == bench_end) {
#ifdef N64_PROF_SAMPLE
				ps_stop_and_dump();
#endif
#ifdef Z80_PCHIST
				z80_pchist_dump();
#endif
#ifdef FAME_PCHIST
				m68k_pchist_dump();
#endif
				{   /* Z80 + YM timer state, to verify Z80-side changes exactly */
					uint32_t zh = bench_hash(PicoMem.zram, 0x2000, 2166136261u);
					for (int r = CZ80_PC; r <= CZ80_IRQ; r++) {
						uint32_t v = Cz80_Get_Reg(&CZ80, r);
						zh = bench_hash(&v, 4, zh);
					}
					int t[] = { Pico.t.z80c_cnt, Pico.t.z80_busdelay, Pico.t.z80_buscycles,
						Pico.t.timer_a_next_oflow, Pico.t.timer_b_next_oflow, Pico.t.ym2612_busy,
						ym2612.OPN.ST.status, ym2612.dacout };
					zh = bench_hash(t, sizeof(t), zh);
					debugf("[ZSTATE] zhash=%08lx\n", (unsigned long)zh);
					snprintf(bench_res[3], sizeof(bench_res[3]), "zhash   %08lx", (unsigned long)zh);
#ifdef N64_RSP_VDP
					{ extern unsigned int rsp_vdp_stat[4], draw2_rsp_ovf, draw2_rsp_maxlist; debugf("[RSPVDP] renders=%u blocked=%u waited=%lu us total, copy waits %lu us, cpu frames %u, longest list %u\n", rsp_vdp_stat[2], rsp_vdp_stat[0], (unsigned long)TIMER_MICROS_LL(rsp_vdp_stat[1]), (unsigned long)TIMER_MICROS_LL(rsp_vdp_stat[3]), draw2_rsp_ovf, draw2_rsp_maxlist); }
#endif
				}
				uint32_t h = bench_hash(PicoMem.ram, 0x10000, 2166136261u);
				h = bench_hash(PicoMem.vram, 0x10000, h);
				unsigned int us = (unsigned int)(TIMER_MICROS_LL(b_ticks) / b_frames);
				debugf("[BENCH] frames=%u us/frame=%u (%u.%u%% speed) "
				       "68k=%u vdp=%u other=%u us/frame  ramhash=%08lx\n",
					b_frames, us,
					(16666670u / us) / 10,
					(16666670u / us) % 10,
					(unsigned int)(TIMER_MICROS_LL(b_68k) / b_frames),
					(unsigned int)(TIMER_MICROS_LL(b_vdp) / b_frames),
					us - (unsigned int)(TIMER_MICROS_LL(b_68k + b_vdp) / b_frames),
					(unsigned long)h);
				debugf("[BENCH2] z80=%u us/frame (%u calls/frame) fmwait=%u (on skip frames %u) blitwait=%u us/frame\n",
					(unsigned int)(TIMER_MICROS_LL(bench_z80_ticks - z0t) / b_frames),
					(bench_z80_calls - z0c) / b_frames,
					(unsigned int)(TIMER_MICROS_LL(bench_fmwait_ticks - fw0) / b_frames),
					(unsigned int)(TIMER_MICROS_LL(bench_fmwait_skip - fs0) / b_frames),
					(unsigned int)(TIMER_MICROS_LL(bench_blitwait_ticks - bw0) / b_frames));
				snprintf(bench_res[0], sizeof(bench_res[0]), "frames %u-%u  %s", bench_start, bench_end, EMBEDDED_ROM_NAME);
				snprintf(bench_res[1], sizeof(bench_res[1]), "us/frame %u  speed %u.%u%%", us,
					(16666670u / us) / 10, (16666670u / us) % 10);
				snprintf(bench_res[2], sizeof(bench_res[2]), "ramhash %08lx", (unsigned long)h);
				snprintf(bench_res[4], sizeof(bench_res[4]), "audio   %08lx (%u smp)", (unsigned long)bench_audio_hash, bench_audio_n);
#ifdef N64_RSP_VDP
				{ extern unsigned int rsp_vdp_stat[4], draw2_rsp_ovf, draw2_rsp_maxlist;
				snprintf(bench_res[5], sizeof(bench_res[5]), "rsp renders %u blocked %u", rsp_vdp_stat[2], rsp_vdp_stat[0]);
				snprintf(bench_res[6], sizeof(bench_res[6]), "rsp waited %lu us copy %lu us",
					(unsigned long)TIMER_MICROS_LL(rsp_vdp_stat[1]), (unsigned long)TIMER_MICROS_LL(rsp_vdp_stat[3]));
				snprintf(bench_res[7], sizeof(bench_res[7]), "cpu frames %u  longest list %u", draw2_rsp_ovf, draw2_rsp_maxlist); }
#endif
				snprintf(bench_res[8], sizeof(bench_res[8]), "blitwait %u us/frame",
					(unsigned int)(TIMER_MICROS_LL(bench_blitwait_ticks - bw0) / b_frames));
			}
			if (f == bench_end + 1) {
				/* dev ROM: the run is over.  Leave the results on screen
				 * (for a photo from real hardware) and stop. */
				rspq_wait();
				for (;;) {
					surface_t *d = display_get();
					graphics_fill_screen(d, 0);
					graphics_draw_text(d, 16, 24, "PicoDrive64 bench  rev" REVISION);
					for (int i = 0; i < 9; i++)
						graphics_draw_text(d, 16, 48 + 16 * i, bench_res[i]);
					display_show(d);
				}
			}
		}
#endif
	}
}

/* Platform stubs */
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count)
{
	g_screen_width = col_count;
	g_screen_height = line_count;
}

void emu_32x_startup(void) { }
void lprintf(const char *fmt, ...) { }
#include <stdarg.h>
/* debugf for files built with NDEBUG (core), e.g. verification output */
void n64_logf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	debugf("%s", buf);
}

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{ return calloc(1, size); }
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{ return realloc(ptr, newsize); }
void plat_munmap(void *ptr, size_t size)
{ free(ptr); }
void *plat_mem_get_for_drc(size_t size) { return NULL; }
int plat_mem_set_exec(void *ptr, size_t size) { return 0; }

void cache_flush_d_inval_i(void *start, void *end)
{
	size_t len = (char *)end - (char *)start;
	if (len > 0) {
		data_cache_hit_writeback(start, len);
		inst_cache_hit_invalidate(start, len);
	}
}

int  mp3_get_bitrate(void *f, int size) { return 0; }
void mp3_start_play(void *f, int pos) { }
void mp3_update(s32 *buffer, int length, int stereo) { }
