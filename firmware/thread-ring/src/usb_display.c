#include "usb_display.h"

void usb_display_init(struct usb_display_policy *policy, int64_t now_ms,
		      bool usb_present)
{
	*policy = (struct usb_display_policy) {
		.usb_present = usb_present,
		.quiet = usb_present,
		.saw_disconnect = false,
		.last_disconnect_ms = now_ms,
	};
}

bool usb_display_update(struct usb_display_policy *policy, int64_t now_ms,
			bool usb_present)
{
	if (usb_present == policy->usb_present) {
		return false;
	}

	policy->usb_present = usb_present;
	if (!usb_present) {
		policy->quiet = false;
		policy->saw_disconnect = true;
		policy->last_disconnect_ms = now_ms;
		return true;
	}

	int64_t disconnected_ms = now_ms - policy->last_disconnect_ms;
	bool quick_reconnect = policy->saw_disconnect && disconnected_ms >= 0 &&
			       disconnected_ms <= USB_DISPLAY_RECONNECT_MS;

	policy->quiet = !quick_reconnect;
	return true;
}
