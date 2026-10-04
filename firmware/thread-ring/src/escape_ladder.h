#ifndef THREAD_RING_ESCAPE_LADDER_H_
#define THREAD_RING_ESCAPE_LADDER_H_

#include <stdbool.h>
#include <stdint.h>

/* Boot-progress ratchet with a native-bootloader escape hatch.
 *
 * A node that wedges before its USB stack is alive used to be recoverable
 * only by opening the enclosure and jumpering into DFU: every buttonless
 * path into the UF2 bootloader needs a running application. The
 * drifting-owl-spark wedge (see docs/boot-recovery-ladder.md and
 * .diagnostics/drifting-owl-spark-20260815-wedge/) proved the class: after
 * a watchdog reset the nRF WDT is disabled, MCUboot and early application
 * init run unprotected, and a hang there is silent and eternal.
 *
 * The ladder: MCUboot counts every boot attempt in retained RAM; the
 * application clears the count only after proving it is recoverable (USB
 * control plane up plus stable uptime). Too many consecutive failures and
 * MCUboot stops retrying the identical wedge and instead escapes to the
 * native UF2 bootloader (USB power present) or parks in System OFF until
 * USB power arrives (battery). Plugging the node in IS the recovery action.
 *
 * This header is compiled verbatim in three places: the application, the
 * MCUboot patch (patches/mcuboot-watchdog-trace.patch carries an identical
 * embedded copy, enforced by tests/test_escape_ladder.py), and the host
 * test. The pure policy below must therefore stay free of Zephyr and nrfx
 * dependencies; the register helpers are guarded on the MDK being present.
 *
 * State lives at a FIXED address near the top of SRAM, carved out of both
 * images' view of RAM (KEEP IN SYNC: the board overlays and
 * sysbuild/mcuboot.overlay). A fixed address rather than a .noinit section
 * because MCUboot and the application are separate links and must agree.
 * RAM survives every warm reset (watchdog included) but not power-on or
 * System OFF; the magic/guard pair rejects random power-on contents, so a
 * lost ladder simply restarts the climb.
 *
 * The address must sit BELOW the native bootloader's stack: the Adafruit
 * UF2 bootloader runs before MCUboot on every reset with its initial MSP
 * at the very top of SRAM (0x20040000, read from the nice!nano 0.6.0
 * release image's vector table) and an 8 KiB stack growing down. The
 * first bench run proved this the hard way — a ladder at 0x2003fff0 was
 * trampled to garbage on every boot and the count never left 1. The
 * carve therefore reserves 0x2003d000..0x20040000 (ladder + bootloader
 * stack + margin) and the ladder lives at its base.
 */

struct escape_ladder_state {
	uint32_t magic;
	uint32_t attempts;
	uint32_t guard;
};

#define ESCAPE_LADDER_MAGIC 0x4c414452UL /* "LADR" */
#define ESCAPE_LADDER_RAM_ADDR 0x2003d000UL
#define ESCAPE_LADDER_MAX_ATTEMPTS 4U

/* One test boot, one reverted boot, and two more chances: the ladder only
 * escapes after MCUboot's own A/B rollback has had its full say. */

enum escape_ladder_action {
	ESCAPE_LADDER_CONTINUE = 0,
	/* Write the UF2 reset magic and reset: the node becomes a NICENANO
	 * mass-storage drive on the host it is plugged into. */
	ESCAPE_LADDER_ENTER_UF2,
	/* The WDT cannot be stopped once started, and a soft reset does not
	 * stop it either: escaping now would hand the native bootloader a
	 * live watchdog that kills the drive mid-recovery. Spin unfed; the
	 * watchdog reset (<= 15 s) disables the WDT and retains RAM, and the
	 * next MCUboot entry escapes cleanly. */
	ESCAPE_LADDER_AWAIT_WDT_RESET,
	/* On battery, a node stranded in the bootloader would only drain.
	 * System OFF with VBUS wake instead: plugging in USB restarts the
	 * climb and escapes within MAX_ATTEMPTS failures. */
	ESCAPE_LADDER_PARK_SYSTEM_OFF,
};

