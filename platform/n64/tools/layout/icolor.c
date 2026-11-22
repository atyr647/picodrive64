/* icolor: optimize I-cache placement ("colors") of hot .text sections against
 * an ares fetch-run trace, for a 16KB direct-mapped I$ with 32B lines.
 *
 *   icolor SECS TRACE.i ITERS SEED OUT.colors   (optimize)
 *   icolor SECS TRACE.i 0 0 - NEWSECS           (evaluate a real layout: NEWSECS = map_sections of a new link)
 *
 * SECS: "addr size name file" sorted by addr (original layout of the trace).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct { uint32_t addr, size; char name[160]; } sec_t;
static sec_t *secs; static int nsecs;
static uint32_t *rsec, *roff, *rlen; static long nruns;

static void load_secs(const char *fn, sec_t **out, int *n)
{
	FILE *f = fopen(fn, "r"); char ln[512]; int cap = 4096; *out = malloc(cap * sizeof(sec_t)); *n = 0;
	while (fgets(ln, sizeof ln, f)) {
		sec_t s; char file[256]; file[0] = 0;
		if (sscanf(ln, "%x %u %159s %255s", &s.addr, &s.size, s.name, file) < 3) continue;
		/* plain .text sections are identified by file */
		if (!strcmp(s.name, ".text")) { snprintf(s.name, sizeof s.name, ".text@%s", file); }
		if (*n == cap) { cap *= 2; *out = realloc(*out, cap * sizeof(sec_t)); }
		(*out)[(*n)++] = s;
	}
	fclose(f);
}

static int find_sec(uint32_t a)
{
	int lo = 0, hi = nsecs - 1, k = -1;
	while (lo <= hi) { int m = (lo + hi) / 2; if (secs[m].addr <= a) { k = m; lo = m + 1; } else hi = m - 1; }
	if (k >= 0 && a < secs[k].addr + secs[k].size) return k;
	return -1;
}

/* base[] gives each section's start address; sections with base 0xffffffff never run */
static uint32_t tags[512];
static long simulate(const uint32_t *base, int use_color)
{
	long miss = 0;
	memset(tags, 0xff, sizeof tags);
	for (long i = 0; i < nruns; i++) {
		uint32_t s, e, sec = rsec[i];
		if (sec == 0xffffffffu) s = roff[i];		/* outside .text: absolute */
		else s = base[sec] + roff[i];
		e = s + 4 * rlen[i] - 4;
		for (uint32_t l = s >> 5; l <= (e >> 5); l++) {
			uint32_t set = l & 511, tag = use_color ? (l | (sec << 12)) : l;
			if (sec == 0xffffffffu) tag = l;
			if (tags[set] != tag) { tags[set] = tag; miss++; }
		}
	}
	return miss;
}

