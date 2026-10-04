/* "This node is deliberately alone" — persistent across reboots.
 *
 * heal_policy assumes a lone node may have a wedged radio and restarts it to
 * rejoin, escalating 1/3/10/30 minutes. That is right for a fleet and
 * permanently wrong for a single-node install: there is nobody to rejoin, so
 * every restart finds the same solitude and the node reboots on the backoff
 * forever, losing any RAM-only state with it.
 *
 * This has to persist, because the condition it describes survives the reboot
 * it prevents — a flag that reset on boot would be cleared by the very restart
 * it exists to stop.
 *
 * It suppresses ONLY the solitude restart. Watchdog recovery, OTA confirmation
 * and every other reset path are untouched.
 */

#pragma once

#include <stdbool.h>

/* Loads the stored flag; defaults to false (fleet behaviour) when absent or
 * corrupt, so an unprovisioned node keeps the safer self-healing default. */
int solo_mode_init(void);

bool solo_mode_get(void);

/* Persists immediately: the next boot must see it. */
int solo_mode_set(bool solo);
