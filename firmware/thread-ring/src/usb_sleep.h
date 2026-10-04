#ifndef THREAD_RING_USB_SLEEP_H_
#define THREAD_RING_USB_SLEEP_H_

#include <stdbool.h>
#include <stdint.h>

#define USB_SLEEP_MIN_PULSE_MS 250
#define USB_SLEEP_MAX_PULSE_MS 8000
#define USB_SLEEP_SEQUENCE_MS 20000

enum usb_sleep_event {
	USB_SLEEP_NONE,
	USB_SLEEP_ARMED,
	USB_SLEEP_CANCELLED,
	USB_SLEEP_ENTER,
};

struct usb_sleep_policy {
	bool usb_present;
	uint8_t knocks;
	int64_t attached_ms;
	int64_t deadline_ms;
};

void usb_sleep_init(struct usb_sleep_policy *policy, int64_t now_ms,
		    bool usb_present);

/* A knock is a 250 ms to 8 s USB-powered pulse. Two knocks, completed within
 * twenty seconds and ending with VBUS absent, request System OFF. */
enum usb_sleep_event usb_sleep_update(struct usb_sleep_policy *policy,
				      int64_t now_ms, bool usb_present);

#endif /* THREAD_RING_USB_SLEEP_H_ */
