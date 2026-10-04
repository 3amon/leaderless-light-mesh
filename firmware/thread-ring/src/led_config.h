#ifndef THREAD_RING_LED_CONFIG_H_
#define THREAD_RING_LED_CONFIG_H_

#include "led_layout.h"

/* Load the per-device layout from the existing settings/NVS partition.
 * A new or invalid store safely defaults to the original 16-pixel ring. */
int led_config_init(void);
enum led_layout led_config_layout(void);

/* Persist before publishing the new value. An unchanged value is a no-op. */
int led_config_set_layout(enum led_layout layout);

#endif
