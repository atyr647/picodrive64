/*
 * ares "emux" emulator extensions (Homebrew mode only: ares
 * --setting Developer/HomebrewMode=true).  On hardware these encodings are
 * reserved COP0 instructions, so only build them into -DN64_EMUX profiling
 * ROMs.  Encoding (ares cpu/interpreter.cpp): COP0 CO function, rd field at
 * bits 20-24, rt at 15-19, code at 6-14.
 *
 *   emux_prof(slot, 1/2)   start/stop accumulating into profile slot
 *   emux_clear(slot)       zero a (stopped) slot
 *   emux_profread(slot, k) read a counter of a slot:
 *       0x0000 CPU cycles   0x0010 I$ hits   0x0011 I$ misses
 *       0x0020 D$ hits      0x0021 D$ misses 0x0022 D$ writebacks
 */
#ifndef N64_EMUX_H
#define N64_EMUX_H
#ifdef N64_EMUX
static inline void emux_prof(unsigned int slot, int code_is_start)
{
	register unsigned int a0 __asm__("$4") = slot;
	if (code_is_start)
		__asm__ volatile(".word 0x42400068" :: "r"(a0) : "memory"); /* XPROF $4, 1 */
	else
		__asm__ volatile(".word 0x424000a8" :: "r"(a0) : "memory"); /* XPROF $4, 2 */
}
static inline unsigned long long emux_profread(unsigned int slot, unsigned int what)
{
	register unsigned int a0 __asm__("$4") = slot;
	register unsigned long long v0 __asm__("$2") = what;
	__asm__ volatile(".word 0x42410029" : "+r"(v0) : "r"(a0) : "memory"); /* XPROFREAD $4,$2 */
	return v0;
}
static inline void emux_clear(unsigned int slot)
{
	register unsigned int a0 __asm__("$4") = slot;
	__asm__ volatile(".word 0x424000e8" :: "r"(a0) : "memory"); /* XPROF $4, 3 */
}
#endif
#ifdef N64_EMUX
#define EMUX_START(s) emux_prof((s), 1)
#define EMUX_STOP(s)  emux_prof((s), 0)
#else
#define EMUX_START(s)
#define EMUX_STOP(s)
#endif
#endif
