/* dcolor: optimize D-cache placement ("colors", 16B line index mod 8KB) of hot
 * data objects against ares data-access traces (8KB direct-mapped, 16B lines,
 * write-back).  Cost = misses + writebacks.
 *   dcolor OBJS ITERS SEED OUT.colors TRACE.d...        (ITERS=0: report only)
 * OBJS: "addr size type name" (from nm -S) of the traced build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct { uint32_t addr, size; char type; char name[128]; } obj_t;
static obj_t *ob; static int nob;
static uint32_t *acc_obj, *acc_rel; static long nacc;	/* rel: byte offset from obj start (line start), or abs addr */

static int find_obj(uint32_t a)
{
	int lo = 0, hi = nob - 1, k = -1;
	while (lo <= hi) { int m = (lo + hi) / 2; if (ob[m].addr <= a) { k = m; lo = m + 1; } else hi = m - 1; }
	if (k >= 0 && a < ob[k].addr + ob[k].size) return k;
	/* line may start before the object: check next object */
	
	return -1;
}

static uint32_t tags[512]; static uint8_t dirty[512];
static uint32_t *base;	/* new base per object (with original sub-16 offset) */
static void sim(long *miss, long *wb, int color_model)
{
	long m = 0, w = 0;
	memset(tags, 0xff, sizeof tags); memset(dirty, 0, sizeof dirty);
	for (long i = 0; i < nacc; i++) {
		uint32_t o = acc_obj[i] & 0x7fffffff, wr = acc_obj[i] >> 31, line, tag;
		if (o == 0x7fffffff) { line = acc_rel[i] >> 4; tag = line; }
		else { line = (base[o] + acc_rel[i]) >> 4; tag = color_model ? ((line & 0x7fffff) | ((o + 1) << 23)) : line; }
		uint32_t set = line & 511;
		if (tags[set] != tag) {
			if (dirty[set]) w++;
			m++; tags[set] = tag; dirty[set] = 0;
		}
		if (wr) dirty[set] = 1;
	}
	*miss = m; *wb = w;
}

