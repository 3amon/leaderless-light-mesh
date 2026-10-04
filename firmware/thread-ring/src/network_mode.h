/* Persistent, priority-ordered fleet mode replicated over Thread. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* v1 (12 bytes) carries mode/priority/blink; v2 (13 bytes) appends the
 * duty byte; v3 (14 bytes) appends the waveform byte and makes the blink
 * effect mode-independent (blink_cs 0 = no effect); v4 (15 bytes) appends
 * the party-accent strength. Nodes transmit v4, v3 and the legacy v1 copy
 * each tick, so older targets keep following mode changes and stay fully
 * OTA-able; v2 remains decodable. */
#define NETWORK_MODE_WIRE_SIZE_V1 12
#define NETWORK_MODE_WIRE_SIZE_V2 13
#define NETWORK_MODE_WIRE_SIZE_V3 14
#define NETWORK_MODE_WIRE_SIZE_V4 15
#define NETWORK_MODE_WIRE_SIZE NETWORK_MODE_WIRE_SIZE_V4

enum network_mode {
	NETWORK_MODE_NORMAL = 0,
	NETWORK_MODE_PARTY = 1,
	NETWORK_MODE_ULTRA = 2,
};

/* Blink period in centiseconds (10 ms units). 0 means no effect. The
 * floor keeps the blink far below the 15-25 Hz photosensitive-seizure
 * band: 13 cs = 130 ms period ≈ 7.7 Hz maximum. */
#define NETWORK_MODE_BLINK_MIN_CS 13U

/* Effect duty in percent. 0 means the 50% default; explicit values are
 * bounded 20-80 — except exactly 100, meaning SOLID (effect disabled by
 * duty). 81-99 stay invalid (imperceptible-flicker territory). For the
 * hard wave, duty is the on-fraction; for the fade wave it is the
 * minimum-brightness floor the breathe descends to. */
#define NETWORK_MODE_DUTY_DEFAULT_PCT 50U
#define NETWORK_MODE_DUTY_MIN_PCT 20U
#define NETWORK_MODE_DUTY_MAX_PCT 80U
#define NETWORK_MODE_DUTY_SOLID_PCT 100U

enum network_mode_wave {
	NETWORK_MODE_WAVE_HARD = 0,   /* square on/off */
	NETWORK_MODE_WAVE_FADE = 1,   /* raised-cosine breathe */
};

/* Party-accent strength in percent, 0-100. Unlike duty, 0 is a REAL
 * setting here (accent off) rather than "use the default", because a dark
 * ring is a legitimate thing to ask for — so a record that predates v4
 * cannot be distinguished by value and is identified by accent_known
 * instead, and decodes to FULL. That keeps every pre-v4 fleet rendering
 * exactly as it does today. */
#define NETWORK_MODE_ACCENT_FULL_PCT 100U
#define NETWORK_MODE_ACCENT_OFF_PCT 0U

struct network_mode_value {
	enum network_mode mode;
	uint32_t priority;
	/* Since v3 the blink effect belongs to the REGISTER, not to ultra:
	 * 0 = no effect (solid, the default in every mode), else the
	 * period in centiseconds for whatever mode is active. */
	uint8_t ultra_blink_cs;
	uint8_t ultra_duty_pct;   /* 0 = default */
	uint8_t wave;             /* enum network_mode_wave */
	/* Party accent strength, 0-100 (0 = off, 100 = the historical
	 * full-strength swell). Only meaningful in party/ultra; normal
	 * mode never renders an accent at all. */
	uint8_t accent_pct;
	/* Provenance: which fields the record's wire era could actually
	 * carry (v1 = none, v2 = duty, v3 = duty+wave, v4/local = all). A record
	 * that could not know a field never wins a TIE against one that
	 * does — otherwise a legacy node's echo of the current winner,
	 * back-filled with its own stale duty/wave, reverts a deliberate
	 * setting fleet-wide. Priority always beats provenance, so an
	 * operator on an old node keeps full control. */
	bool duty_known;
	bool wave_known;
	bool accent_known;
};

bool network_mode_blink_valid(uint8_t blink_cs);
float network_mode_blink_period_s(uint8_t blink_cs);
bool network_mode_duty_valid(uint8_t duty_pct);
float network_mode_duty_fraction(uint8_t duty_pct);
bool network_mode_wave_valid(uint8_t wave);
const char *network_mode_wave_name(uint8_t wave);
bool network_mode_accent_valid(uint8_t accent_pct);
/* 0.0-1.0 multiplier applied to the party accent swell. */
float network_mode_accent_fraction(uint8_t accent_pct);

enum network_mode_apply_result {
	NETWORK_MODE_UNCHANGED,
	NETWORK_MODE_ADOPTED,
	NETWORK_MODE_REJECTED,
	NETWORK_MODE_STORAGE_ERROR,
};

bool network_mode_valid(enum network_mode mode);
const char *network_mode_name(enum network_mode mode);

/* Positive means candidate wins. Priorities compare with wrap-around
 * serial-number arithmetic (RFC 1982 style): the signed difference decides,
 * so "current priority + 1" wins forever and the space never exhausts —
 * even a huge accidental priority is recoverable by continuing forward.
 * Equal priorities deliberately prefer the calmer (lower-valued) mode,
 * giving every node the same conservative answer to a conflicting
 * command: normal beats party beats ultra. */
int network_mode_compare(const struct network_mode_value *candidate,
			 const struct network_mode_value *current);

int network_mode_encode_v1(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value);
int network_mode_encode_v2(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value);
int network_mode_encode_v3(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value);
int network_mode_encode_v4(uint8_t *wire, size_t capacity,
			   const struct network_mode_value *value);
bool network_mode_decode(struct network_mode_value *value,
			 const uint8_t *wire, size_t length);

int network_mode_init(uint32_t node_id);
struct network_mode_value network_mode_get(void);
enum network_mode_apply_result
network_mode_set(enum network_mode mode, uint32_t priority,
		 uint8_t ultra_blink_cs, uint8_t ultra_duty_pct,
		 uint8_t wave, uint8_t accent_pct);

/* Called by sync's RX thread for an auxiliary gossip datagram. */
bool network_mode_receive(const uint8_t *wire, size_t length);

/* Periodic anti-entropy plus a short jittered burst after a new winner. */
void network_mode_tick(bool attached);
