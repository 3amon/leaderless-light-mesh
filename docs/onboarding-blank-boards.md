# Onboarding a blank board

Recorded 2026-08-23 after onboarding five nice!nano boards, with the failure
modes hit along the way. `pi-tools/onboard_watch.py` automates this; run it
with `bash /home/pi/onboard-watch/start.sh` on the border-router Pi.

Everything below runs **on the Pi**, with the board attached to it. The laptop
is only needed to build firmware.

## Order

Confirmed from the canonical 2026-08-11 onboarding evidence
(`/home/pi/onboard-new-ring-*/evidence/`, ordered by mtime):

1. **Bootstrap** — board in UF2 bootloader mounts as `NICENANO`; copy the
   release's `thread-ring.combined.uf2` onto it.
2. **Join** — reboots, derives its 3-word name, joins ring-net (~10-60 s).
3. **Inventory** — name, address, usb_serial, `"canary": false`, next
   `rollout_order`.
4. **Control key** — before anything needing authenticated control.
5. **OTA gate** — `deploy.py`, the transactional active+confirmed check.
6. **Geometry** — `ringctl.py <name> layout <ring16|jewel7|rope100>`.

The control key must precede the OTA gate: `deploy.py` authenticates.

## Choosing geometry per board

The watcher reads the layout for the *next* board from
`/home/pi/onboard-watch/layout`, so a run of jewel7s can be slipped in between
ring16s without restarting it:

```sh
echo jewel7 > /home/pi/onboard-watch/layout
```

Missing or unrecognised content falls back to `ring16` and logs
`layout_invalid`, rather than passing junk to `ringctl` or silently giving a
board the wrong geometry. The chosen value appears in the `onboarding` and
`onboard_complete` events, so each board's evidence records what it was built
as.

From the firmware's `led_layout.c`:

| Layout | Pixels | Default brightness | USB-quiet display |
|---|---|---|---|
| `ring16` | 16 | 45% | yes |
| `jewel7` | 7 | 100% | yes |
| `rope100` | 100 | 100% | **no** |

`rope100` is the only layout exempt from the USB-quiet display, so a jewel7 on
USB power sits green and dark exactly like a ring16 does.

Geometry is a runtime setting, not a build-time one — the devicetree allocates
`chain-length = 100` (`LED_LAYOUT_MAX_PIXELS`) regardless, and `ringctl` picks
the logical length. One firmware image serves all three.

## Bootstrap artifact

`dist/vX.Y.Z/provenance.json` records `recovery_uf2.image.version`, and for
v0.1.83 that is the release's own `thread-ring.combined.uf2` — it covers
`0x26000..0x90000`, which is MCUboot (`0x26000-0x32000`), slot 0
(`0x32000-0x8F000`), **and slot 1's first 4 KB**. That last part matters: it
wipes slot 1's header, so a stale candidate cannot trigger a swap on boot.

Do not substitute `quick_update.py`'s artifact here — and note `quick_update.py`
requires the board to already be in `rings.json`, so it cannot bootstrap a board
that has never joined.

## MCUboot qualification is currently SKIPPED

`mcuboot_update.py` is not run by the watcher. The only reviewed profile on the
Pi, `/home/pi/releases/mcuboot-v0145/profile.json`, has a **v0.1.44** recovery
artifact, and that image **does not boot on this hardware batch**: on
2026-08-23 it took a clean, complete write on `feral-dune-haze` and never
enumerated again. Recovery was double-tap DFU plus rewriting v0.1.83.

This does **not** mean the boards run a different bootloader.
`dist/v0.1.83/provenance.json` pins `bootloader/mcuboot = 2d61c318` (v2.2.0)
plus `patches/mcuboot-watchdog-trace.patch`, identical to
`config/toolchain.lock.json`. Same MCUboot version and patch set as the
reviewed baseline. What is skipped is the qualification **transaction**
(verified install, slot-1 erase, transactional forward deploy with evidence).

