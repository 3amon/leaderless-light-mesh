#include "beacon_codec.h"

#include <stdbool.h>
#include <string.h>

#define LEGACY_MAGIC_V1 0x47535931U
#define LEGACY_MAGIC_V2 0x47535932U

#define V3_VERSION 3U

static const uint8_t v3_magic[4] = {'G', 'S', 'Y', '3'};

static void put_le32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_le32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
	       ((uint32_t)in[3] << 24);
}

static void put_le64(uint8_t *out, uint64_t value)
{
	put_le32(out, (uint32_t)value);
	put_le32(out + 4, (uint32_t)(value >> 32));
}

static uint64_t get_le64(const uint8_t *in)
{
	return (uint64_t)get_le32(in) | ((uint64_t)get_le32(in + 4) << 32);
}

static void put_be32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *in)
{
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	       ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static void put_be64(uint8_t *out, uint64_t value)
{
	put_be32(out, (uint32_t)(value >> 32));
	put_be32(out + 4, (uint32_t)value);
}

static uint64_t get_be64(const uint8_t *in)
{
	return ((uint64_t)get_be32(in) << 32) | (uint64_t)get_be32(in + 4);
}

static int64_t signed_from_bits(uint64_t value)
{
	if ((value & (UINT64_C(1) << 63)) == 0U) {
		return (int64_t)value;
	}
	if (value == (UINT64_C(1) << 63)) {
		return INT64_MIN;
	}
	return -(int64_t)((~value) + 1U);
}

static int32_t signed32_from_bits(uint32_t value)
{
	if ((value & (UINT32_C(1) << 31)) == 0U) {
		return (int32_t)value;
	}
	if (value == (UINT32_C(1) << 31)) {
		return INT32_MIN;
	}
	return -(int32_t)((~value) + 1U);
}

static bool small_id_valid(uint8_t small_id)
{
	return small_id < 32U || small_id == BEACON_SMALL_ID_UNKNOWN;
}

static int32_t legacy_rate_to_ppb(float rate_ppm, bool *valid)
{
	/* The comparison also rejects NaN without depending on libm. */
	if (!(rate_ppm >= -10000.0F && rate_ppm <= 10000.0F)) {
		*valid = false;
		return 0;
	}
	float scaled = rate_ppm * 1000.0F;

	*valid = true;
	return (int32_t)(scaled >= 0.0F ? scaled + 0.5F : scaled - 0.5F);
}

static int decode_legacy(struct beacon_message *message, const uint8_t *wire,
			 size_t wire_size)
{
	if (wire_size != BEACON_LEGACY_V1_SIZE && wire_size != BEACON_LEGACY_V2_SIZE) {
		return BEACON_CODEC_INVALID;
	}
	uint32_t magic = get_le32(wire);
	uint8_t version;

	if (wire_size == BEACON_LEGACY_V1_SIZE && magic == LEGACY_MAGIC_V1) {
		version = 1U;
	} else if (wire_size == BEACON_LEGACY_V2_SIZE && magic == LEGACY_MAGIC_V2) {
		version = 2U;
	} else {
		return BEACON_CODEC_INVALID;
	}

	uint32_t rate_bits = get_le32(wire + 24);
	float rate_ppm;
	bool rate_valid;

	memcpy(&rate_ppm, &rate_bits, sizeof(rate_ppm));
	int32_t rate_ppb = legacy_rate_to_ppb(rate_ppm, &rate_valid);
	if (!rate_valid) {
		return BEACON_CODEC_INVALID;
	}

	*message = (struct beacon_message){
		.wire_version = version,
		.node_id = get_le32(wire + 4),
		.global_us = signed_from_bits(get_le64(wire + 8)),
		.raw_us = signed_from_bits(get_le64(wire + 16)),
		.rate_ppb = rate_ppb,
		.seed = get_le32(wire + 28),
		.small_id = BEACON_SMALL_ID_UNKNOWN,
	};
	if (version == 2U) {
		message->small_id = wire[32];
		message->view = get_le32(wire + 36);
		memcpy(message->ext, wire + 40, sizeof(message->ext));
		if (!small_id_valid(message->small_id)) {
			return BEACON_CODEC_INVALID;
		}
	}
	return BEACON_CODEC_OK;
}

int beacon_encode(uint8_t *out, size_t capacity, const struct beacon_message *message)
{
	if (out == NULL || message == NULL || !small_id_valid(message->small_id)) {
		return BEACON_CODEC_INVALID;
	}
	if (capacity < BEACON_WIRE_SIZE) {
		return BEACON_CODEC_NO_SPACE;
	}

	memset(out, 0, BEACON_WIRE_SIZE);
	memcpy(out, v3_magic, sizeof(v3_magic));
	out[4] = V3_VERSION;
	out[5] = BEACON_WIRE_SIZE;
	out[6] = 0U;
	out[7] = message->small_id;
	put_be32(out + 8, message->node_id);
	put_be64(out + 12, (uint64_t)message->global_us);
	put_be64(out + 20, (uint64_t)message->raw_us);
	put_be32(out + 28, (uint32_t)message->rate_ppb);
	put_be32(out + 32, message->seed);
	put_be32(out + 36, message->view);
	memcpy(out + 40, message->ext, sizeof(message->ext));

	return (int)BEACON_WIRE_SIZE;
}

int beacon_encode_legacy_v2(uint8_t *out, size_t capacity,
			    const struct beacon_message *message)
{
	if (out == NULL || message == NULL || !small_id_valid(message->small_id)) {
		return BEACON_CODEC_INVALID;
	}
	if (capacity < BEACON_LEGACY_V2_SIZE) {
		return BEACON_CODEC_NO_SPACE;
	}

	memset(out, 0, BEACON_LEGACY_V2_SIZE);
	put_le32(out, LEGACY_MAGIC_V2);
	put_le32(out + 4, message->node_id);
	put_le64(out + 8, (uint64_t)message->global_us);
	put_le64(out + 16, (uint64_t)message->raw_us);
	float legacy_rate_ppm = (float)message->rate_ppb / 1000.0F;
	uint32_t legacy_rate_bits;

	memcpy(&legacy_rate_bits, &legacy_rate_ppm, sizeof(legacy_rate_bits));
	put_le32(out + 24, legacy_rate_bits);
	put_le32(out + 28, message->seed);
	out[32] = message->small_id;
	put_le32(out + 36, message->view);
	memcpy(out + 40, message->ext, sizeof(message->ext));
	return (int)BEACON_LEGACY_V2_SIZE;
}

int beacon_decode(struct beacon_message *message, const uint8_t *wire, size_t wire_size)
{
	if (message == NULL || wire == NULL) {
		return BEACON_CODEC_INVALID;
	}
	if (wire_size == BEACON_LEGACY_V1_SIZE ||
	    (wire_size == BEACON_LEGACY_V2_SIZE && memcmp(wire, v3_magic, 4) != 0)) {
		return decode_legacy(message, wire, wire_size);
	}
	if (wire_size != BEACON_WIRE_SIZE || memcmp(wire, v3_magic, sizeof(v3_magic)) != 0 ||
	    wire[4] != V3_VERSION || wire[5] != BEACON_WIRE_SIZE || wire[6] != 0U ||
	    !small_id_valid(wire[7])) {
		return BEACON_CODEC_INVALID;
	}

	struct beacon_message decoded = {
		.wire_version = V3_VERSION,
		.node_id = get_be32(wire + 8),
		.global_us = signed_from_bits(get_be64(wire + 12)),
		.raw_us = signed_from_bits(get_be64(wire + 20)),
		.rate_ppb = signed32_from_bits(get_be32(wire + 28)),
		.seed = get_be32(wire + 32),
		.small_id = wire[7],
		.view = get_be32(wire + 36),
	};
	memcpy(decoded.ext, wire + 40, sizeof(decoded.ext));

	*message = decoded;
	return BEACON_CODEC_OK;
}
