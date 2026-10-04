/* genome.c — pattern base families + seed-driven variant expansion.
 *
 * The seven families moved here from main.c, refactored two ways:
 *   - they render float RGB 0..1 with NO brightness applied (composited
 *     additively by the caller, overflow scaled proportionally there)
 *   - their hue constants hang off a palette primary H so the genome's
 *     hue wheel can repaint any base (offsets preserve each family's
 *     internal color relationships)
 */

#include <math.h>
#include <stdint.h>

#include "genome.h"

#define PI_F 3.14159265f
#define CYCLE_S 60.f
#define CYCLE_FADE_S 3.f
#define BOLD_FIRST 4
#define BOLD_COUNT 3

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static float fracf(float x) { return x - floorf(x); }

static float sstep(float x)
{
	x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
	return x * x * (3.f - 2.f * x);
}

static float ring_dist(float a, float b, int led_count)
{
	float d = fabsf(fmodf(a - b, (float)led_count));

	if (d > (float)led_count / 2.f) {
		d = (float)led_count - d;
	}
	return d;
}

/* float-RGB HSL, 0..1 output, no global-brightness scaling */
static struct frgb hslf(float h, float s, float l)
{
	h = fmodf(fmodf(h, 360.f) + 360.f, 360.f) / 360.f;
	float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
	float p = 2.f * l - q;
	float rgb[3] = { h + 1.f / 3.f, h, h - 1.f / 3.f };

	for (int i = 0; i < 3; i++) {
		float t = fmodf(fmodf(rgb[i], 1.f) + 1.f, 1.f);
		float v;

		if (t < 1.f / 6.f) {
			v = p + (q - p) * 6.f * t;
		} else if (t < 1.f / 2.f) {
			v = q;
		} else if (t < 2.f / 3.f) {
			v = p + (q - p) * (2.f / 3.f - t) * 6.f;
		} else {
			v = p;
		}
		rgb[i] = v;
	}
	return (struct frgb){ rgb[0], rgb[1], rgb[2] };
}

/* ---------- base families, hue offsets relative to a primary H ----------
 * Original absolute hues are preserved when H equals the family's native
 * primary (listed per family); the genome passes its wheel hue instead.
 */

/* native primary 256 */
static struct frgb fam_jelly(float t, int k, float H, int led_count)
{
	float ph = fracf(t / 5.8f);
	float a = ph < 0.42f ? sstep(ph / 0.42f) : 1.f - sstep((ph - 0.42f) / 0.58f);
	float L = 0.055f + 0.32f * a +
		  0.012f * sinf((float)k / (float)led_count *
				 2.f * PI_F * 2.f + t) * a;
	return hslf(H - 16.f * a, 0.8f, L);
}

/* native primary 228 */
static struct frgb fam_lighthouse(float t, int k, float H, int led_count)
{
	float pos = fracf(t / 22.f) * (float)led_count;
	float d = ring_dist(k, pos, led_count);
	float g = expf(-(d * d) / 7.f);
	float L = 0.065f + 0.30f * g * (0.8f + 0.2f * sinf(2.f * PI_F * t / 5.5f));

	return hslf(H - 36.f * g, 0.8f - 0.35f * g, L);
}

/* native primary 228 (twinkles sit 42° cooler) */
static struct frgb fam_plankton(float t, int k, float H, int led_count)
{
	(void)led_count;
	float u = t / 2.1f + k * 0.37f;
	int32_t slot = (int32_t)floorf(u);
	float fu = u - slot;
	uint32_t h = hash32((uint32_t)slot * 2654435761U ^ (uint32_t)k * 40503U ^ 0xBEEF);

	if (h % 9 == 0) {
		float tw = sinf(PI_F * fu);

		tw = tw * tw * (0.55f + (float)((h >> 4) & 3) / 6.7f);
		return hslf(H - 42.f, 0.55f, 0.06f + 0.42f * tw);
	}
	return hslf(H, 0.85f, 0.06f);
}

/* native primary 224 (petals +92°) */
static struct frgb fam_lotus(float t, int k, float H, int led_count)
{
	(void)led_count;
	float e = 0.5f + 0.45f * sinf(2.f * PI_F * t / 5.9f);
	float swell = 0.9f + 0.1f * sinf(2.f * PI_F * t / 17.f);

	if (k % 2 == 0) {
		return hslf(H + 92.f, 0.7f, (0.05f + 0.27f * e) * swell);
	}
	return hslf(H, 0.8f, (0.05f + 0.27f * (1.f - e)) * swell);
}

