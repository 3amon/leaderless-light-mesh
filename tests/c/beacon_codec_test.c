#include "beacon_codec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct beacon_message example = {
	.node_id = 0x12345678U,
	.global_us = -INT64_C(0x0102030405060708),
	.raw_us = INT64_C(0x1122334455667788),
	.rate_ppb = -2345678,
	.seed = 0x90abcdefU,
	.small_id = 13,
	.view = 0x01002000U,
	.ext = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
};

static void assert_message(const struct beacon_message *actual, uint8_t version)
{
	assert(actual->wire_version == version);
	assert(actual->node_id == example.node_id);
	assert(actual->global_us == example.global_us);
	assert(actual->raw_us == example.raw_us);
	if (version == 3) {
		assert(actual->rate_ppb == example.rate_ppb);
	} else {
		assert(actual->rate_ppb >= example.rate_ppb - 1);
		assert(actual->rate_ppb <= example.rate_ppb + 1);
	}
	assert(actual->seed == example.seed);
	assert(actual->small_id == example.small_id);
	assert(actual->view == example.view);
	assert(memcmp(actual->ext, example.ext, sizeof(actual->ext)) == 0);
}

static void test_v3_round_trip(void)
{
	uint8_t wire[BEACON_WIRE_SIZE];
	struct beacon_message decoded;
	static const uint8_t expected_global[8] = {0xfe, 0xfd, 0xfc, 0xfb,
						 0xfa, 0xf9, 0xf8, 0xf8};
	static const uint8_t expected_raw[8] = {0x11, 0x22, 0x33, 0x44,
					      0x55, 0x66, 0x77, 0x88};
	static const uint8_t expected_rate[4] = {0xff, 0xdc, 0x35, 0x32};

	assert(beacon_encode(wire, sizeof(wire), &example) == BEACON_WIRE_SIZE);
	assert(memcmp(wire, "GSY3", 4) == 0);
	assert(wire[4] == 3);
	assert(wire[5] == BEACON_WIRE_SIZE);
	assert(wire[8] == 0x12 && wire[9] == 0x34 && wire[10] == 0x56 && wire[11] == 0x78);
	assert(memcmp(wire + 12, expected_global, sizeof(expected_global)) == 0);
	assert(memcmp(wire + 20, expected_raw, sizeof(expected_raw)) == 0);
	assert(memcmp(wire + 28, expected_rate, sizeof(expected_rate)) == 0);
	assert(memcmp(wire + 32, "\x90\xab\xcd\xef", 4) == 0);
	assert(memcmp(wire + 36, "\x01\x00\x20\x00", 4) == 0);
	assert(memcmp(wire + 40, example.ext, sizeof(example.ext)) == 0);

	assert(beacon_decode(&decoded, wire, sizeof(wire)) == BEACON_CODEC_OK);
	assert_message(&decoded, 3);
}

static void test_legacy_v2_round_trip(void)
{
	uint8_t wire[BEACON_WIRE_SIZE];
	struct beacon_message decoded;

	assert(beacon_encode_legacy_v2(wire, sizeof(wire), &example) ==
	       BEACON_LEGACY_V2_SIZE);
	assert(memcmp(wire, "2YSG", 4) == 0);
	assert(beacon_decode(&decoded, wire, BEACON_LEGACY_V2_SIZE) == BEACON_CODEC_OK);
	assert_message(&decoded, 2);
}

static void test_legacy_v1_defaults_group_fields(void)
{
	uint8_t wire[BEACON_WIRE_SIZE];
	struct beacon_message decoded;

	assert(beacon_encode_legacy_v2(wire, sizeof(wire), &example) ==
	       BEACON_LEGACY_V2_SIZE);
	memcpy(wire, "1YSG", 4);
	assert(beacon_decode(&decoded, wire, BEACON_LEGACY_V1_SIZE) == BEACON_CODEC_OK);
	assert(decoded.wire_version == 1);
	assert(decoded.node_id == example.node_id);
	assert(decoded.small_id == BEACON_SMALL_ID_UNKNOWN);
	assert(decoded.view == 0U);
}

static void test_rejects_malformed_packets(void)
{
	uint8_t wire[BEACON_WIRE_SIZE];
	struct beacon_message decoded;
	struct beacon_message invalid = example;

	assert(beacon_encode(wire, sizeof(wire), &example) == BEACON_WIRE_SIZE);
	assert(beacon_decode(&decoded, wire, BEACON_LEGACY_V1_SIZE - 1) ==
	       BEACON_CODEC_INVALID);

	wire[4] = 4;
	assert(beacon_decode(&decoded, wire, sizeof(wire)) == BEACON_CODEC_INVALID);
	wire[4] = 3;
	wire[6] = 1;
	assert(beacon_decode(&decoded, wire, sizeof(wire)) == BEACON_CODEC_INVALID);
	wire[6] = 0;
	wire[7] = 32;
	assert(beacon_decode(&decoded, wire, sizeof(wire)) == BEACON_CODEC_INVALID);

	assert(beacon_encode(wire, BEACON_WIRE_SIZE - 1, &example) == BEACON_CODEC_NO_SPACE);
	assert(beacon_encode_legacy_v2(wire, BEACON_LEGACY_V2_SIZE - 1, &example) ==
	       BEACON_CODEC_NO_SPACE);
	invalid.small_id = 32;
	assert(beacon_encode(wire, sizeof(wire), &invalid) == BEACON_CODEC_INVALID);
}

int main(void)
{
	test_v3_round_trip();
	test_legacy_v2_round_trip();
	test_legacy_v1_defaults_group_fields();
	test_rejects_malformed_packets();
	puts("beacon codec tests passed");
	return 0;
}
