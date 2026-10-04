#ifndef THREAD_RING_BOOT_STATUS_H_
#define THREAD_RING_BOOT_STATUS_H_

#include <stddef.h>
#include <stdint.h>

enum boot_stage {
	BOOT_STAGE_ENTRY = 1,
	BOOT_STAGE_WATCHDOG,
	BOOT_STAGE_USB_QUIESCED,
	BOOT_STAGE_USB,
	BOOT_STAGE_ATTACHED,
	BOOT_STAGE_INFRASTRUCTURE,
	BOOT_STAGE_CONFIRMED,
};

/* Start a new persistent boot record.  The state is deliberately tiny and
 * stored in the existing settings/NVS partition as one checksummed value. */
int boot_status_init(uint32_t reset_cause, uint8_t reboot_reason,
		     uint8_t mcuboot_phase, uint8_t mcuboot_detail);
int boot_status_record(enum boot_stage stage);
uint32_t boot_status_sequence(void);
/* Consecutive rejoin attempts, persisted because the backoff must outlive the
 * reboot it counts. Setting an unchanged value is a no-op (no flash write). */
uint8_t boot_status_heal_attempts(void);
int boot_status_set_heal_attempts(uint8_t attempts);
enum boot_stage boot_status_stage(void);
const char *boot_status_stage_name(enum boot_stage stage);
/* Escape-ladder failed-attempt count captured at main() entry, reported as
 * `ladder=N` in the boot reply so recovery evidence survives in telemetry. */
void boot_status_note_escape_attempts(uint32_t attempts);
int boot_status_format(char *buf, size_t len);

#endif
