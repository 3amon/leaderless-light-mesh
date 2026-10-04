#include "social_scene.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#define PI_F 3.14159265f
#define SCENE_EPOCH_S 60.0f
#define SCENE_FADE_S 5.0f
#define DUO_PERIOD_MIN_S 3.5f
#define DUO_PERIOD_SPAN_S 3.5f
#define CROWD_PERIOD_MIN_S 1.25f
#define CROWD_PERIOD_SPAN_S 1.5f
#define PARTY_SOLO_PERIOD_MIN_S 1.25f
#define PARTY_SOLO_PERIOD_SPAN_S 1.0f
#define PARTY_DUO_PERIOD_MIN_S 0.9f
#define PARTY_DUO_PERIOD_SPAN_S 0.9f
#define PARTY_CROWD_PERIOD_MIN_S 0.75f
#define PARTY_CROWD_PERIOD_SPAN_S 0.75f
#define PARTY_FLASH_ATTACK_S 0.09f
#define PARTY_FLASH_RELEASE_S \
	(SOCIAL_SCENE_PARTY_FLASH_ACTIVE_S - PARTY_FLASH_ATTACK_S)
/* Accent gain: each pixel is multiplied by (1 + GAIN * strength), so at
 * full strength the ring swells to 3.2x its own instantaneous value rather
 * than every pixel being dragged to one absolute target. Chosen to be a
 * clearly visible beat while leaving the brightest pixels short of the 1.0
 * clamp, since clamping is what re-flattens the scene. */
#define PARTY_FLASH_GAIN 2.2f

static const float hue_wheel[] = {
	0.f,   15.f,  28.f,  45.f,  72.f,  105.f, 132.f, 155.f,
	175.f, 190.f, 205.f, 220.f, 235.f, 250.f, 265.f, 280.f,
	295.f, 310.f, 325.f, 340.f, 200.f, 240.f, 160.f, 330.f,
};

static const char *const family_names[3][SOCIAL_SCENE_FAMILIES_PER_TIER] = {
	{"tide", "comet", "starfield"},
	{"braid", "opposing-arcs", "two-tone-tide"},
	{"kaleidoscope", "aurora-ripple", "prism-sparks"},
};

static const char *const party_family_names[3][SOCIAL_SCENE_FAMILIES_PER_TIER] = {
	{"comet-cyclone", "neon-chase", "spark-rail"},
	{"counter-beams", "laser-braid", "color-rush"},
	{"rave-kaleidoscope", "prism-storm", "confetti-rush"},
};

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static float clamp01(float value)
{
	return value < 0.f ? 0.f : (value > 1.f ? 1.f : value);
}

static float fracf_positive(float value)
{
	return value - floorf(value);
}

static float smooth(float value)
{
	value = clamp01(value);
	return value * value * (3.f - 2.f * value);
}

static float ring_distance(float a, float b, int led_count)
{
	float distance = fabsf(fmodf(a - b, (float)led_count));

	if (distance > (float)led_count / 2.f) {
		distance = (float)led_count - distance;
	}
	return distance;
}

static struct frgb hsl(float hue, float saturation, float lightness)
{
	hue = fracf_positive(hue / 360.f);
	saturation = clamp01(saturation);
	lightness = clamp01(lightness);
	float q = lightness < 0.5f ? lightness * (1.f + saturation)
					  : lightness + saturation -
						    lightness * saturation;
	float p = 2.f * lightness - q;
	float channels[3] = {hue + 1.f / 3.f, hue, hue - 1.f / 3.f};

	for (int i = 0; i < 3; i++) {
		float t = fracf_positive(channels[i]);

		if (t < 1.f / 6.f) {
			channels[i] = p + (q - p) * 6.f * t;
		} else if (t < 1.f / 2.f) {
			channels[i] = q;
		} else if (t < 2.f / 3.f) {
			channels[i] = p + (q - p) * (2.f / 3.f - t) * 6.f;
		} else {
			channels[i] = p;
		}
	}
	return (struct frgb){channels[0], channels[1], channels[2]};
}

static struct frgb hsv(float hue, float saturation, float value)
{
	hue = fracf_positive(hue / 360.f) * 6.f;
	saturation = clamp01(saturation);
	value = clamp01(value);
	int sector = (int)floorf(hue);
	float fraction = hue - floorf(hue);
	float low = value * (1.f - saturation);
	float falling = value * (1.f - saturation * fraction);
	float rising = value * (1.f - saturation * (1.f - fraction));

	switch (sector % 6) {
	case 0:
		return (struct frgb){value, rising, low};
	case 1:
		return (struct frgb){falling, value, low};
	case 2:
		return (struct frgb){low, value, rising};
	case 3:
		return (struct frgb){low, falling, value};
	case 4:
		return (struct frgb){rising, low, value};
	default:
		return (struct frgb){value, low, falling};
	}
}

