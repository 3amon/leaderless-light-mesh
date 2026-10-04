#ifndef THREAD_RING_LED_POWER_MODE_H_
#define THREAD_RING_LED_POWER_MODE_H_

#include <stdbool.h>

enum led_power_mode {
	LED_POWER_CARRIER = 0,
	LED_POWER_MODULE_VCC = 1,
};

bool led_power_mode_valid(enum led_power_mode mode);
const char *led_power_mode_name(enum led_power_mode mode);

/* A missing/invalid record selects the module's switchable VCC output.
 * Valid saved carrier/module-VCC choices continue to take precedence. */
int led_power_mode_init(void);
enum led_power_mode led_power_mode_get(void);
int led_power_mode_set(enum led_power_mode mode);

#endif /* THREAD_RING_LED_POWER_MODE_H_ */
