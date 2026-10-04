/* Versioned, host-testable wire codec for clock/group gossip beacons. */

#ifndef THREAD_RING_BEACON_CODEC_H_
#define THREAD_RING_BEACON_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#define BEACON_LEGACY_V1_SIZE 32U
#define BEACON_LEGACY_V2_SIZE 48U
#define BEACON_WIRE_SIZE 48U

#define BEACON_SMALL_ID_UNKNOWN UINT8_MAX

enum beacon_codec_result {
	BEACON_CODEC_OK = 0,
	BEACON_CODEC_INVALID = -1,
	BEACON_CODEC_NO_SPACE = -2,
};

struct beacon_message {
	uint8_t wire_version;
	uint32_t node_id;
	int64_t global_us;
	int64_t raw_us;
	int32_t rate_ppb;
	uint32_t seed;
	uint8_t small_id;
	uint32_t view;
	uint8_t ext[8];
};

/* Encode an explicitly versioned, network-byte-order v3 beacon. */
int beacon_encode(uint8_t *out, size_t capacity, const struct beacon_message *message);

/* Encode the legacy little-endian/IEEE-754 v2 representation during the
 * rolling migration from v0.1.17. Remove this after every node accepts v3. */
int beacon_encode_legacy_v2(uint8_t *out, size_t capacity,
			    const struct beacon_message *message);

/* Decode legacy v1/v2 packets or a v3 packet. */
int beacon_decode(struct beacon_message *message, const uint8_t *wire, size_t wire_size);

#endif