static float color_max(struct frgb color)
{
	float maximum = color.r > color.g ? color.r : color.g;

	return maximum > color.b ? maximum : color.b;
}

static float color_min(struct frgb color)
{
	float minimum = color.r < color.g ? color.r : color.g;

	return minimum < color.b ? minimum : color.b;
}

static float color_hue(struct frgb color, float fallback_hue)
{
	float maximum = color_max(color);
	float minimum = color_min(color);
	float chroma = maximum - minimum;

	if (chroma <= 0.000001f) {
		return fallback_hue;
	}
	float hue;

	if (maximum == color.r) {
		hue = 60.f * fmodf((color.g - color.b) / chroma, 6.f);
	} else if (maximum == color.g) {
		hue = 60.f * ((color.b - color.r) / chroma + 2.f);
	} else {
		hue = 60.f * ((color.r - color.g) / chroma + 4.f);
	}
	return hue < 0.f ? hue + 360.f : hue;
}

static struct frgb mix_vivid(struct frgb first, struct frgb second,
			     float amount)
{
	amount = clamp01(amount);
	if (amount <= 0.f) {
		return first;
	}
	if (amount >= 1.f) {
		return second;
	}
	float first_value = color_max(first);
	float second_value = color_max(second);
	float first_saturation =
		first_value > 0.f ? (first_value - color_min(first)) / first_value :
				   0.f;
	float second_saturation =
		second_value > 0.f ?
			(second_value - color_min(second)) / second_value :
			0.f;
	float first_hue = color_hue(first, 0.f);
	float second_hue = color_hue(second, first_hue);
	float hue_delta =
		fmodf(second_hue - first_hue + 540.f, 360.f) - 180.f;

	return hsv(first_hue + hue_delta * amount,
		   first_saturation * (1.f - amount) +
			   second_saturation * amount,
		   first_value * (1.f - amount) + second_value * amount);
}

struct frgb social_scene_vivid(struct frgb color, float fallback_hue)
{
	color.r = clamp01(color.r);
	color.g = clamp01(color.g);
	color.b = clamp01(color.b);
	float value = color_max(color);

	if (value <= 0.f) {
		return color;
	}
	float saturation = (value - color_min(color)) / value;

	if (saturation >= SOCIAL_SCENE_MIN_SATURATION) {
		return color;
	}
	return hsv(color_hue(color, fallback_hue),
		   SOCIAL_SCENE_MIN_SATURATION, value);
}

static uint32_t epoch_hash(const struct social_scene *scene, int64_t epoch)
{
	uint64_t bits = (uint64_t)epoch;
	uint32_t folded = (uint32_t)bits ^ (uint32_t)(bits >> 32);

	return hash32(scene->seed ^ hash32(folded ^ 0x6d2b79f5U) ^
		      ((uint32_t)scene->tier * 0x9e3779b9U) ^
		      ((uint32_t)scene->member_count * 0x85ebca6bU) ^
		      (scene->party ? 0xf17e5eedU : 0U));
}

static uint8_t family_for_epoch(const struct social_scene *scene, int64_t epoch)
{
	uint32_t identity =
		hash32(scene->seed ^ ((uint32_t)scene->tier * 0x9e3779b9U) ^
		       ((uint32_t)scene->member_count * 0x85ebca6bU) ^
		       (scene->party ? 0x50415254U : 0U) ^
		       0x42108421U);
	int64_t epoch_mod = epoch % SOCIAL_SCENE_FAMILIES_PER_TIER;

	if (epoch_mod < 0) {
		epoch_mod += SOCIAL_SCENE_FAMILIES_PER_TIER;
	}
	return (uint8_t)((identity + (uint32_t)epoch_mod) %
			 SOCIAL_SCENE_FAMILIES_PER_TIER);
}

static void plan_for_epoch_raw(const struct social_scene *scene, int64_t epoch,
			   struct social_scene_plan *plan)
{
	uint32_t value = epoch_hash(scene, epoch);
	size_t wheel_count = sizeof(hue_wheel) / sizeof(hue_wheel[0]);
	uint32_t hue_identity =
		hash32(scene->seed ^ ((uint32_t)scene->tier * 0x27d4eb2dU));
	int64_t hue_epoch = epoch % 360;

	if (hue_epoch < 0) {
		hue_epoch += 360;
	}
	/* Walk the anchor around the wheel in bounded steps. An independent
	 * random hue every minute can put complementary palettes on opposite
	 * sides of a crossfade, which a physical diffuser averages toward white.
	 */
	float primary = hue_wheel[hue_identity % wheel_count] +
			47.f * (float)hue_epoch;