static inline bool escape_ladder_valid(const volatile struct escape_ladder_state *st)
{
	return st->magic == ESCAPE_LADDER_MAGIC &&
	       st->guard == (ESCAPE_LADDER_MAGIC ^ ~st->attempts);
}

static inline uint32_t escape_ladder_attempts(const volatile struct escape_ladder_state *st)
{
	return escape_ladder_valid(st) ? st->attempts : 0U;
}

static inline void escape_ladder_set(volatile struct escape_ladder_state *st,
				     uint32_t attempts)
{
	st->magic = ESCAPE_LADDER_MAGIC;
	st->attempts = attempts;
	st->guard = ESCAPE_LADDER_MAGIC ^ ~attempts;
}

static inline void escape_ladder_clear(volatile struct escape_ladder_state *st)
{
	escape_ladder_set(st, 0U);
}

static inline uint32_t escape_ladder_note_attempt(volatile struct escape_ladder_state *st)
{
	uint32_t attempts = escape_ladder_attempts(st);

	if (attempts < UINT32_MAX) {
		attempts++;
	}
	escape_ladder_set(st, attempts);
	return attempts;
}

static inline enum escape_ladder_action escape_ladder_decide(uint32_t attempts,
							     bool vbus_present,
							     bool wdt_running)
{
	if (attempts < ESCAPE_LADDER_MAX_ATTEMPTS) {
		return ESCAPE_LADDER_CONTINUE;
	}
	if (wdt_running) {
		return ESCAPE_LADDER_AWAIT_WDT_RESET;
	}
	return vbus_present ? ESCAPE_LADDER_ENTER_UF2
			    : ESCAPE_LADDER_PARK_SYSTEM_OFF;
}

#if defined(NRF_POWER) && defined(NRF_WDT) && defined(NRF_CLOCK)

#define ESCAPE_LADDER_STATE \
	((volatile struct escape_ladder_state *)ESCAPE_LADDER_RAM_ADDR)

/* Must match what the application's Zephyr WDT path expects to inherit:
 * 15 s on reload channel 0. Whoever starts the nRF WDT first locks its
 * configuration; both armers therefore use identical values. */
#define ESCAPE_LADDER_WDT_CRV (15U * 32768U)

static inline bool escape_ladder_wdt_running(void)
{
	return (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) != 0U;
}

static inline bool escape_ladder_vbus_present(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U;
}

/* Arm the 15 s watchdog if nothing already did (the after-watchdog-reset
 * case). Runs in sleep, pauses in debug halt. The WDT counts on LFCLK;
 * request the RC-sourced LFCLK without waiting so a hang before the clock
 * driver still gets a ticking watchdog a few microseconds later. */
static inline void escape_ladder_arm_wdt(void)
{
	if (escape_ladder_wdt_running()) {
		return;
	}
	if ((NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk) == 0U) {
		NRF_CLOCK->TASKS_LFCLKSTART = 1U;
	}
	NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Msk;
	NRF_WDT->CRV = ESCAPE_LADDER_WDT_CRV;
	NRF_WDT->RREN = WDT_RREN_RR0_Msk;
	NRF_WDT->TASKS_START = 1U;
}

static inline void escape_ladder_execute(volatile struct escape_ladder_state *st,
					 enum escape_ladder_action action)
{
	switch (action) {
	case ESCAPE_LADDER_CONTINUE:
		return;
	case ESCAPE_LADDER_AWAIT_WDT_RESET:
		/* Busy spin, never WFE: the locked WDT CONFIG may pause in
		 * sleep, and an active CPU keeps it counting regardless. */
		for (;;) {
		}
	case ESCAPE_LADDER_ENTER_UF2:
		/* Clear first: the freshly flashed image must get a whole
		 * ladder of its own instead of bouncing straight back. */
		escape_ladder_clear(st);
		NRF_POWER->GPREGRET = 0x57U; /* DFU_MAGIC_UF2_RESET */
		NVIC_SystemReset();
		for (;;) {
		}
	case ESCAPE_LADDER_PARK_SYSTEM_OFF:
		escape_ladder_clear(st);
		NRF_POWER->SYSTEMOFF = 1U;
		for (;;) {
		}
	}
}

#endif /* NRF register helpers */

#endif /* THREAD_RING_ESCAPE_LADDER_H_ */
