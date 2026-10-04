#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "network_mode.h"

static void test_priority_order(void)
{
	struct network_mode_value normal5 = { .mode = NETWORK_MODE_NORMAL, .priority = 5, .ultra_blink_cs = 0 };
	struct network_mode_value party4 = { .mode = NETWORK_MODE_PARTY, .priority = 4, .ultra_blink_cs = 0 };
	struct network_mode_value party6 = { .mode = NETWORK_MODE_PARTY, .priority = 6, .ultra_blink_cs = 0 };
	struct network_mode_value normal6 = { .mode = NETWORK_MODE_NORMAL, .priority = 6, .ultra_blink_cs = 0 };

	assert(network_mode_compare(&party4, &normal5) < 0);
	assert(network_mode_compare(&party6, &normal5) > 0);
	assert(network_mode_compare(&normal6, &party6) > 0);
	assert(network_mode_compare(&normal5, &normal5) == 0);

	/* ultra sits above party in the calmness ordering: at equal priority
	 * both party and normal beat it, and it needs a priority bump to win */
	struct network_mode_value ultra6 = { .mode = NETWORK_MODE_ULTRA, .priority = 6, .ultra_blink_cs = 0 };
	struct network_mode_value ultra7 = { .mode = NETWORK_MODE_ULTRA, .priority = 7, .ultra_blink_cs = 0 };

	assert(network_mode_valid(NETWORK_MODE_ULTRA));
	assert(network_mode_compare(&party6, &ultra6) > 0);
	assert(network_mode_compare(&normal6, &ultra6) > 0);
	assert(network_mode_compare(&ultra6, &party6) < 0);
	assert(network_mode_compare(&ultra7, &party6) > 0);

	/* priorities are an endless serial-number cycle, not a ladder: one
	 * step past the u32 wrap still wins, so the scheme never runs out */
	struct network_mode_value near_wrap = { .mode = NETWORK_MODE_PARTY, .priority = 0xffffffffU, .ultra_blink_cs = 0 };
	struct network_mode_value wrapped = { .mode = NETWORK_MODE_NORMAL, .priority = 0U, .ultra_blink_cs = 0 };
	struct network_mode_value wrapped_next = { .mode = NETWORK_MODE_ULTRA, .priority = 1U, .ultra_blink_cs = 0 };

	assert(network_mode_compare(&wrapped, &near_wrap) > 0);
	assert(network_mode_compare(&near_wrap, &wrapped) < 0);
	assert(network_mode_compare(&wrapped_next, &wrapped) > 0);
}