	*plan = (struct social_scene_plan){
		.tier = scene->tier,
		.member_count = scene->member_count,
		.family = family_for_epoch(scene, epoch),
		.party = scene->party,
		.saturation = scene->party
			? 0.94f + 0.06f * (float)((value >> 10) & 15U) / 15.f
			: 0.76f + 0.18f * (float)((value >> 10) & 15U) / 15.f,
		.phase = 2.f * PI_F *
			 (float)((value >> 24) & 255U) / 255.f,
		.salt = hash32(value ^ 0xa511e9b3U),
	};

	if (scene->tier == SOCIAL_SOLO) {
		static const float companion_spread[] = {22.f, 34.f, 48.f, 64.f};
		float direction = ((value >> 28) & 1U) ? 1.f : -1.f;

		plan->color_count = 1;
		plan->hue[0] = primary;
		plan->companion_hue =
			primary + direction *
					  companion_spread[(value >> 26) & 3U];
		plan->period_s = scene->party
			? PARTY_SOLO_PERIOD_MIN_S + PARTY_SOLO_PERIOD_SPAN_S *
				(float)((value >> 14) & 255U) / 255.f
			: 10.f +
				 10.f * (float)((value >> 14) & 255U) / 255.f;
		return;
	}

	if (scene->tier == SOCIAL_DUO) {
		static const float separation[] = {60.f, 75.f, 90.f, 120.f,
						  150.f, 180.f};
		size_t separation_count =
			sizeof(separation) / sizeof(separation[0]);
		float direction = ((value >> 20) & 1U) ? 1.f : -1.f;

		plan->color_count = 2;
		plan->hue[0] = primary;
		plan->hue[1] =
			primary + direction *
					  separation[(value >> 16) %
						     separation_count];
		plan->companion_hue = plan->hue[1];
		plan->period_s = scene->party
			? PARTY_DUO_PERIOD_MIN_S + PARTY_DUO_PERIOD_SPAN_S *
				(float)((value >> 21) & 7U) / 7.f
			: DUO_PERIOD_MIN_S + DUO_PERIOD_SPAN_S *
				(float)((value >> 21) & 7U) / 7.f;
		return;
	}

	uint8_t colors = scene->member_count;

	if (colors < 3U) {
		colors = 3U;
	}
	if (colors > SOCIAL_SCENE_MAX_COLORS) {
		colors = SOCIAL_SCENE_MAX_COLORS;
	}
	plan->color_count = colors;
	uint8_t scheme = (uint8_t)((value >> 16) % 4U);
	static const float crowd_hue_step[] = {28.f, 34.f, 40.f};
	float direction = ((value >> 20) & 1U) ? 1.f : -1.f;

	plan->companion_hue = primary;
	if (scheme == 3U) {
		/* Two-anchor scheme: the palette alternates between the
		 * primary and a second anchor 70 to 100 degrees away, each
		 * with a small spread, instead of one fan of neighbours. Reads
		 * as a deliberate pairing rather than a gradient. Applies to
		 * every mode, since party crowds draw from the same list. */
		static const float anchor_gap[] = {70.f, 85.f, 100.f};
		float gap = anchor_gap[(value >> 26) % 3U];

		for (uint8_t i = 0; i < colors; i++) {
			float anchor = (i & 1U) ? primary + direction * gap
						: primary;

			plan->hue[i] = anchor + direction * 12.f *
						       (float)(i / 2U);
		}
	} else {
		float step = crowd_hue_step[scheme];

		for (uint8_t i = 0; i < colors; i++) {
			if (i == 0U) {
				plan->hue[i] = primary;
				continue;
			}
			float distance = step * (float)((i + 1U) / 2U);
			float side = (i & 1U) ? direction : -direction;

			plan->hue[i] = primary + side * distance;
		}
	}
	plan->period_s = scene->party
		? PARTY_CROWD_PERIOD_MIN_S + PARTY_CROWD_PERIOD_SPAN_S *
			(float)((value >> 21) & 7U) / 7.f
		: CROWD_PERIOD_MIN_S + CROWD_PERIOD_SPAN_S *
			(float)((value >> 21) & 7U) / 7.f;
}

/* Ultra rides on the party pacing (same families, saturation, and epoch
 * identity) and then compresses every period further and gates the final
 * color through a shared-clock square wave. */
static void plan_for_epoch(const struct social_scene *scene, int64_t epoch,
			   struct social_scene_plan *plan)
{
	plan_for_epoch_raw(scene, epoch, plan);
	plan->ultra = scene->ultra;
	plan->effect_period_s = scene->effect_period_s;
	plan->effect_duty = scene->effect_duty;
	plan->effect_fade = scene->effect_fade;
	if (scene->ultra) {
		plan->period_s *= SOCIAL_SCENE_ULTRA_SPEEDUP;
	}
}