To re-enable: build a profile whose `recovery` artifact is known to boot on
this batch, with `forward` at a higher version — which needs a build above
0.1.83, since that is currently the top of the tree. `rollback` is only
required with `--qualification`, and the canonical run used
`qualification: false`.

## Failure signatures

| Symptom | Cause | Action |
|---|---|---|
| `classification: rollback`, `reset=00000012` | `RESET_WATCHDOG\|RESET_SOFTWARE`, reboot reason `none` — watchdog fired mid-swap. A healthy OTA shows `reset=00000002/ota` (`RESET_SOFTWARE`, reason `ota`). | Pre-existing; see `20260815-mcuboot-regression/`. Board is fine, OTA path is not. |
| `classification: reboot_loss`, `ambiguous: true`, `status=None` | Device never answered after reboot. | Check whether it booted at all; may escape to DFU. |
| Board cycles DFU → flashed → DFU | Truncated write. **First write ~13.7 s, retries ~2.4 s** — too fast for 868 KB to nRF52 flash. Partial image cannot boot, boot-recovery ladder escapes to DFU, watcher reflashes. | Bounded by `MAX_FLASH_ATTEMPTS`; `flash_loop_quarantined`. A complete write does eventually resolve it. |
| Zero USB kernel events on plug-in | Power or data, not firmware. Broken firmware still enumerates *then* misbehaves. | Cable or port. Double-tap reset always restores `NICENANO`. |
| `target/sentinel/baseline preflight failed` at ~5.7 s | The 5 s `backend.status()` timeout expiring; the mesh is intermittently flaky. | Retry. |

## Why a freshly provisioned ring looks dead

Almost always one of two mundane things:

- **USB power is present.** `usb_display.c` sets `quiet = usb_present`, and the
  render loop does `if (usb_quiet_display_active()) { render_usb_quiet(now);
  continue; }` — that `continue` skips *all* pattern rendering. You get three
  green pulses then `green = 5/255`: visually black, and easily mistaken for a
  "charging" indicator. Applies to every layout except `rope100`.

  The flag comes from `usb_vbus_present()`, i.e. **VBUS**, not host enumeration
  — so a dumb USB charger triggers quiet mode exactly like the Pi does. The
  only ways to get normal rendering are to unplug and replug within **20 s**
  (`USB_DISPLAY_RECONNECT_MS`, which sets `quick_reconnect` and clears quiet
  while staying on USB), or to run it off battery with no VBUS at all.

- **It is unpowered.** These boards are USB-powered; unplugging from the Pi
  takes them off the mesh *and* kills the LEDs.

`pat=-1` / `"searching"` is a normal resting state, not damage — roughly a
third of the fleet sits there. Confirm the render loop is alive by checking
`frames=` is climbing.

## Command gotchas

- `provision_control_key.py` takes `--target <name>`, **not** a positional.
  Use a generous `--timeout`; the confirmation wait is slow on a freshly
  booted board. Re-running never regenerates an existing key (`O_EXCL`, then
  load), so retries are safe.
- `deploy.py` and `mcuboot_update.py` need `--smpmgr /home/pi/.local/bin/smpmgr`
  **under sudo**: the default is `Path.home()/.local/bin/smpmgr`, which becomes
  `/root/...` and does not exist.
- `mcuboot_update.py` requires `--allow-disruptive`, and a `--sentinel` that is
  a *different inventoried ring* — not the Pi. The border router being up only
  proves the border router is up; a second ring still answering over Thread
  separates "target broke" from "mesh hiccuped", which is what keeps the
  transaction fail-closed.
- `identify_target.py`'s exit status is a **classification**, not
  success/failure: `0` one known target, `1` none attached, **`2` unknown or
  ambiguous**, `3` error. Treating `2` as failure silently discards valid JSON
  for exactly the case onboarding cares about.
- A detached watcher cannot use `udisksctl` — polkit needs a controlling
  terminal. Mount with `sudo mount -t vfat -o uid=,gid=`.