static void test_wire_codec(void)
{
	const struct network_mode_value expected = {
		.mode = NETWORK_MODE_PARTY,
		.priority = UINT32_MAX,
	};
	uint8_t wire[NETWORK_MODE_WIRE_SIZE_V1];
	struct network_mode_value decoded;

	assert(network_mode_encode_v1(wire, sizeof(wire), &expected) ==
	       NETWORK_MODE_WIRE_SIZE_V1);
	assert(memcmp(wire, "FMD1\x01\x0c\x01\x00\xff\xff\xff\xff",
		      sizeof(wire)) == 0);
	assert(network_mode_decode(&decoded, wire, sizeof(wire)));
	assert(decoded.mode == expected.mode && decoded.priority == expected.priority);
	assert(!network_mode_decode(&decoded, wire, sizeof(wire) - 1));
	wire[7] = 1;
	assert(!network_mode_decode(&decoded, wire, sizeof(wire)));
	wire[7] = 0;
	wire[6] = 2;   /* ultra decodes since v0.1.72 */
	assert(network_mode_decode(&decoded, wire, sizeof(wire)));
	assert(decoded.mode == NETWORK_MODE_ULTRA);
	wire[6] = 3;
	assert(!network_mode_decode(&decoded, wire, sizeof(wire)));

	/* configurable blink rides the old reserved byte: 0 = no effect,
	 * anything faster than the 130 ms floor fails closed */
	wire[6] = 2;
	wire[7] = 32;
	assert(network_mode_decode(&decoded, wire, sizeof(wire)));
	assert(decoded.ultra_blink_cs == 32);
	wire[7] = 12;
	assert(!network_mode_decode(&decoded, wire, sizeof(wire)));
	assert(network_mode_blink_valid(0) && network_mode_blink_valid(13));
	assert(!network_mode_blink_valid(5));
	/* since v3, blink 0 means NO effect — the old 320 ms default here
	 * was a trap that turned "solid" into a blink for unguarded callers */
	assert(network_mode_blink_period_s(0) == 0.f);
	assert(network_mode_blink_period_s(250) > 2.49f);

	struct network_mode_value slow_blink = { .mode = NETWORK_MODE_ULTRA, .priority = 9, .ultra_blink_cs = 100 };
	struct network_mode_value fast_blink = { .mode = NETWORK_MODE_ULTRA, .priority = 9, .ultra_blink_cs = 20 };

	/* ties on priority+mode converge toward the calmer (slower) blink,
	 * and no effect at all (0) beats any active effect */
	assert(network_mode_compare(&slow_blink, &fast_blink) > 0);
	assert(network_mode_compare(&fast_blink, &slow_blink) < 0);
	struct network_mode_value no_effect = slow_blink;

	no_effect.ultra_blink_cs = 0;
	assert(network_mode_compare(&no_effect, &slow_blink) > 0);
	assert(network_mode_compare(&slow_blink, &no_effect) < 0);

	/* wave ties: fade is calmer than hard */
	struct network_mode_value hard_wave = slow_blink;
	struct network_mode_value fade_wave = slow_blink;

	fade_wave.wave = NETWORK_MODE_WAVE_FADE;
	assert(network_mode_compare(&fade_wave, &hard_wave) > 0);
	assert(network_mode_compare(&hard_wave, &fade_wave) < 0);

	/* v3 wire carries the wave byte; invalid waves fail closed */
	uint8_t wire3[NETWORK_MODE_WIRE_SIZE_V3];
	struct network_mode_value decoded3;

	assert(network_mode_encode_v3(wire3, sizeof(wire3), &fade_wave) ==
	       NETWORK_MODE_WIRE_SIZE_V3);
	assert(wire3[4] == 3 && wire3[13] == NETWORK_MODE_WAVE_FADE);
	assert(network_mode_decode(&decoded3, wire3, sizeof(wire3)));
	assert(decoded3.wave == NETWORK_MODE_WAVE_FADE && decoded3.duty_known &&
	       decoded3.wave_known);
	wire3[13] = 7;
	assert(!network_mode_decode(&decoded3, wire3, sizeof(wire3)));
	assert(network_mode_wave_valid(0) && network_mode_wave_valid(1) &&
	       !network_mode_wave_valid(2));
	struct network_mode_value blink_encode_check = slow_blink;
	uint8_t wire2[NETWORK_MODE_WIRE_SIZE];

	assert(network_mode_encode_v1(wire2, sizeof(wire2),
				      &blink_encode_check) ==
	       NETWORK_MODE_WIRE_SIZE_V1);
	assert(wire2[7] == 100);
	fast_blink.ultra_blink_cs = 5;
	assert(network_mode_encode_v1(wire2, sizeof(wire2), &fast_blink) < 0);
}

