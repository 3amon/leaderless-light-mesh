#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "escape_ladder.h"

/* Host build: the NRF register helpers are compiled out, leaving the pure
 * ratchet policy shared verbatim with MCUboot and the application. */

static void test_garbage_state_reads_as_zero_attempts(void)
{
	struct escape_ladder_state st;

	memset(&st, 0xA5, sizeof(st));
	assert(!escape_ladder_valid(&st));
	assert(escape_ladder_attempts(&st) == 0U);

	/* Power-on RAM that happens to hold the magic but a stale guard must
	 * also be rejected: magic alone is 1-in-2^32, magic+guard is paired. */
	st.magic = ESCAPE_LADDER_MAGIC;
	st.attempts = 3U;
	st.guard = 0U;
	assert(!escape_ladder_valid(&st));
	assert(escape_ladder_attempts(&st) == 0U);
}

static void test_attempts_ratchet_and_clear(void)
{
	struct escape_ladder_state st;

	memset(&st, 0, sizeof(st));
	assert(escape_ladder_note_attempt(&st) == 1U);
	assert(escape_ladder_valid(&st));
	assert(escape_ladder_note_attempt(&st) == 2U);
	assert(escape_ladder_note_attempt(&st) == 3U);
	assert(escape_ladder_attempts(&st) == 3U);

	escape_ladder_clear(&st);
	assert(escape_ladder_valid(&st));
	assert(escape_ladder_attempts(&st) == 0U);
	assert(escape_ladder_note_attempt(&st) == 1U);
}

static void test_counter_saturates_instead_of_wrapping(void)
{
	struct escape_ladder_state st;

	memset(&st, 0, sizeof(st));
	escape_ladder_set(&st, UINT32_MAX);
	assert(escape_ladder_valid(&st));
	assert(escape_ladder_note_attempt(&st) == UINT32_MAX);
}

static void test_below_threshold_always_continues(void)
{
	for (uint32_t attempts = 0; attempts < ESCAPE_LADDER_MAX_ATTEMPTS;
	     attempts++) {
		for (int vbus = 0; vbus <= 1; vbus++) {
			for (int wdt = 0; wdt <= 1; wdt++) {
				assert(escape_ladder_decide(attempts, vbus,
							    wdt) ==
				       ESCAPE_LADDER_CONTINUE);
			}
		}
	}
}

static void test_threshold_with_live_watchdog_waits_for_wdt_reset(void)
{
	/* A soft reset does not stop the nRF WDT: escaping now would kill
	 * the native bootloader's drive mid-recovery. Both power sources
	 * defer identically. */
	assert(escape_ladder_decide(ESCAPE_LADDER_MAX_ATTEMPTS, true, true) ==
	       ESCAPE_LADDER_AWAIT_WDT_RESET);
	assert(escape_ladder_decide(ESCAPE_LADDER_MAX_ATTEMPTS, false, true) ==
	       ESCAPE_LADDER_AWAIT_WDT_RESET);
}

static void test_threshold_on_usb_power_escapes_to_uf2(void)
{
	assert(escape_ladder_decide(ESCAPE_LADDER_MAX_ATTEMPTS, true, false) ==
	       ESCAPE_LADDER_ENTER_UF2);
	assert(escape_ladder_decide(ESCAPE_LADDER_MAX_ATTEMPTS + 7U, true,
				    false) == ESCAPE_LADDER_ENTER_UF2);
}

static void test_threshold_on_battery_parks_instead_of_stranding(void)
{
	assert(escape_ladder_decide(ESCAPE_LADDER_MAX_ATTEMPTS, false,
				    false) == ESCAPE_LADDER_PARK_SYSTEM_OFF);
}

static void test_ladder_survival_semantics(void)
{
	/* Warm resets (watchdog included) retain SRAM: the climb continues.
	 * System OFF and power-on lose SRAM: modelled here as garbage, the
	 * climb restarts — a parked node escapes within MAX_ATTEMPTS boots
	 * of being plugged in, it does not escape instantly. */
	struct escape_ladder_state st;

	memset(&st, 0, sizeof(st));
	for (uint32_t boot = 1; boot < ESCAPE_LADDER_MAX_ATTEMPTS; boot++) {
		assert(escape_ladder_note_attempt(&st) == boot);
	}
	memset(&st, 0x5A, sizeof(st)); /* System OFF wipes the count */
	assert(escape_ladder_note_attempt(&st) == 1U);
	assert(escape_ladder_decide(1U, true, false) ==
	       ESCAPE_LADDER_CONTINUE);
}

static void test_fixed_address_layout_is_stable(void)
{
	/* The address and size are a cross-image ABI shared with the MCUboot
	 * patch and the sram0 carve-outs in the devicetree overlays. The
	 * ladder must sit at the base of the reserved top region, BELOW the
	 * native bootloader's stack (initial MSP 0x20040000, 8 KiB deep,
	 * runs on every reset) — a top-of-RAM ladder gets trampled to
	 * garbage every boot, as the first bench run proved. */
	assert(sizeof(struct escape_ladder_state) == 12U);
	assert(ESCAPE_LADDER_RAM_ADDR == 0x2003d000UL);
	assert(0x20040000UL - ESCAPE_LADDER_RAM_ADDR >= 0x2000UL + 0x1000UL);
	assert(ESCAPE_LADDER_MAX_ATTEMPTS == 4U);
}

int main(void)
{
	test_garbage_state_reads_as_zero_attempts();
	test_attempts_ratchet_and_clear();
	test_counter_saturates_instead_of_wrapping();
	test_below_threshold_always_continues();
	test_threshold_with_live_watchdog_waits_for_wdt_reset();
	test_threshold_on_usb_power_escapes_to_uf2();
	test_threshold_on_battery_parks_instead_of_stranding();
	test_ladder_survival_semantics();
	test_fixed_address_layout_is_stable();
	printf("escape ladder tests passed\n");
	return 0;
}
