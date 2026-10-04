/* genome.h — seed → pattern-variant expansion (TIME-SYNC.md "Genome").
 *
 * Every node's look = a pure function of (global clock, seed). The seed is
 * hashed and sliced into { base family, palette hue from a cool-weighted
 * wheel, overlay texture + amount, tempo ±20%, phase } — thousands of
 * on-brand variants from a handful of base families. Group patterns feed a
 * different seed (XOR of member seed-hashes) through the same expansion.
 *
 * Rendering is in float RGB 0..1 (no brightness/gamma applied) so layers
 * can be composited additively and overflow-scaled proportionally —
 * per-channel clipping washes to white, scaling preserves hue.
 */

#pragma once

#include <stdint.h>

struct frgb { float r, g, b; };

struct genome {
	uint8_t  base;       /* base family index */
	float    hue;        /* palette primary (degrees) — repaints the family */
	uint8_t  overlay;    /* 0 none, 1 twinkle, 2 roving-arc, 3 shimmer,
			      * 4 counter-glow */
	float    ov_amt;     /* overlay strength 0.25..0.6 */
	float    tempo;      /* 0.8..1.2 time multiplier */
	float    phase_s;    /* time offset, seconds */
	uint32_t salt;       /* decorrelates hash-driven texture (twinkles) */
};

void genome_from_seed(uint32_t seed, struct genome *g);

/* t in seconds of the shared global clock, k = LED index. led_count makes
 * spatial patterns scale to the configured physical layout. */
struct frgb genome_render(const struct genome *g, float t, int k,
			  int led_count);

/* Hashed genomes keep their seed-selected starting family, then rotate
 * through the bold family set on shared one-minute boundaries. */
int genome_base_at(const struct genome *g, float t);

/* base families are also renderable directly (demo `ring pattern <n>`) */
#define GENOME_N_BASES 7
extern const char *const genome_base_names[GENOME_N_BASES];
struct frgb genome_render_base(int base, float t, int k, int led_count);

/* number of overlay kinds (for telemetry) */
const char *genome_overlay_name(uint8_t overlay);