static void test_v2_wire_carries_duty(void)
{
	struct network_mode_value ultra = {
		.mode = NETWORK_MODE_ULTRA,
		.priority = 12,
		.ultra_blink_cs = 100,
		.ultra_duty_pct = 70,
		.duty_known = true,
	};
	uint8_t wire[NETWORK_MODE_WIRE_SIZE_V2];
	struct network_mode_value decoded;

	assert(network_mode_encode_v2(wire, sizeof(wire), &ultra) ==
	       NETWORK_MODE_WIRE_SIZE_V2);
	assert(wire[4] == 2 && wire[5] == NETWORK_MODE_WIRE_SIZE_V2 &&
	       wire[12] == 70);
	assert(network_mode_decode(&decoded, wire, sizeof(wire)));
	assert(decoded.ultra_duty_pct == 70 && decoded.duty_known);

	/* a legacy v1 record decodes with NO duty opinion */
	uint8_t wire_v1[NETWORK_MODE_WIRE_SIZE_V1];

	assert(network_mode_encode_v1(wire_v1, sizeof(wire_v1), &ultra) ==
	       NETWORK_MODE_WIRE_SIZE_V1);
	assert(network_mode_decode(&decoded, wire_v1, sizeof(wire_v1)));
	assert(decoded.ultra_duty_pct == 0 && !decoded.duty_known &&
	       !decoded.wave_known);

	/* duty outside 20-80 fails closed on the wire and at encode */
	wire[12] = 10;
	assert(!network_mode_decode(&decoded, wire, sizeof(wire)));
	wire[12] = 85;
	assert(!network_mode_decode(&decoded, wire, sizeof(wire)));
	ultra.ultra_duty_pct = 90;
	assert(network_mode_encode_v2(wire, sizeof(wire), &ultra) < 0);
	assert(network_mode_duty_valid(0) && network_mode_duty_valid(20) &&
	       network_mode_duty_valid(80));
	assert(network_mode_duty_valid(100));   /* solid no-blink ultra */
	assert(!network_mode_duty_valid(19) && !network_mode_duty_valid(81) &&
	       !network_mode_duty_valid(99));
	assert(network_mode_duty_fraction(0) > 0.49f &&
	       network_mode_duty_fraction(0) < 0.51f);
	assert(network_mode_duty_fraction(70) > 0.69f);

	/* full ties converge toward the duty nearest 50% */
	struct network_mode_value centered = {
		.mode = NETWORK_MODE_ULTRA, .priority = 12,
		.ultra_blink_cs = 100, .ultra_duty_pct = 60,
		.duty_known = true,
	};
	struct network_mode_value skewed = centered;

	skewed.ultra_duty_pct = 80;
	assert(network_mode_compare(&centered, &skewed) > 0);
	assert(network_mode_compare(&skewed, &centered) < 0);
}

static void test_provenance_rank_blocks_legacy_echo(void)
{
	/* The mixed-fleet regression: a v0.1.74-77 node echoes the current
	 * winner as v2 at the SAME priority, back-filled with its stale
	 * duty (50, "nearest 50 wins") and no wave byte. Provenance rank
	 * must make that echo lose the tie in BOTH directions, or a single
	 * old node reverts every deliberate duty/wave setting. */
	struct network_mode_value mine = {
		.mode = NETWORK_MODE_PARTY, .priority = 40,
		.ultra_blink_cs = 100, .ultra_duty_pct = 80,
		.wave = NETWORK_MODE_WAVE_HARD,
		.duty_known = true, .wave_known = true,
	};
	struct network_mode_value echo_v2 = mine;

	echo_v2.ultra_duty_pct = 50;
	echo_v2.wave_known = false;   /* decoded from a v2 wire */
	assert(network_mode_compare(&echo_v2, &mine) < 0);
	assert(network_mode_compare(&mine, &echo_v2) > 0);

	/* the same echo must not beat SOLID (duty 100) either */
	struct network_mode_value solid = mine;

	solid.ultra_duty_pct = NETWORK_MODE_DUTY_SOLID_PCT;
	assert(network_mode_compare(&echo_v2, &solid) < 0);

	/* a v1 record (rank 0, values inherited so otherwise equal) loses
	 * ties too */
	struct network_mode_value echo_v1 = mine;

	echo_v1.duty_known = false;
	echo_v1.wave_known = false;
	assert(network_mode_compare(&echo_v1, &mine) < 0);
	assert(network_mode_compare(&mine, &echo_v1) > 0);

	/* but priority ALWAYS beats provenance: an operator commanding
	 * from an old node keeps control of the fleet */
	echo_v2.priority = 41;
	assert(network_mode_compare(&echo_v2, &mine) > 0);
	echo_v1.priority = 41;
	assert(network_mode_compare(&echo_v1, &mine) > 0);
}

