/* Deterministic social-tier LED scenes.
 *
 * A scene is a pure function of an identity/group seed, agreed member count,
 * shared global time, and LED index. There is no mutable random state: rings
 * reboot into the same scene and grouped rings independently render identical
 * pixels. Scene choices rotate on shared one-minute boundaries with a smooth
 * crossfade.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "genome.h"

#define SOCIAL_SCENE_MAX_COLORS 5
#define SOCIAL_SCENE_FAMILIES_PER_TIER 3
#define SOCIAL_SCENE_MIN_SATURATION 0.78f
#define SOCIAL_SCENE_MIN_DIFFUSED_SATURATION 0.35f
#define SOCIAL_SCENE_PARTY_FLASH_PERIOD_MIN_S 4.25f
#define SOCIAL_SCENE_PARTY_FLASH_PERIOD_MAX_S 5.75f
#define SOCIAL_SCENE_PARTY_FLASH_ACTIVE_S 0.37f
#define SOCIAL_SCENE_ULTRA_BLINK_DUTY 0.5f
#define SOCIAL_SCENE_ULTRA_SPEEDUP 0.6f

enum social_tier {
	SOCIAL_SOLO = 0,
	SOCIAL_DUO = 1,
	SOCIAL_CROWD = 2,
};

struct social_scene {
	uint32_t seed;
	enum social_tier tier;
	uint8_t member_count;
	bool party;
	bool ultra;            /* faster pacing on top of party */
	/* Register-wide synchronized effect, independent of mode:
	 * period 0 = none. Hard wave: duty = on-fraction. Fade wave:
	 * duty = minimum-brightness floor of the raised-cosine breathe.
	 * Duty >= 1.0 = solid either way. */
	float effect_period_s;
	float effect_duty;     /* 0 = 0.5 default */
	bool effect_fade;
	/* Party-accent strength, 0.0-1.0. 1.0 is the historical swell;
	 * 0.0 disables the accent while keeping party's denser motion.
	 * Only consulted in party/ultra — normal never accents. */
	float party_accent;
};

struct social_scene_plan {
	enum social_tier tier;
	uint8_t member_count;
	uint8_t family;
	bool party;
	bool ultra;
	float effect_period_s;
	float effect_duty;
	bool effect_fade;
	uint8_t color_count;
	float hue[SOCIAL_SCENE_MAX_COLORS];
	/* Solo only: an analogous second hue the solo families lean toward.
	 * Chosen per minute from a small set of spreads so a lone ring's
	 * palette varies from minute to minute without ever going
	 * complementary (a diffuser averages opposite hues to white). */
	float companion_hue;
	float saturation;
	float period_s;
	float phase;
	uint32_t salt;
};

enum social_tier social_tier_for_member_count(uint8_t member_count);

void social_scene_init(struct social_scene *scene, uint32_t seed,
		       enum social_tier tier, uint8_t member_count);
void social_scene_set_party(struct social_scene *scene, bool party);
void social_scene_set_ultra(struct social_scene *scene, bool ultra);
/* 0.0-1.0; 1.0 is the historical full-strength party accent. */
void social_scene_set_accent(struct social_scene *scene, float accent);
void social_scene_set_effect(struct social_scene *scene, float period_s,
			     float duty, bool fade);

/* Synchronized brightness envelope, identical on every crowd member via
 * the shared global clock. Returns 1.0 with no effect or solid duty;
 * a 0/1 square for the hard wave; a raised-cosine breathe descending to
 * the duty floor for the fade wave. */
float social_scene_effect_level(float global_t, float period_s, float duty,
				bool fade);

/* Expand the current minute's deterministic plan for telemetry and tests. */
void social_scene_plan_at(const struct social_scene *scene, float global_t,
			  struct social_scene_plan *plan);

/* Smooth, deterministic 0..1 party accent. There is at most one 370 ms
 * accent in each 4.25-to-5.75-second interval; normal mode always returns 0.
 */
float social_scene_party_flash_strength(const struct social_scene *scene,
					float global_t);

/* Render float RGB in 0..1, before global brightness/gamma. */
struct frgb social_scene_render(const struct social_scene *scene,
				float global_t, int led_index, int led_count);

/* Remove pastel/white lift from a composed social color while preserving its
 * peak brightness. fallback_hue is used only when the input has no hue (gray).
 */
struct frgb social_scene_vivid(struct frgb color, float fallback_hue);

/* Stable numeric pattern ID for heartbeat/status rotation checks. */
int social_scene_pattern_id(const struct social_scene *scene, float global_t);

const char *social_tier_name(enum social_tier tier);
const char *social_scene_family_name(enum social_tier tier, uint8_t family);
const char *social_scene_family_name_for_mode(bool party, enum social_tier tier,
					      uint8_t family);
