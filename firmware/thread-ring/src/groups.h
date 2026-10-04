/* groups.h — membership, group minting, and the composite render plan.
 *
 * Implements the four stacked layers from TIME-SYNC.md "Group patterns":
 *   1. candidate view from fresh direct-radio visibility
 *   2. membership BITMAP gossiped in every beacon (resolves disagreement)
 *   3. agreed set = damped component of the mutual-proximity graph; a group
 *      lights only when topology is complete and every member is phase-locked
 *      (80 ms measured-offset level with asymmetric 3-tick acquisition and
 *      5-tick loss hysteresis for noisy beacon timestamp telemetry)
 *   4. bounded committed/pending handoff: keep a certified group visible
 *      while the next set converges, then crossfade group-to-group without a
 *      solo detour; sustained invalid topology still fails closed
 *
 * groups_tick() runs ~1 Hz from the main loop (same thread as the render
 * loop, so render state needs no locking).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "social_scene.h"

void groups_init(uint32_t my_node_id, uint32_t my_seed);
void groups_tick(void);
void groups_set_party(bool party);
void groups_set_ultra(bool ultra);
void groups_set_accent(float accent);
void groups_set_effect(float period_s, float duty, bool fade);

/* What the render loop should draw this frame (weights include fades). */
struct groups_render {
	/* group layer (current + fading-out previous) */
	float group_w; /* 0..1 layer weight */
	struct social_scene group_scene;
	float old_w;
	struct social_scene old_scene;
	/* Normalized approach hint from the furthest-along candidate. */
	float approach_w;
	/* Solo yields completely once the agreed group layer is visible. */
	float solo_w;
};
void groups_get_render(struct groups_render *out);

/* telemetry for `ring group` / UDP */
int groups_status(char *buf, int len);
uint8_t groups_small_id(void);
uint32_t groups_agreed(void);   /* damped consensus bitmap */
bool groups_lit(void);          /* group layer showing (agreed≥2 + locked) */
int groups_group_pattern(void); /* lit social-scene pattern, -1 when none */
uint8_t groups_member_count(void);
enum social_tier groups_tier(void);