/* native primary 190 (comets at H, H+82, H−40) */
static struct frgb fam_comets(float t, int k, float H, int led_count)
{
	const float off[3] = { 0.f, 82.f, -40.f };
	float head = fracf(t / 2.4f) * (float)led_count;
	float best = 0.f;
	float bh = H;

	for (int c = 0; c < 3; c++) {
		float p = fmodf(head + c * ((float)led_count / 3.f),
				 (float)led_count);
		float dt = fmodf(p - (float)k + (float)led_count,
				  (float)led_count);
		float v = fmaxf(expf(-dt * dt * 0.55f),
				0.55f * expf(-dt * 0.85f));

		if (v > best) {
			best = v;
			bh = H + off[c];
		}
	}
	return hslf(bh, 0.95f, 0.02f + 0.40f * best);
}

/* native primary 172 (other half +86°) */
static struct frgb fam_tide(float t, int k, float H, int led_count)
{
	float edge = fracf(t / 3.2f) * 2.f * PI_F;
	float sq = tanhf(4.f * sinf((float)k / (float)led_count *
				     2.f * PI_F - edge));
	float m = 0.5f + 0.5f * sq;
	float surge = 0.8f + 0.2f * sinf(2.f * PI_F * t / 4.3f);
	float L = 0.055f + m * (0.30f * surge);

	return hslf(m > 0.5f ? H : H + 86.f, 0.9f, L);
}

/* native primary 205 */
static struct frgb fam_ripple(float t, int k, float H, int led_count)
{
	float ph = fracf(t / 2.4f);
	float r = ph * ((float)led_count / 2.f + 2.f);
	float d = ring_dist((float)k, 0.f, led_count);
	float g = expf(-(d - r) * (d - r) / 1.1f) * (1.f - ph) * (1.f - ph);

	return hslf(H - 55.f * g, 0.9f, 0.05f + 0.38f * g);
}

typedef struct frgb (*fam_fn)(float t, int k, float H, int led_count);

static const struct { fam_fn fn; float native_h; } families[GENOME_N_BASES] = {
	{ fam_jelly,      256.f }, { fam_lighthouse, 228.f },
	{ fam_plankton,   228.f }, { fam_lotus,      224.f },
	{ fam_comets,     190.f }, { fam_tide,       172.f },
	{ fam_ripple,     205.f },
};

const char *const genome_base_names[GENOME_N_BASES] = {
	"jelly-breath", "lighthouse-glass", "plankton", "lotus-breath",
	"comet-run", "tide-turn", "drop-ripple",
};

struct frgb genome_render_base(int base, float t, int k, int led_count)
{
	if (base < 0 || base >= GENOME_N_BASES) {
		base = 0;
	}
	if (led_count <= 0) {
		led_count = 1;
	}
	return families[base].fn(t, k, families[base].native_h, led_count);
}

/* ---------- genome expansion ---------- */

/* weighted hue wheel: mostly blues/violets/teals/greens, two rare warm
 * accents — 16 slots so 4 bits of hash pick one */
static const float hue_wheel[16] = {
	200.f, 218.f, 232.f, 248.f, 262.f, 275.f,     /* blues → violets */
	186.f, 172.f, 158.f, 145.f,                   /* teals → greens */
	210.f, 240.f, 190.f, 255.f,                   /* extra cool weight */
	28.f, 330.f,                                  /* rare warm accents */
};

void genome_from_seed(uint32_t seed, struct genome *g)
{
	/* hash before slicing — small/sequential seeds must not collapse
	 * the variant space */
	uint32_t h = hash32(seed);

	/* draw only from the BOLD families (comet-run, tide-turn,
	 * drop-ripple — sharp features, fast beats): sync state must be
	 * OBVIOUS at a glance. The ambient families (jelly/plankton/lotus/
	 * lighthouse) read as "fine" even badly desynced, which hides
	 * exactly what the wander behavior is supposed to show. They stay
	 * available via `ring pattern <n>` for demos. */
	g->base    = 4 + (h % 3);
	g->hue     = hue_wheel[(h >> 4) & 15];
	g->overlay = (uint8_t)((h >> 8) % 5);
	g->ov_amt  = 0.25f + 0.35f * (float)((h >> 12) & 15) / 15.f;
	g->tempo   = 0.8f + 0.4f * (float)((h >> 16) & 255) / 255.f;
	g->phase_s = 8.f * (float)((h >> 24) & 63) / 63.f;
	g->salt    = hash32(h ^ 0x5a17U);
}

/* ---------- overlays (additive textures on the base) ---------- */

static struct frgb ov_twinkle(const struct genome *g, float t, int k)
{
	float u = t / 2.0f + k * 0.61f;
	int32_t slot = (int32_t)floorf(u);
	float fu = u - slot;
	uint32_t h = hash32((uint32_t)slot * 2654435761U ^
			    (uint32_t)k * 40503U ^ g->salt);

