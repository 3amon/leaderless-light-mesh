#ifndef THREAD_RING_HEAL_POLICY_H_
#define THREAD_RING_HEAL_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/* Rejoin (self-heal) backoff policy.
 *
 * A leader with no neighbours is a singleton partition. That can mean the radio
 * wedged itself, or simply that nobody else is here -- the node cannot tell the
 * two apart and does not try. It assumes a fault first, because the rejoin
 * restart is seamless and therefore cheap to attempt, then concludes that the
 * solitude is real and stops churning.
 *
 * The attempt count is an explicit input and output rather than internal state,
 * because it has to survive the very reboot it counts. Keeping it in the caller
 * forces the question of WHERE it is stored to be answered out loud: an earlier
 * version hid it in a static and silently lost it on every boot, so the
 * interval never escalated past the first step.
 */

#define HEAL_POLICY_STEPS 4

/* Wait before the Nth rejoin attempt, clamped at the final step:
 * 1 min, 3 min, 10 min, then 30 min forever. */
int64_t heal_policy_interval_ms(uint8_t attempts);

struct heal_observation {
	/* How long solitude has lasted. A DURATION rather than (now, started_at):
	 * the pair forced callers to synthesise a start time, and a test harness
	 * backdating one past a freshly-booted uptime produced a negative value
	 * that read as "not alone" and silently declined to act. */
	int64_t lonely_for_ms;
	/* Attempts already made, as recovered from persistent storage. */
	uint8_t attempts;
	bool lonely;
	/* This boot came from a power-on/pin reset: a fresh deployment rather
	 * than the continuation of a retry sequence. */
	bool power_on_reset;
	/* Solitude is the DEPLOYMENT, not a fault. The policy below assumes a
	 * lone node may have a wedged radio, which is right for a fleet and
	 * permanently wrong for a deliberate single-node install: there is no
	 * one to rejoin, so every restart is certain to find the same solitude
	 * and the node reboots on the backoff forever. Set for such installs to
	 * suppress the rejoin restart while leaving every other recovery path
	 * untouched. */
	bool solo_expected;
};

struct heal_decision {
	bool reboot;
	/* Attempt count the caller should persist (unchanged unless it moved). */
	uint8_t attempts;
	/* The interval currently in force, for logging. */
	int64_t interval_ms;
};

struct heal_decision heal_policy_update(const struct heal_observation *obs);

#endif /* THREAD_RING_HEAL_POLICY_H_ */
