/* "This node is permanently powered" — persistent across reboots.
 *
 * A USB-powered node defaults to the quiet charging display (three green
 * pulses, then a very dim glow) because USB VBUS is the only charger-related
 * signal the MCU has, and a flower on a charger should not run its scene.
 * Some installs are powered from USB for good — a mains supply, a bench
 * rail — and for those the quiet display is simply the node never playing.
 *
 * With always-on set, USB presence no longer affects the display: the node
 * renders its normal scene whenever it has power, exactly as the externally
 * powered `rope100` layout already does. Nothing else about USB changes —
 * the knock-to-sleep gesture, the DFU entry and the control refusals that
 * key off VBUS are untouched.
 *
 * Persisted, because the condition is a property of how the node is
 * mounted, not of this boot. Absent or corrupt record means the default
 * (quiet on USB), which is the safer reading for a node whose history is
 * unknown.
 */

#pragma once

#include <stdbool.h>

/* Loads the stored flag; defaults to false when absent or corrupt. */
int always_on_init(void);

bool always_on_get(void);

/* Persists immediately: the next boot must see it. */
int always_on_set(bool always_on);