int main(int argc, char **argv)
{
	if (argc < 6) { fprintf(stderr, "usage: dcolor OBJS ITERS SEED OUT TRACE...\n"); return 1; }
	FILE *f = fopen(argv[1], "r"); char ln[512]; int cap = 2048; ob = malloc(cap * sizeof(obj_t));
	while (fgets(ln, sizeof ln, f)) {
		obj_t o; char t[8];
		if (sscanf(ln, "%x %u %7s %127s", &o.addr, &o.size, t, o.name) != 4) continue;
		o.type = t[0]; o.addr &= 0x1fffffff;
		if (nob == cap) { cap *= 2; ob = realloc(ob, cap * sizeof(obj_t)); }
		ob[nob++] = o;
	}
	fclose(f);
	double *heat = calloc(nob, sizeof(double));
	long capa = 1 << 24; acc_obj = malloc(capa * 4); acc_rel = malloc(capa * 4);
	for (int t = 5; t < argc; t++) {
		f = fopen(argv[t], "rb"); uint32_t buf[65536]; size_t n;
		while ((n = fread(buf, 4, 65536, f)) > 0) {
			for (size_t j = 0; j < n; j++) {
				uint32_t v = buf[j], wr = v >> 31, a = v & 0x7fffffff;
				if (nacc == capa) { capa *= 2; acc_obj = realloc(acc_obj, capa * 4); acc_rel = realloc(acc_rel, capa * 4); }
				int k = find_obj(a);
				/* only objects > 8 bytes outside small data are movable */
				if (k <= -2) k = -2 - k;
				if (k >= 0 && ob[k].size > 8 && strchr("bBdDrR", ob[k].type)) {
					acc_obj[nacc] = k | (wr << 31); acc_rel[nacc] = a - ob[k].addr; heat[k] += 1;
				} else {
					acc_obj[nacc] = 0x7fffffff | (wr << 31); acc_rel[nacc] = a;
				}
				nacc++;
			}
		}
		fclose(f);
	}
	base = malloc(nob * 4);
	for (int k = 0; k < nob; k++) base[k] = ob[k].addr;
	long m, w; sim(&m, &w, 0);
	long orig = m + w;
	fprintf(stderr, "accesses %ld, original: misses %ld writebacks %ld cost %ld\n", nacc, m, w, orig);
	long iters = atol(argv[2]); srand(atoi(argv[3]));
	if (iters < 0) {	/* evaluate the real layout given by OUT (= nm objs of a new link) */
		FILE *nf = fopen(argv[4], "r"); int found = 0;
		while (fgets(ln, sizeof ln, nf)) {
			unsigned a, sz; char t[8], nm[128];
			if (sscanf(ln, "%x %u %7s %127s", &a, &sz, t, nm) != 4) continue;
			for (int k = 0; k < nob; k++)
				if (heat[k] > 0 && !strcmp(ob[k].name, nm) && ob[k].size == sz) { base[k] = a & 0x1fffffff; found++; break; }
		}
		fclose(nf);
		sim(&m, &w, 0);
		printf("new layout: misses %ld writebacks %ld cost %ld vs original %ld (%+.1f%%), %d objects matched\n",
		       m, w, m + w, orig, 100.0 * (m + w - orig) / orig, found);
		return 0;
	}
	int *hot = malloc(nob * sizeof(int)), nh = 0; double htot = 0;
	for (int k = 0; k < nob; k++) if (heat[k] > 0) { hot[nh++] = k; htot += sqrt(heat[k]); }
	uint32_t *color = malloc(nob * 4);
	for (int k = 0; k < nob; k++) { color[k] = (ob[k].addr >> 4) & 511; base[k] = color[k] * 16 + (ob[k].addr & 15); }
	sim(&m, &w, 1); long cur = m + w, best = cur;
	fprintf(stderr, "hot movable objects %d; color model of original: %ld\n", nh, cur);
	uint32_t *bestc = malloc(nob * 4); memcpy(bestc, color, nob * 4);
	double T0 = cur * 0.002, T1 = 0.5;
	for (long it = 0; it < iters; it++) {
		double T = T0 * pow(T1 / T0, (double)it / iters);
		double r = (double)rand() / RAND_MAX * htot; int a = hot[nh - 1];
		for (int j = 0; j < nh; j++) { r -= sqrt(heat[hot[j]]); if (r <= 0) { a = hot[j]; break; } }
		int mv = rand() % 3, b = -1; uint32_t oa = color[a], obb = 0;
		if (mv == 0) color[a] = rand() & 511;
		else if (mv == 1) color[a] = (color[a] + (rand() % 9) - 4) & 511;
		else { b = hot[rand() % nh]; obb = color[b]; color[a] = obb; color[b] = oa; base[b] = color[b] * 16 + (ob[b].addr & 15); }
		base[a] = color[a] * 16 + (ob[a].addr & 15);
		sim(&m, &w, 1); long c = m + w;
		if (c <= cur || exp((cur - c) / T) > (double)rand() / RAND_MAX) { cur = c; if (c < best) { best = c; memcpy(bestc, color, nob * 4); } }
		else { color[a] = oa; base[a] = oa * 16 + (ob[a].addr & 15); if (b >= 0) { color[b] = obb; base[b] = obb * 16 + (ob[b].addr & 15); } }
		if (it % 200 == 0) fprintf(stderr, "it %ld T %.1f cur %ld best %ld (orig %ld)\n", it, T, cur, best, orig);
	}
	FILE *o = fopen(argv[4], "w");
	for (int j = 0; j < nh; j++) { int k = hot[j]; fprintf(o, "%u %u %.0f %c %s\n", bestc[k], ob[k].size, heat[k], ob[k].type, ob[k].name); }
	fclose(o);
	/* report heat by object */
	if (iters == 0) {
		for (int j = 0; j < nh; j++) { int k = hot[j]; if (heat[k] > nacc / 200) fprintf(stderr, "  %10.0f %7u %c %s\n", heat[k], ob[k].size, ob[k].type, ob[k].name); }
	}
	printf("best color-model cost %ld vs original %ld (%.1f%%)\n", best, orig, 100.0 * (best - orig) / orig);
	return 0;
}