void social_scene_set_party(struct social_scene *scene, bool party)
{
	scene->party = party;
}

void social_scene_set_ultra(struct social_scene *scene, bool ultra)
{
	scene->ultra = ultra;
}

void social_scene_set_accent(struct social_scene *scene, float accent)
{
	scene->party_accent = accent;
}

void social_scene_set_effect(struct social_scene *scene, float period_s,
			     float duty, bool fade)
{
	scene->effect_period_s = period_s;
	scene->effect_duty = duty;
	scene->effect_fade = fade;
}

float social_scene_effect_level(float global_t, float period_s, float duty,
				bool fade)
{
	if (period_s <= 0.f) {
		return 1.f;
	}
	float effective_duty = duty > 0.f ? duty : SOCIAL_SCENE_ULTRA_BLINK_DUTY;

	if (effective_duty >= 1.f) {
		return 1.f;   /* solid: effect disabled by duty */
	}
	float phase = fracf_positive(global_t / period_s);

	if (!fade) {
		return phase < effective_duty ? 1.f : 0.f;
	}
	/* raised-cosine breathe from 1.0 down to the duty floor and back */
	float swing = 0.5f - 0.5f * cosf(2.f * PI_F * phase);

	return 1.f - (1.f - effective_duty) * swing;
}

enum social_tier social_tier_for_member_count(uint8_t member_count)
{
	if (member_count <= 1U) {
		return SOCIAL_SOLO;
	}
	return member_count == 2U ? SOCIAL_DUO : SOCIAL_CROWD;
}

void social_scene_init(struct social_scene *scene, uint32_t seed,
		       enum social_tier tier, uint8_t member_count)
{
	if (tier < SOCIAL_SOLO || tier > SOCIAL_CROWD) {
		tier = SOCIAL_SOLO;
	}
	if (tier == SOCIAL_SOLO) {
		member_count = 1U;
	} else if (tier == SOCIAL_DUO) {
		member_count = 2U;
	} else if (member_count < 3U) {
		member_count = 3U;
	}
	*scene = (struct social_scene){
		.seed = seed,
		.tier = tier,
		.member_count = member_count,
		/* Full strength unless the register says otherwise: a
		 * zeroed accent means "no accent", so every scene must be
		 * born with the historical swell rather than silently
		 * rendering party mode without it. */
		.party_accent = 1.f,
	};
}

void social_scene_plan_at(const struct social_scene *scene, float global_t,
			  struct social_scene_plan *plan)
{
	int64_t epoch = (int64_t)floorf(global_t / SCENE_EPOCH_S);

	plan_for_epoch(scene, epoch, plan);
}

float social_scene_party_flash_strength(const struct social_scene *scene,
					float global_t)
{
	if (scene == NULL || !scene->party) {
		return 0.f;
	}
	uint32_t identity =
		hash32(scene->seed ^ ((uint32_t)scene->tier * 0x9e3779b9U) ^
		       ((uint32_t)scene->member_count * 0x85ebca6bU) ^
		       0xf1a5acceU);
	float period = SOCIAL_SCENE_PARTY_FLASH_PERIOD_MIN_S +
		       (SOCIAL_SCENE_PARTY_FLASH_PERIOD_MAX_S -
			SOCIAL_SCENE_PARTY_FLASH_PERIOD_MIN_S) *
			       (float)(identity & 1023U) / 1023.f;
	float phase_offset = period *
		(float)((identity >> 10) & 0xffffU) / 65535.f;
	float phase = fracf_positive((global_t + phase_offset) / period) *
		      period;

	if (phase < PARTY_FLASH_ATTACK_S) {
		return smooth(phase / PARTY_FLASH_ATTACK_S);
	}
	if (phase < SOCIAL_SCENE_PARTY_FLASH_ACTIVE_S) {
		return 1.f - smooth((phase - PARTY_FLASH_ATTACK_S) /
				    PARTY_FLASH_RELEASE_S);
	}
	return 0.f;
}

/* A lone ring keeps ONE anchor hue so the diffuser reads a single colour,
 * but every solo family carries an analogous companion, 22 to 64 degrees
 * away and on a side fixed per minute by the plan. Analogous, never
 * complementary: a physical cap averages neighbouring pixels, and hues
 * that close still average to a saturated colour leaning warm or cool,
 * whereas opposite hues average to white. */
static float solo_companion_hue(const struct social_scene_plan *plan)
{
	return plan->companion_hue;
}

