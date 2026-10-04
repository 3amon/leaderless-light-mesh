#include "network_mode.h"

#include <errno.h>
#include <string.h>

#define MODE_GOSSIP_VERSION 1

bool network_mode_valid(enum network_mode mode)
{
	return mode == NETWORK_MODE_NORMAL || mode == NETWORK_MODE_PARTY ||
	       mode == NETWORK_MODE_ULTRA;
}

const char *network_mode_name(enum network_mode mode)
{
	switch (mode) {
	case NETWORK_MODE_PARTY: return "party";
	case NETWORK_MODE_ULTRA: return "ultra";
	default:                 return "normal";
	}
}

bool network_mode_blink_valid(uint8_t blink_cs)
{
	return blink_cs == 0U || blink_cs >= NETWORK_MODE_BLINK_MIN_CS;
}

float network_mode_blink_period_s(uint8_t blink_cs)
{
	/* 0 = no effect, and 0.f is social_scene's no-effect sentinel too;
	 * the old 320 ms default here would silently turn "solid" into a
	 * blink for any caller that forgot to guard. */
	return (float)blink_cs / 100.f;
}

bool network_mode_duty_valid(uint8_t duty_pct)
{
	return duty_pct == 0U || duty_pct == NETWORK_MODE_DUTY_SOLID_PCT ||
	       (duty_pct >= NETWORK_MODE_DUTY_MIN_PCT &&
		duty_pct <= NETWORK_MODE_DUTY_MAX_PCT);
}

bool network_mode_wave_valid(uint8_t wave)
{
	return wave == NETWORK_MODE_WAVE_HARD ||
	       wave == NETWORK_MODE_WAVE_FADE;
}

const char *network_mode_wave_name(uint8_t wave)
{
	return wave == NETWORK_MODE_WAVE_FADE ? "fade" : "hard";
}

bool network_mode_accent_valid(uint8_t accent_pct)
{
	return accent_pct <= NETWORK_MODE_ACCENT_FULL_PCT;
}

float network_mode_accent_fraction(uint8_t accent_pct)
{
	if (accent_pct > NETWORK_MODE_ACCENT_FULL_PCT) {
		accent_pct = (uint8_t)NETWORK_MODE_ACCENT_FULL_PCT;
	}
	return (float)accent_pct / 100.f;
}

float network_mode_duty_fraction(uint8_t duty_pct)
{
	uint8_t effective = duty_pct != 0U
		? duty_pct
		: (uint8_t)NETWORK_MODE_DUTY_DEFAULT_PCT;

	return (float)effective / 100.f;
}

int network_mode_compare(const struct network_mode_value *candidate,
			 const struct network_mode_value *current)
{
	/* Wrap-around serial comparison: the sign of the 32-bit difference
	 * decides, so priorities form an endless cycle instead of a ladder
	 * that can run out. At the single ambiguous point exactly 2^31
	 * apart the difference is INT32_MIN in BOTH directions, which
	 * would make each side reject the other forever; break that tie by
	 * raw value so every node picks the same winner. */
	int32_t distance =
		(int32_t)(candidate->priority - current->priority);

	if (distance == INT32_MIN) {
		return candidate->priority > current->priority ? 1 : -1;
	}
	if (distance != 0) {
		return distance > 0 ? 1 : -1;
	}
	if (candidate->mode != current->mode) {
		return candidate->mode < current->mode ? 1 : -1;
	}
	/* Blink ties: no effect (0) is calmest and wins outright; between
	 * two active effects the slower period wins. */
	if (candidate->ultra_blink_cs != current->ultra_blink_cs) {
		if (candidate->ultra_blink_cs == 0U) {
			return 1;
		}
		if (current->ultra_blink_cs == 0U) {
			return -1;
		}
		return candidate->ultra_blink_cs > current->ultra_blink_cs
			? 1 : -1;
	}
	/* Provenance ties: a record whose wire era could not carry
	 * duty/wave loses to one whose era could — its opinion on the
	 * remaining fields is back-filled, not commanded. */
	int candidate_rank = (candidate->duty_known ? 1 : 0) +
			     (candidate->wave_known ? 1 : 0) +
			     (candidate->accent_known ? 1 : 0);
	int current_rank = (current->duty_known ? 1 : 0) +
			   (current->wave_known ? 1 : 0) +
			   (current->accent_known ? 1 : 0);

	if (candidate_rank != current_rank) {
		return candidate_rank > current_rank ? 1 : -1;
	}
	/* Accent ties: the dimmer accent is the calmer one, matching how
	 * blink prefers "no effect" and wave prefers the fade. */
	if (candidate->accent_pct != current->accent_pct) {
		return candidate->accent_pct < current->accent_pct ? 1 : -1;
	}
	/* Wave ties: the fade is calmer than the hard square. */
	if (candidate->wave != current->wave) {
		return candidate->wave == NETWORK_MODE_WAVE_FADE ? 1 : -1;
	}
	/* Ties otherwise equal converge toward the duty nearest 50% (the
	 * calmest), with the lower value as the deterministic final word. */
	uint8_t candidate_duty = candidate->ultra_duty_pct != 0U
		? candidate->ultra_duty_pct
		: (uint8_t)NETWORK_MODE_DUTY_DEFAULT_PCT;
	uint8_t current_duty = current->ultra_duty_pct != 0U
		? current->ultra_duty_pct
		: (uint8_t)NETWORK_MODE_DUTY_DEFAULT_PCT;

	if (candidate_duty != current_duty) {
		int candidate_distance = candidate_duty > 50 ?
			candidate_duty - 50 : 50 - candidate_duty;
		int current_distance = current_duty > 50 ?
			current_duty - 50 : 50 - current_duty;

		if (candidate_distance != current_distance) {
			return candidate_distance < current_distance ? 1 : -1;
		}
		return candidate_duty < current_duty ? 1 : -1;
	}
	/* Raw duty 0 and 50 render identically but are distinct bytes; pick
	 * the lower byte as canonical so registers converge bit-for-bit
	 * instead of gossip-disagreeing forever. */
	if (candidate->ultra_duty_pct != current->ultra_duty_pct) {
		return candidate->ultra_duty_pct < current->ultra_duty_pct
			? 1 : -1;
	}
	return 0;
}

