#ifndef THREAD_RING_LED_LAYOUT_H_
#define THREAD_RING_LED_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

#define LED_LAYOUT_MAX_PIXELS 100

enum led_layout {
	LED_LAYOUT_RING16 = 0,
	LED_LAYOUT_JEWEL7 = 1,
	LED_LAYOUT_ROPE100 = 2,
};

bool led_layout_valid(enum led_layout layout);
const char *led_layout_name(enum led_layout layout);
uint8_t led_layout_pixel_count(enum led_layout layout);
uint8_t led_layout_default_brightness(enum led_layout layout);
bool led_layout_uses_usb_quiet_display(enum led_layout layout);

#endif