	if (h % 11 != 0) {
		return (struct frgb){ 0, 0, 0 };
	}
	float tw = sinf(PI_F * fu);

	tw = tw * tw;
	return hslf(g->hue - 30.f, 0.35f, 0.35f * tw * g->ov_amt);
}

static struct frgb ov_arc(const struct genome *g, float t, int k,
			  int led_count)
{
	float pos = fracf(t / 13.f) * (float)led_count;
	float d = ring_dist((float)k, pos, led_count);
	float a = expf(-(d * d) / 3.5f);

	return hslf(g->hue + 24.f, 0.6f, 0.30f * a * g->ov_amt);
}

static struct frgb ov_shimmer(const struct genome *g, float t, int k)
{
	/* slow per-LED luminance noise, crossfaded between hash slots so it
	 * never pops */
	float u = t / 1.6f;
	int32_t slot = (int32_t)floorf(u);
	float fu = sstep(u - slot);
	uint32_t h0 = hash32((uint32_t)slot * 40503U ^ (uint32_t)k ^ g->salt);
	uint32_t h1 = hash32((uint32_t)(slot + 1) * 40503U ^ (uint32_t)k ^ g->salt);
	float n = ((float)(h0 & 255) * (1.f - fu) + (float)(h1 & 255) * fu) / 255.f;

	return hslf(g->hue, 0.5f, 0.12f * n * g->ov_amt);
}

static struct frgb ov_counter(const struct genome *g, float t, int k,
			      int led_count)
{
	float pos = (float)led_count - fracf(t / 9.f) * (float)led_count;
	float d = ring_dist((float)k, pos, led_count);
	float a = expf(-(d * d) / 6.f);

	return hslf(g->hue - 50.f, 0.7f, 0.22f * a * g->ov_amt);
}

const char *genome_overlay_name(uint8_t overlay)
{
	static const char *const names[5] = {
		"none", "twinkle", "arc", "shimmer", "counter",
	};
	return names[overlay % 5];
}

int genome_base_at(const struct genome *g, float t)
{
	int slot = (int)floorf(t / CYCLE_S);
	int start = (int)g->base - BOLD_FIRST;

	/* genome_from_seed() always chooses a bold base, but normalize here so
	 * a malformed or older persisted value cannot escape the family set. */
	start %= BOLD_COUNT;
	if (start < 0) {
		start += BOLD_COUNT;
	}
	int offset = slot % BOLD_COUNT;

	if (offset < 0) {
		offset += BOLD_COUNT;
	}
	return BOLD_FIRST + (start + offset) % BOLD_COUNT;
}

struct frgb genome_render(const struct genome *g, float t, int k,
			  int led_count)
{
	if (led_count <= 0) {
		led_count = 1;
	}
	float tt = (t + g->phase_s) * g->tempo;
	int base = genome_base_at(g, t);
	struct frgb c = families[base].fn(tt, k, g->hue, led_count);
	struct frgb o = { 0, 0, 0 };
	float cycle_pos = fmodf(t, CYCLE_S);

	if (cycle_pos < 0.f) {
		cycle_pos += CYCLE_S;
	}
	if (cycle_pos < CYCLE_FADE_S) {
		int prev = BOLD_FIRST + (base - BOLD_FIRST + BOLD_COUNT - 1)
			   % BOLD_COUNT;
		float mix = sstep(cycle_pos / CYCLE_FADE_S);
		struct frgb p = families[prev].fn(tt, k, g->hue, led_count);

		c.r = p.r * (1.f - mix) + c.r * mix;
		c.g = p.g * (1.f - mix) + c.g * mix;
		c.b = p.b * (1.f - mix) + c.b * mix;
	}

	switch (g->overlay % 5) {
	case 1: o = ov_twinkle(g, tt, k); break;
	case 2: o = ov_arc(g, tt, k, led_count); break;
	case 3: o = ov_shimmer(g, tt, k); break;
	case 4: o = ov_counter(g, tt, k, led_count); break;
	default: break;
	}
	c.r += o.r; c.g += o.g; c.b += o.b;
	float maximum = c.r > c.g ? c.r : c.g;

	maximum = maximum > c.b ? maximum : c.b;
	if (maximum > 1.f) {
		/* A denser spatial overlay on the seven-pixel layout can push a
		 * channel above one. Scale proportionally so the renderer contract
		 * remains bounded without clipping the hue. */
		c.r /= maximum;
		c.g /= maximum;
		c.b /= maximum;
	}
	return c;
}