static int encode_common(uint8_t *wire, size_t capacity, size_t wire_size,
			 uint8_t version,
			 const struct network_mode_value *value)
{
	if (wire == NULL || value == NULL || capacity < wire_size ||
	    !network_mode_valid(value->mode) ||
	    !network_mode_blink_valid(value->ultra_blink_cs) ||
	    !network_mode_duty_valid(value->ultra_duty_pct) ||
	    !network_mode_wave_valid(value->wave) ||
	    !network_mode_accent_valid(value->accent_pct)) {
		return -EINVAL;
	}
	memcpy(wire, "FMD1", 4);
	wire[4] = version;
	wire[5] = (uint8_t)wire_size;
	wire[6] = (uint8_t)value->mode;
	wire[7] = value->ultra_blink_cs;
	wire[8] = (uint8_t)(value->priority >> 24);
	wire[9] = (uint8_t)(value->priority >> 16);
	wire[10] = (uint8_t)(value->priority >> 8);
	wire[11] = (uint8_t)value->priority;
	return (int)wire_size;
}

int network_mode_encode_v1(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value)
{
	return encode_common(wire, capacity, NETWORK_MODE_WIRE_SIZE_V1, 1U,
			     value);
}

int network_mode_encode_v2(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value)
{
	int length = encode_common(wire, capacity, NETWORK_MODE_WIRE_SIZE_V2,
				   2U, value);

	if (length > 0) {
		wire[12] = value->ultra_duty_pct;
	}
	return length;
}

int network_mode_encode_v3(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value)
{
	int length = encode_common(wire, capacity, NETWORK_MODE_WIRE_SIZE_V3,
				   3U, value);

	if (length > 0) {
		wire[12] = value->ultra_duty_pct;
		wire[13] = value->wave;
	}
	return length;
}

int network_mode_encode_v4(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value)
{
	int length = encode_common(wire, capacity, NETWORK_MODE_WIRE_SIZE_V4,
				   4U, value);

	if (length > 0) {
		wire[12] = value->ultra_duty_pct;
		wire[13] = value->wave;
		wire[14] = value->accent_pct;
	}
	return length;
}

bool network_mode_decode(struct network_mode_value *value,
			 const uint8_t *wire, size_t length)
{
	if (value == NULL || wire == NULL ||
	    (length != NETWORK_MODE_WIRE_SIZE_V1 &&
	     length != NETWORK_MODE_WIRE_SIZE_V2 &&
	     length != NETWORK_MODE_WIRE_SIZE_V3 &&
	     length != NETWORK_MODE_WIRE_SIZE_V4) ||
	    memcmp(wire, "FMD1", 4) != 0 || wire[5] != length ||
	    !network_mode_blink_valid(wire[7]) ||
	    !network_mode_valid((enum network_mode)wire[6])) {
		return false;
	}
	uint8_t version = length == NETWORK_MODE_WIRE_SIZE_V1 ? 1U :
			  length == NETWORK_MODE_WIRE_SIZE_V2 ? 2U :
			  length == NETWORK_MODE_WIRE_SIZE_V3 ? 3U : 4U;

	if (wire[4] != version) {
		return false;
	}
	if (version >= 2U && !network_mode_duty_valid(wire[12])) {
		return false;
	}
	if (version >= 3U && !network_mode_wave_valid(wire[13])) {
		return false;
	}
	if (version >= 4U && !network_mode_accent_valid(wire[14])) {
		return false;
	}
	*value = (struct network_mode_value){
		.mode = (enum network_mode)wire[6],
		.priority = ((uint32_t)wire[8] << 24) |
			    ((uint32_t)wire[9] << 16) |
			    ((uint32_t)wire[10] << 8) | wire[11],
		.ultra_blink_cs = wire[7],
		.ultra_duty_pct = version >= 2U ? wire[12] : 0U,
		.wave = version >= 3U ? wire[13]
				      : (uint8_t)NETWORK_MODE_WAVE_HARD,
		/* Pre-v4 senders render the historical full-strength swell,
		 * so that is what their record means — not "off". */
		.accent_pct = version >= 4U
			? wire[14]
			: (uint8_t)NETWORK_MODE_ACCENT_FULL_PCT,
		.duty_known = version >= 2U,
		.wave_known = version >= 3U,
		.accent_known = version >= 4U,
	};
	return true;
}