/* Two-tone solo pixel: the anchor hue blended toward the companion by
 * `lean` (0..1) at one lightness, through the short hue arc so the blend
 * stays vivid. */
static struct frgb solo_tone(const struct social_scene_plan *plan, float lean,
			     float lightness)
{
	return mix_vivid(hsl(plan->hue[0], plan->saturation, lightness),
			 hsl(solo_companion_hue(plan), plan->saturation,
			     lightness),
			 clamp01(lean));
}

static struct frgb render_solo(const struct social_scene_plan *plan, float t,
			       int led, int led_count)
{
	float angle = 2.f * PI_F * (float)led / (float)led_count;
	float travel = t / plan->period_s + plan->phase / (2.f * PI_F);

	switch (plan->family) {
	case 0: {
		/* tide: the whole-ring breath, with one soft crest of light
		 * drifting around the ring on a slightly different period so
		 * the two never lock into a visible loop. The crest leans
		 * toward the companion hue, so the colour appears to move
		 * with the light rather than the ring merely dimming. */
		float pulse = 0.5f + 0.5f * sinf(2.f * PI_F * travel);
		float crest = 0.5f +
			      0.5f * sinf(angle -
					  2.f * PI_F * t /
						  (plan->period_s * 0.8f) +
					  plan->phase);

		crest *= crest;
		/* Levels sit just under the 45/LED frame power cap on average:
		 * a whole-ring scene is the rail's worst case, and anything the
		 * limiter trims off the top of the breath is dynamic range the
		 * eye never gets to see. */
		return solo_tone(plan, crest,
				 0.03f + 0.06f * smooth(pulse) + 0.09f * crest);
	}
	case 1: {
		/* comet: the orbiting halo grows a tail. The head is sharp
		 * on its leading edge and decays exponentially behind, so
		 * the direction of travel is legible at a glance; a dimmer
		 * companion-hued ember circles the other way at half speed
		 * and the two pass through each other twice a lap. */
		float span = (float)led_count / 16.f;
		float head = fracf_positive(travel) * (float)led_count;
		float behind = fmodf(head - (float)led + (float)led_count,
				     (float)led_count);
		float ahead = (float)led_count - behind;
		float tail = expf(-behind / (2.6f * span));
		float front = expf(-(ahead * ahead) / (1.1f * span * span));
		float glow = tail > front ? tail : front;
		float ember_at = fracf_positive(0.37f - 0.5f * travel) *
				 (float)led_count;
		float ember_d = ring_distance((float)led, ember_at, led_count);
		float ember = 0.45f * expf(-(ember_d * ember_d) /
					   (3.f * span * span));
		float lean = ember / (glow + ember + 0.02f);

		return solo_tone(plan, lean,
				 0.04f + 0.27f * glow + 0.22f * ember);
	}
	default: {
		/* starfield: the soft shimmer, with about one pixel in four
		 * per slot chosen to twinkle in the companion hue. Slots are
		 * crossfaded rather than switched (the spark-rail lesson), so
		 * each star rises over one slot and fades over the next and
		 * nothing ever steps. */
		float slot_s = plan->period_s / 7.f;
		float u = t / slot_s;
		int32_t slot = (int32_t)floorf(u);
		float amount = smooth(u - floorf(u));
		float noise = 0.f;
		float spark = 0.f;

		for (int i = 0; i < 2; i++) {
			uint32_t texture =
				hash32((uint32_t)(slot + i) * 40503U ^
				       (uint32_t)led * 2654435761U ^
				       plan->salt);
			float weight = i == 0 ? 1.f - amount : amount;

			noise += weight * (float)(texture & 255U) / 255.f;
			spark += weight * ((texture >> 8) % 4U == 0U ? 1.f
								     : 0.f);
		}
		float breath = 0.5f +
			       0.5f * sinf(2.f * PI_F * travel + angle * 0.15f);

		return solo_tone(plan, spark,
				 0.03f + 0.045f * noise + 0.03f * breath +
					 0.14f * spark);
	}
	}
}