static void test_v4_wire_carries_accent(void)
{
	struct network_mode_value quiet = {
		.mode = NETWORK_MODE_PARTY, .priority = 77,
		.ultra_blink_cs = 0, .ultra_duty_pct = 50,
		.wave = NETWORK_MODE_WAVE_FADE,
		.accent_pct = NETWORK_MODE_ACCENT_OFF_PCT,
		.duty_known = true, .wave_known = true, .accent_known = true,
	};
	uint8_t wire4[NETWORK_MODE_WIRE_SIZE_V4];

	assert(network_mode_encode_v4(wire4, sizeof(wire4), &quiet) ==
	       NETWORK_MODE_WIRE_SIZE_V4);

	struct network_mode_value decoded;

	assert(network_mode_decode(&decoded, wire4, sizeof(wire4)));
	assert(decoded.accent_pct == NETWORK_MODE_ACCENT_OFF_PCT);
	assert(decoded.accent_known);
	assert(decoded.wave == NETWORK_MODE_WAVE_FADE);
	assert(decoded.priority == 77);

	/* A pre-v4 wire has no accent field. It must mean "full", the way
	 * every pre-v4 build actually renders — decoding it as 0 would let
	 * one old node switch the accent off across the fleet. */
	uint8_t wire3[NETWORK_MODE_WIRE_SIZE_V3];

	assert(network_mode_encode_v3(wire3, sizeof(wire3), &quiet) ==
	       NETWORK_MODE_WIRE_SIZE_V3);
	assert(network_mode_decode(&decoded, wire3, sizeof(wire3)));
	assert(decoded.accent_pct == NETWORK_MODE_ACCENT_FULL_PCT);
	assert(!decoded.accent_known);

	/* out-of-range accent is refused on both encode and decode */
	struct network_mode_value bad = quiet;

	bad.accent_pct = 101;
	assert(network_mode_encode_v4(wire4, sizeof(wire4), &bad) < 0);
	assert(network_mode_encode_v4(wire4, sizeof(wire4), &quiet) ==
	       NETWORK_MODE_WIRE_SIZE_V4);
	wire4[14] = 101;
	assert(!network_mode_decode(&decoded, wire4, sizeof(wire4)));

	/* fraction mapping used by the renderer */
	assert(network_mode_accent_fraction(0) == 0.f);
	assert(network_mode_accent_fraction(100) == 1.f);
	assert(network_mode_accent_valid(100));
	assert(!network_mode_accent_valid(101));
}

static void test_v3_echo_cannot_revert_a_deliberate_accent(void)
{
	/* Same shape as the v2 regression above, one era later: a node
	 * still on a v3 build echoes the winner at equal priority with the
	 * accent back-filled to FULL. Provenance must stop that from
	 * undoing a deliberate "accent off" across the fleet. */
	struct network_mode_value mine = {
		.mode = NETWORK_MODE_PARTY, .priority = 206,
		.ultra_blink_cs = 0, .ultra_duty_pct = 50,
		.wave = NETWORK_MODE_WAVE_HARD,
		.accent_pct = NETWORK_MODE_ACCENT_OFF_PCT,
		.duty_known = true, .wave_known = true, .accent_known = true,
	};
	struct network_mode_value echo_v3 = mine;

	echo_v3.accent_pct = NETWORK_MODE_ACCENT_FULL_PCT;
	echo_v3.accent_known = false;   /* decoded from a v3 wire */
	assert(network_mode_compare(&echo_v3, &mine) < 0);
	assert(network_mode_compare(&mine, &echo_v3) > 0);

	/* priority still beats provenance, so an operator on an old node
	 * can always take the fleet back */
	echo_v3.priority = 207;
	assert(network_mode_compare(&echo_v3, &mine) > 0);

	/* between two v4 records the calmer (dimmer) accent wins a tie */
	struct network_mode_value bright = mine;

	bright.accent_pct = NETWORK_MODE_ACCENT_FULL_PCT;
	assert(network_mode_compare(&mine, &bright) > 0);
	assert(network_mode_compare(&bright, &mine) < 0);
}

