#include "led_layout.h"

bool led_layout_valid(enum led_layout layout)
{
	return layout == LED_LAYOUT_RING16 || layout == LED_LAYOUT_JEWEL7 ||
	       layout == LED_LAYOUT_ROPE100;
}

const char *led_layout_name(enum led_layout layout)
{
	switch (layout) {
	case LED_LAYOUT_JEWEL7:
		return "jewel7";
	case LED_LAYOUT_ROPE100:
		return "rope100";
	default:
		return "ring16";
	}
}

uint8_t led_layout_pixel_count(enum led_layout layout)
{
	switch (layout) {
	case LED_LAYOUT_JEWEL7:
		return 7U;
	case LED_LAYOUT_ROPE100:
		return 100U;
	default:
		return 16U;
	}
}

uint8_t led_layout_default_brightness(enum led_layout layout)
{
	switch (layout) {
	case LED_LAYOUT_JEWEL7:
		return 100U;
	case LED_LAYOUT_ROPE100:
		return 100U;
	default:
		return 45U;
	}
}

bool led_layout_uses_usb_quiet_display(enum led_layout layout)
{
	return layout != LED_LAYOUT_ROPE100;
}