static struct frgb render_duo(const struct social_scene_plan *plan, float t,
			      int led, int led_count)
{
	float angle = 2.f * PI_F * (float)led / (float)led_count;

	switch (plan->family) {
	case 0: {
		float amount =
			0.5f +
			0.5f * sinf(2.f * angle -
				    2.f * PI_F * t / plan->period_s +
				    plan->phase);
		float lightness =
			0.18f + 0.09f * sinf(angle +
					    2.f * PI_F * t /
						    (plan->period_s * 1.6f));

		return mix_vivid(hsl(plan->hue[0], plan->saturation, lightness),
				 hsl(plan->hue[1], plan->saturation,
				     lightness),
				 smooth(amount));
	}
	case 1: {
		float first_position =
			fracf_positive(t / plan->period_s +
				       plan->phase / (2.f * PI_F)) *
			(float)led_count;
		float second_position =
			fmodf(first_position + (float)led_count / 2.f,
			       (float)led_count);
		float first = expf(-ring_distance((float)led, first_position,
					  led_count) *
				   ring_distance((float)led, first_position,
						 led_count) /
				   3.5f);
		float second =
			expf(-ring_distance((float)led, second_position,
					    led_count) *
				 ring_distance((float)led, second_position,
					       led_count) /
				 3.5f);
		bool choose_second = second > first;
		float strength = choose_second ? second : first;

		return hsl(plan->hue[choose_second ? 1 : 0],
			   plan->saturation, 0.055f + 0.31f * strength);
	}
	default: {
		float wave = sinf(angle -
				  2.f * PI_F * t / plan->period_s +
				  plan->phase);
		float amount = smooth(0.5f + 0.5f * wave);
		float surge = 0.82f +
			      0.18f * sinf(2.f * PI_F * t /
					  (plan->period_s * 0.73f));
		float lightness = 0.075f + 0.25f * surge;

		return mix_vivid(hsl(plan->hue[0], plan->saturation, lightness),
				 hsl(plan->hue[1], plan->saturation,
				     lightness),
				 amount);
	}
	}
}

static struct frgb palette_mix(const struct social_scene_plan *plan, float index,
			       float lightness)
{
	float wrapped = fmodf(index, (float)plan->color_count);

	if (wrapped < 0.f) {
		wrapped += (float)plan->color_count;
	}
	int first = (int)floorf(wrapped);
	int second = (first + 1) % plan->color_count;
	float amount = smooth(wrapped - floorf(wrapped));

	return mix_vivid(hsl(plan->hue[first], plan->saturation, lightness),
			 hsl(plan->hue[second], plan->saturation, lightness),
			 amount);
}

static struct frgb render_party_solo(const struct social_scene_plan *plan,
				     float t, int led, int led_count)
{
	float position = (float)led / (float)led_count;
	float travel = t / plan->period_s + plan->phase / (2.f * PI_F);
	float angle = 2.f * PI_F * position;

	switch (plan->family) {
	case 0: {
		float head = fracf_positive(travel) * (float)led_count;
		float distance = ring_distance((float)led, head, led_count);
		float beam = expf(-(distance * distance) / 1.7f);

		return hsv(plan->hue[0] + distance * 18.f,
			   plan->saturation, 0.07f + 0.86f * beam);
	}
	case 1: {
		float wave = 0.5f + 0.5f * sinf(3.f * angle -
						     2.f * PI_F * travel);
		float beam = wave * wave * wave;
		float lane = fmodf(floorf(position * 6.f), 2.f);

		return hsv(plan->hue[0] + lane * 145.f + travel * 35.f,
			   plan->saturation, 0.08f + 0.78f * beam);
	}
	default: {
		float sweep = 0.5f + 0.5f * sinf(angle - 2.f * PI_F * travel);
		/* The sparkle set AND its hue offset are re-rolled on a
		 * quantised time slot. Switching hard at the boundary moved two
		 * things at once in a single frame: a quarter of the ring
		 * changed brightness, and every pixel's hue jumped by up to 180
		 * degrees. Measured as a 381-unit step every 0.448s — roughly
		 * twice a second — which is what reads as the pattern
		 * stuttering. A discontinuity in the content cannot be fixed by
		 * sampling it faster, so crossfade the outgoing slot into the
		 * incoming one across the slot's life instead. mix_vivid takes
		 * the short way around the hue wheel and keeps saturation up,
		 * so the blend stays vivid rather than passing through grey. */
		float slot_pos = travel * 5.f;
		uint32_t slot = (uint32_t)floorf(slot_pos);
		float blend = smooth(slot_pos - floorf(slot_pos));
		struct frgb shade[2];

		for (int i = 0; i < 2; i++) {
			uint32_t texture =
				hash32((uint32_t)led * 40503U ^
				       (slot + (uint32_t)i) ^ plan->salt);
			float spark = (texture % 4U == 0U) ? sweep * sweep
							   : 0.f;

			shade[i] = hsv(plan->hue[0] + (float)(texture % 180U),
				       plan->saturation,
				       0.07f + 0.31f * sweep + 0.58f * spark);
		}
		return mix_vivid(shade[0], shade[1], blend);
	}
	}
}