int main(int argc, char **argv)
{
	if (argc < 6) { fprintf(stderr, "usage\n"); return 1; }
	load_secs(argv[1], &secs, &nsecs);
	FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint32_t *tr = malloc(sz); fread(tr, 1, sz, f); fclose(f);
	nruns = sz / 8;
	rsec = malloc(nruns * 4); roff = malloc(nruns * 4); rlen = malloc(nruns * 4);
	double *heat = calloc(nsecs, sizeof(double));
	for (long i = 0; i < nruns; i++) {
		uint32_t a = tr[2 * i], l = tr[2 * i + 1];
		int k = find_sec(a);
		rlen[i] = l;
		if (k < 0) { rsec[i] = 0xffffffffu; roff[i] = a; }
		else { rsec[i] = k; roff[i] = a - secs[k].addr; heat[k] += l; }
	}
	free(tr);
	uint32_t *base = malloc(nsecs * 4);
	for (int k = 0; k < nsecs; k++) base[k] = secs[k].addr;
	long orig = simulate(base, 0);
	fprintf(stderr, "runs %ld, original layout misses %ld\n", nruns, orig);

	if (argc >= 7 && atol(argv[3]) == 0) {	/* evaluate a new real layout */
		sec_t *ns; int nn; load_secs(argv[6], &ns, &nn);
		int found = 0;
		for (int k = 0; k < nsecs; k++) {
			base[k] = 0xffffffffu;
			for (int j = 0; j < nn; j++) if (!strcmp(ns[j].name, secs[k].name) && ns[j].size == secs[k].size) { base[k] = ns[j].addr; found++; break; }
			if (base[k] == 0xffffffffu && heat[k] > 0) fprintf(stderr, "missing hot section %s\n", secs[k].name);
		}
		printf("new layout misses %ld (orig %ld, %+.1f%%)\n", simulate(base, 0), orig, 100.0 * (simulate(base, 0) - orig) / orig);
		return 0;
	}

	long iters = atol(argv[3]); srand(atoi(argv[4]));
	/* hot list */
	int *hot = malloc(nsecs * sizeof(int)), nh = 0; double htot = 0;
	for (int k = 0; k < nsecs; k++) if (heat[k] > 0) { hot[nh++] = k; htot += sqrt(heat[k]); }
	/* color model: base = color*32 (line aligned), tags made unique per section */
	uint32_t *color = malloc(nsecs * 4);
	for (int k = 0; k < nsecs; k++) { color[k] = (secs[k].addr >> 5) & 511; base[k] = color[k] * 32 + (secs[k].addr & 31 & 0); }
	if (argc >= 7) {	/* warm start from a colors file */
		FILE *ci = fopen(argv[6], "r"); char ln[512];
		while (fgets(ln, sizeof ln, ci)) {
			unsigned c, sz; double h; char nm[200];
			if (sscanf(ln, "%u %u %lf %199s", &c, &sz, &h, nm) != 4) continue;
			for (int k = 0; k < nsecs; k++) if (!strcmp(secs[k].name, nm)) { color[k] = c; base[k] = c * 32; break; }
		}
		fclose(ci);
	}
	long cur = simulate(base, 1), best = cur;
	fprintf(stderr, "color model of original: %ld\n", cur);
	uint32_t *bestc = malloc(nsecs * 4); memcpy(bestc, color, nsecs * 4);
	double T0 = cur * 0.002, T1 = 0.5;
	for (long it = 0; it < iters; it++) {
		double T = T0 * pow(T1 / T0, (double)it / iters);
		/* pick a hot section weighted by sqrt(heat) */
		double r = (double)rand() / RAND_MAX * htot; int a = hot[nh - 1];
		for (int j = 0; j < nh; j++) { r -= sqrt(heat[hot[j]]); if (r <= 0) { a = hot[j]; break; } }
		int mv = rand() % 3, b = -1; uint32_t oa = color[a], ob = 0;
		if (mv == 0) color[a] = rand() & 511;
		else if (mv == 1) { color[a] = (color[a] + (rand() % 17) - 8) & 511; }
		else { b = hot[rand() % nh]; ob = color[b]; color[a] = ob; color[b] = oa; base[b] = color[b] * 32; }
		base[a] = color[a] * 32;
		long c = simulate(base, 1);
		if (c <= cur || exp((cur - c) / T) > (double)rand() / RAND_MAX) {
			cur = c;
			if (c < best) { best = c; memcpy(bestc, color, nsecs * 4); }
		} else {
			color[a] = oa; base[a] = oa * 32;
			if (b >= 0) { color[b] = ob; base[b] = ob * 32; }
		}
		if (it % 500 == 0) fprintf(stderr, "it %ld T %.1f cur %ld best %ld (orig %ld)\n", it, T, cur, best, orig);
	}
	FILE *o = fopen(argv[5], "w");
	for (int j = 0; j < nh; j++) { int k = hot[j]; fprintf(o, "%u %u %.0f %s\n", bestc[k], secs[k].size, heat[k], secs[k].name); }
	fclose(o);
	printf("best color-model misses %ld vs original %ld (%.1f%%)\n", best, orig, 100.0 * (best - orig) / orig);
	return 0;
}