static void test_serial_ambiguity_resolves(void)
{
	/* Exactly 2^31 apart the signed difference is INT32_MIN both ways;
	 * without a tie-break each side rejects the other forever. */
	struct network_mode_value low = {
		.mode = NETWORK_MODE_NORMAL, .priority = 7,
	};
	struct network_mode_value high = low;

	high.priority = 7U + 0x80000000U;
	assert(network_mode_compare(&high, &low) > 0);
	assert(network_mode_compare(&low, &high) < 0);
}

static void test_duty_zero_alias_converges(void)
{
	/* duty 0 and 50 render identically; the register must still settle
	 * on one canonical byte (0) instead of comparing equal-but-different
	 * forever. */
	struct network_mode_value zero = {
		.mode = NETWORK_MODE_NORMAL, .priority = 3,
		.duty_known = true, .wave_known = true,
	};
	struct network_mode_value fifty = zero;

	fifty.ultra_duty_pct = 50;
	assert(network_mode_compare(&zero, &fifty) > 0);
	assert(network_mode_compare(&fifty, &zero) < 0);
	assert(network_mode_compare(&zero, &zero) == 0);
}

#define SIM_NODES 24

static uint32_t sim_hash(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	return value;
}

static void sim_round(struct network_mode_value nodes[SIM_NODES],
		      unsigned int round)
{
	struct network_mode_value before[SIM_NODES];

	memcpy(before, nodes, sizeof(before));
	for (unsigned int sender = 0; sender < SIM_NODES; sender++) {
		for (unsigned int receiver = 0; receiver < SIM_NODES; receiver++) {
			if (sender == receiver ||
			    sim_hash(round * 997U + sender * 31U + receiver) % 10U < 3U) {
				continue; /* deterministic 30% loss */
			}
			if (network_mode_compare(&before[sender], &nodes[receiver]) > 0) {
				nodes[receiver] = before[sender];
			}
		}
	}
}

static void assert_all(struct network_mode_value nodes[SIM_NODES],
		       enum network_mode mode, uint32_t priority)
{
	for (unsigned int index = 0; index < SIM_NODES; index++) {
		assert(nodes[index].mode == mode);
		assert(nodes[index].priority == priority);
	}
}

static void test_lossy_24_node_epidemic_convergence(void)
{
	struct network_mode_value nodes[SIM_NODES] = { { .mode = NETWORK_MODE_NORMAL, .priority = 0, .ultra_blink_cs = 0 } };

	nodes[17] = (struct network_mode_value){ .mode = NETWORK_MODE_PARTY, .priority = 10, .ultra_blink_cs = 0 };
	for (unsigned int round = 0; round < 20; round++) {
		sim_round(nodes, round);
	}
	assert_all(nodes, NETWORK_MODE_PARTY, 10);

	/* An explicit later normal command wins. Repeated delayed party/10
	 * packets cannot resurrect the older state. */
	nodes[3] = (struct network_mode_value){ .mode = NETWORK_MODE_NORMAL, .priority = 20, .ultra_blink_cs = 0 };
	for (unsigned int round = 20; round < 40; round++) {
		sim_round(nodes, round);
		struct network_mode_value stale = { .mode = NETWORK_MODE_PARTY, .priority = 10, .ultra_blink_cs = 0 };

		assert(network_mode_compare(&stale, &nodes[round % SIM_NODES]) < 0);
	}
	assert_all(nodes, NETWORK_MODE_NORMAL, 20);
}

int main(void)
{
	test_priority_order();
	test_wire_codec();
	test_v2_wire_carries_duty();
	test_provenance_rank_blocks_legacy_echo();
	test_v4_wire_carries_accent();
	test_v3_echo_cannot_revert_a_deliberate_accent();
	test_serial_ambiguity_resolves();
	test_duty_zero_alias_converges();
	test_lossy_24_node_epidemic_convergence();
	puts("network mode policy tests passed");
	return 0;
}