static struct frgb render_party_duo(const struct social_scene_plan *plan,
				    float t, int led, int led_count)
{
	float position = (float)led / (float)led_count;
	float angle = 2.f * PI_F * position;
	float travel = t / plan->period_s + plan->phase / (2.f * PI_F);
	float first = 0.5f + 0.5f * sinf(2.f * angle - 2.f * PI_F * travel);
	float second = 0.5f + 0.5f * sinf(2.f * angle + 2.f * PI_F * travel);
	float amount;
	float value;

	if (plan->family == 0) {
		first *= first;
		second *= second;
		amount = second > first ? 1.f : 0.f;
		value = 0.08f + 0.82f * fmaxf(first, second);
	} else if (plan->family == 1) {
		amount = smooth(0.5f + 0.5f * sinf(4.f * angle +
							 2.f * PI_F * travel));
		value = 0.12f + 0.7f * (0.5f + 0.5f *
				sinf(3.f * angle - 4.f * PI_F * travel));
	} else {
		amount = fracf_positive(position * 2.f + travel * 2.f);
		value = 0.16f + 0.68f * smooth(0.5f + 0.5f *
				sinf(6.f * angle - 2.f * PI_F * travel));
	}
	return mix_vivid(hsv(plan->hue[0], plan->saturation, value),
			 hsv(plan->hue[1], plan->saturation, value), amount);
}

static struct frgb render_party_crowd(const struct social_scene_plan *plan,
				      float t, int led, int led_count)
{
	float position = (float)led / (float)led_count;
	float travel = t / plan->period_s + plan->phase / (2.f * PI_F);
	float palette_position;
	float value;

	if (plan->family == 0) {
		palette_position = position * (float)plan->color_count * 3.f -
				   travel * 2.f;
		float wave = 0.5f + 0.5f * sinf(2.f * PI_F *
			(position * 4.f + travel));
		value = 0.08f + 0.78f * wave * wave;
	} else if (plan->family == 1) {
		palette_position = position * (float)plan->color_count +
				   travel * 3.f;
		float crossed = sinf(2.f * PI_F * (position * 3.f - travel)) *
				cosf(2.f * PI_F * (position * 2.f + travel * 1.7f));
		value = 0.12f + 0.72f * (0.5f + 0.5f * crossed);
	} else {
		palette_position = position * (float)plan->color_count * 1.5f +
				   travel * 2.2f;
		uint32_t slot = (uint32_t)floorf(travel * 6.f);
		uint32_t texture = hash32((uint32_t)led * 2654435761U ^ slot ^
					  plan->salt);
		float chase = 0.5f + 0.5f * sinf(2.f * PI_F *
			(position * 5.f - travel * 2.f));
		value = 0.09f + 0.46f * chase +
			((texture % 3U == 0U) ? 0.35f * chase * chase : 0.f);
	}
	return palette_mix(plan, palette_position, clamp01(value));
}

static struct frgb render_crowd(const struct social_scene_plan *plan, float t,
				int led, int led_count)
{
	float position = (float)led / (float)led_count;
	float travel = t / plan->period_s + plan->phase / (2.f * PI_F);

	switch (plan->family) {
	case 0: {
		float palette_position =
			position * (float)plan->color_count * 2.f - travel;
		float pulse =
			0.5f +
			0.5f * sinf(2.f * PI_F *
				    (position * 3.f + travel));

		pulse *= pulse;
		return palette_mix(plan, palette_position,
				   0.035f + 0.21f * pulse);
	}
	case 1: {
		float radius =
			fracf_positive(travel) * ((float)led_count / 2.f + 2.f);
		float distance = ring_distance((float)led, 0.f, led_count);
		float ridge = expf(-(distance - radius) *
				   (distance - radius) / 1.4f);
		float palette_position =
			position * (float)plan->color_count + travel * 1.7f;

		return palette_mix(plan, palette_position,
				   0.025f + 0.34f * ridge);
	}
	default: {
		float palette_position =
			position * (float)plan->color_count + travel * 1.35f;
		float spark =
			0.5f +
			0.5f * sinf(2.f * PI_F *
				    (travel * 2.f + position * 5.f));

		spark *= spark;
		uint32_t texture = hash32((uint32_t)led * 40503U ^
					  (uint32_t)floorf(travel * 2.f) ^
					  plan->salt);
		float accent = (texture % 5U == 0U) ? 0.1f * spark : 0.f;

		return palette_mix(plan, palette_position,
				   0.03f + 0.2f * spark + accent);
	}
	}
}

static struct frgb render_plan(const struct social_scene_plan *plan, float t,
			       int led, int led_count)
{
	if (plan->party) {
		if (plan->tier == SOCIAL_SOLO) {
			return render_party_solo(plan, t, led, led_count);
		}
		if (plan->tier == SOCIAL_DUO) {
			return render_party_duo(plan, t, led, led_count);
		}
		return render_party_crowd(plan, t, led, led_count);
	}
	if (plan->tier == SOCIAL_SOLO) {
		return render_solo(plan, t, led, led_count);
	}
	if (plan->tier == SOCIAL_DUO) {
		return render_duo(plan, t, led, led_count);
	}
	return render_crowd(plan, t, led, led_count);
}

