#ifndef THREAD_RING_USB_DISPLAY_H_
#define THREAD_RING_USB_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#define USB_DISPLAY_RECONNECT_MS 20000

/* USB VBUS is the only charger-related signal available to the MCU. The
 * policy therefore describes the display while USB is present; it does not
 * claim that battery current is flowing. */
struct usb_display_policy {
	bool usb_present;
	bool quiet;
	bool saw_disconnect;
	int64_t last_disconnect_ms;
};

void usb_display_init(struct usb_display_policy *policy, int64_t now_ms,
		      bool usb_present);

/* Returns true only when the observed USB edge changes display policy. */
bool usb_display_update(struct usb_display_policy *policy, int64_t now_ms,
			bool usb_present);

#endif /* THREAD_RING_USB_DISPLAY_H_ */
