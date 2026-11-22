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
	{
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
#ifdef N64_BENCH
	/* Never wait on the audio DAC in benchmark mode: measure raw
	 * throughput even when emulation runs faster than real time. */
	if (free_bufs > 0)
		audio_push(snd_stereo, nsamples, false);
#else
	audio_push(snd_stereo, nsamples, true);
#endif
}

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
uint32_t bench_fb_hash = 2166136261u; unsigned int bench_fb_n;
unsigned int bench_z80_ticks, bench_z80_calls, bench_fmwait_ticks, bench_blitwait_ticks, bench_fmwait_skip;

static unsigned int bench_pad(unsigned int f)
{
	unsigned int pad = 0;
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
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
	rdpq_init();
	audio_init(SND_RATE, 4);
	rsp_audio_init();  /* Register RSP audio overlay with rspq */
	rsp_fm_init();     /* Register RSP FM synthesis overlay */
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

	rom_copy = (unsigned char *)malloc(EMBEDDED_ROM_SIZE + 4);
	if (!rom_copy) {
		printf("  ERROR: Out of memory!\n");
		console_render();
		for (;;) {}
	}
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

			/* Finish previous RDP blit (ran during skip frame) */
			if (pending_fb) {
#ifdef N64_BENCH
				{ unsigned int _bt = timer_ticks(); rspq_wait();
				  bench_blitwait_ticks += timer_ticks() - _bt; }
#else
				rspq_wait();
#endif
#ifdef N64_PERF_HUD
				graphics_set_color(
					graphics_make_color(0xFF,0xFF,0xFF,0xFF),
					graphics_make_color(0,0,0,0xFF));
				/* e%d = emulation fps (PicoFrame/sec), d%d = display fps. */
				sprintf(fps_buf, "e%d d%d 68k%d V%d S%d",
					fps_display, fps_display / (FRAME_SKIP + 1),
					prof_68k_pct, prof_vdp_pct, prof_snd_pct);
				graphics_draw_text(pending_fb, 4, 4, fps_buf);
#endif
				display_show(pending_fb);
				pending_fb = NULL;
			}

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
					const uint8_t *row = screen_buffer + 8*SCR_PITCH + 8;
					for (int yy = 0; yy < h; yy++, row += SCR_PITCH)
						for (int xx = 0; xx < w; xx++)
							bench_fb_hash = (bench_fb_hash ^ row[xx]) * 16777619u;
					bench_fb_n++;
				}
#endif
				unsigned wb_bytes = (SCR_PITCH * h + 15) & ~15;
				data_cache_hit_writeback(screen_buffer, wb_bytes);
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

				rdpq_attach(fb, NULL);
				rdpq_set_mode_standard();
				rdpq_mode_filter(FILTER_POINT);
				rdpq_mode_tlut(TLUT_RGBA16);
				rdpq_tex_upload_tlut(pal_rgba5551, 0, 256);
				rdpq_tex_blit(&ci8_surf, 0, y_off, NULL);

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

				rdpq_detach();  /* Non-blocking! RDP runs during next skip frame */
				pending_fb = fb;
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
		{
			static unsigned int b_last, b_frames;
			static uint64_t b_ticks, b_68k, b_vdp;
			static unsigned int z0t, z0c, fw0, bw0, fs0;
			unsigned int f = Pico.m.frame_count;
			unsigned int tnow = timer_ticks();

			PicoIn.pad[0] = bench_pad(f);
			if ((f % 60) == 0)
				debugf("[BF] f=%u\n", f);
			if (f > BENCH_START && f <= BENCH_END) {
				b_ticks += tnow - b_last;
				b_68k += prof_68k_ticks;
				b_vdp += prof_vdp_ticks;
				b_frames++;
			}
			b_last = tnow;
#ifdef N64_PROF_SAMPLE
			if (f == BENCH_START)
				ps_start();
#endif
#ifdef N64_EMUX
			if (f == BENCH_START) EMUX_START(0);
			if (f == BENCH_END) {
				EMUX_STOP(0);
				static const char *nm[5] = { "all", "68k", "z80", "vdp", "snd" };
				for (int k = 0; k < 5; k++)
					debugf("[EMUX] %-3s cyc/frame=%lu  I$miss/frame=%lu  D$miss/frame=%lu  D$wb/frame=%lu  I$hit/frame=%lu\n",
						nm[k],
						(unsigned long)(emux_profread(k, 0x0000) / (BENCH_END - BENCH_START)),
						(unsigned long)(emux_profread(k, 0x0011) / (BENCH_END - BENCH_START)),
						(unsigned long)(emux_profread(k, 0x0021) / (BENCH_END - BENCH_START)),
						(unsigned long)(emux_profread(k, 0x0022) / (BENCH_END - BENCH_START)),
						(unsigned long)(emux_profread(k, 0x0010) / (BENCH_END - BENCH_START)));
			}
#endif
			if (f == BENCH_START) {
				z0t = bench_z80_ticks; z0c = bench_z80_calls;
				fw0 = bench_fmwait_ticks; bw0 = bench_blitwait_ticks; fs0 = bench_fmwait_skip;
			}
#ifdef Z80_PCHIST
			if (f == BENCH_START)
				memset(z80_pchist, 0, sizeof(z80_pchist));
#endif
			if (f == BENCH_END || f == BENCH_END + 1)
				debugf("[AUDIO] f=%u samples=%u hash=%08lx fbframes=%u fbhash=%08lx\n", f, bench_audio_n,
				       (unsigned long)bench_audio_hash, bench_fb_n, (unsigned long)bench_fb_hash);
			if (f == BENCH_END) {
#ifdef N64_PROF_SAMPLE
				ps_stop_and_dump();
#endif
#ifdef Z80_PCHIST
				z80_pchist_dump();
#endif
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