static struct frgb apply_party_flash(const struct social_scene *scene,
				     float global_t, struct frgb color,
				     float fallback_hue)
{
	float strength = social_scene_party_flash_strength(scene, global_t);

	if (strength <= 0.f) {
		return color;
	}
	/* Scale by the register's accent setting. Clamped rather than
	 * trusted so a malformed register can never over-drive the swell
	 * past its designed ceiling. */
	float accent_scale = scene->party_accent;

	accent_scale = accent_scale < 0.f ? 0.f
					  : (accent_scale > 1.f ? 1.f
								: accent_scale);
	strength *= accent_scale;
	if (strength <= 0.f) {
		return color;
	}
	float value = color_max(color);
	float minimum = color_min(color);
	float saturation = value > 0.f ? (value - minimum) / value : 1.f;
	/* PROPORTIONAL, not additive-toward-a-constant. The original pulled
	 * every pixel toward the same absolute target, so a pixel at 0.005
	 * rose ~48x while one already at 0.6 barely moved, and at full
	 * strength they all converged on that target exactly: measured ring
	 * contrast collapsed from 7.2:1 to 1.0:1. A uniformly lit ring has no
	 * structure left for the eye to read hue from, which is why the
	 * "accent" read as a white strobe through the diffuser instead of the
	 * soft swell it is meant to be. Scaling each pixel by its own value
	 * keeps the pattern and only raises the beat. */
	float boosted = value * (1.f + PARTY_FLASH_GAIN * strength);

	if (boosted > 1.f) {
		boosted = 1.f;
	}
	return hsv(color_hue(color, fallback_hue),
		   fmaxf(saturation, SOCIAL_SCENE_MIN_SATURATION), boosted);
}

struct frgb social_scene_render(const struct social_scene *scene,
				float global_t, int led_index, int led_count)
{
	if (led_count <= 0) {
		led_count = 1;
	}
	int led = led_index % led_count;

	if (led < 0) {
		led += led_count;
	}
	int64_t epoch = (int64_t)floorf(global_t / SCENE_EPOCH_S);
	struct social_scene_plan current;

	plan_for_epoch(scene, epoch, &current);
	struct frgb color = render_plan(&current, global_t, led, led_count);
	float position = global_t - (float)epoch * SCENE_EPOCH_S;

	if (position >= 0.f && position < SCENE_FADE_S) {
		struct social_scene_plan previous;

		plan_for_epoch(scene, epoch - 1, &previous);
		struct frgb old = render_plan(&previous, global_t, led,
					      led_count);

		color = mix_vivid(old, color, smooth(position / SCENE_FADE_S));
	}
	if (current.party) {
		color = apply_party_flash(scene, global_t, color,
					current.hue[0]);
	}
	float level = social_scene_effect_level(global_t,
						current.effect_period_s,
						current.effect_duty,
						current.effect_fade);

	if (level <= 0.f) {
		return (struct frgb){0.f, 0.f, 0.f};
	}
	struct frgb lit = social_scene_vivid(color, current.hue[0]);

	lit.r *= level;
	lit.g *= level;
	lit.b *= level;
	return lit;
}

int social_scene_pattern_id(const struct social_scene *scene, float global_t)
{
	struct social_scene_plan plan;

	social_scene_plan_at(scene, global_t, &plan);
	return (plan.party ? 9 : 0) +
	       (int)plan.tier * SOCIAL_SCENE_FAMILIES_PER_TIER +
	       (int)plan.family;
}

const char *social_tier_name(enum social_tier tier)
{
	static const char *const names[] = {"solo", "duo", "crowd"};

	return tier >= SOCIAL_SOLO && tier <= SOCIAL_CROWD ? names[tier] :
							    "solo";
}

const char *social_scene_family_name(enum social_tier tier, uint8_t family)
{
	if (tier < SOCIAL_SOLO || tier > SOCIAL_CROWD) {
		tier = SOCIAL_SOLO;
	}
	return family_names[tier][family % SOCIAL_SCENE_FAMILIES_PER_TIER];
}

const char *social_scene_family_name_for_mode(bool party, enum social_tier tier,
					      uint8_t family)
{
	if (tier < SOCIAL_SOLO || tier > SOCIAL_CROWD) {
		tier = SOCIAL_SOLO;
	}
	return (party ? party_family_names : family_names)[tier]
		[family % SOCIAL_SCENE_FAMILIES_PER_TIER];
}
