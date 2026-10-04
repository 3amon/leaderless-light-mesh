# ADR: boot-recovery ladder — guaranteed buttonless USB recovery

Status: implemented; USB-power escape path HARDWARE-QUALIFIED 2026-08-15 on
`drifting-owl-spark` (fleet board, bench use approved by operator). The
deliberately wedging `CONFIG_THREAD_RING_ESCAPE_HIL_BOOT_HANG` image
self-presented as a NICENANO drive **51 s** after reset (three watchdog
cycles + escape on attempt 4), recovery was one file copy onto that drive,
and the board returned healthy/confirmed in 5.7 s with `ladder=1`
steady-state telemetry. Battery park (System OFF) remains unqualified on
hardware. Wedge evidence: a findings note archived with
the flash snapshot (CURRENT.UF2 sha256 `41010d57…`) at
`/home/pi/deployments/20260815-drifting-owl-wedge-forensics/` on the Pi.

The first bench run failed and exposed a cross-image ABI landmine now fixed
and regression-locked: the native UF2 bootloader runs on every reset with
its initial MSP at the very top of SRAM (0x20040000, read from the
nice!nano 0.6.0 release vector table) and an 8 KiB stack growing down, so
retained state at 0x2003fff0 was trampled every boot and the attempt count
never left 1. The ladder therefore lives at 0x2003d000, at the base of a
reserved 12 KiB region covering the bootloader stack plus margin.

## Problem

A node that wedges before its USB stack comes up is unrecoverable without
opening the enclosure and jumpering into the native bootloader. Every
buttonless path into UF2 mode today (1200-baud touch, `ring dfu`, the sleep
gesture) requires a *running, cooperating application*.

The drifting-owl-spark wedge proved the failure class is real and is not an
OTA-flash-corruption problem: MCUboot behaved perfectly (clean revert, fully
consistent trailers), yet the board was silently bricked for days because the
reverted application hung in its **watchdog-unprotected early-boot window**.
Two specific holes combined:

1. After a watchdog reset, the nRF52 WDT is disabled. MCUboot runs and hands
   off unprotected; the application re-arms the WDT only after
   `led_power_prepare` + three settings/NVS loads + `control_auth_init`.
   A hang anywhere in that window is eternal and silent.
2. Nothing counts failed boot attempts, so no amount of failing ever changes
   behavior. The node retries the identical wedge forever instead of
   escalating to a recoverable state.

## Decision

Add a **boot-progress ratchet** with an **escape hatch into the native UF2
bootloader**, plus watchdog coverage of the entire boot chain.

### 1. Retained boot-attempt counter

A `.noinit` RAM word (magic + attempt counter + inverted-copy check; random
power-on values rejected, same pattern as the GPREGRET2 discipline in
`main.c`). Semantics:

- MCUboot increments it at entry (extension of
  `patches/mcuboot-watchdog-trace.patch`).
- The application clears it only on reaching *provably recoverable* state:
  USB control plane initialized + N seconds of uptime. Deliberately NOT gated
  on Thread attach — solitude is normal, wedge is not.

### 2. Watchdog covers the whole chain

- MCUboot arms the WDT itself if it is not already running (the
  after-WDT-reset case — exactly the drifting-owl hole) before validation,
  keeping the existing feed points.
- The application's first act in `main()` is to arm/feed the WDT — before
  any settings/NVS I/O, LED prepare, or auth init. Stage markers move
  accordingly.

Result: no instruction between reset vector and the main supervision loop
runs unprotected, so "hang" always becomes "reset + counter increment"
within 15 s.

### 3. Escape policy (pure module, host-tested)

At MCUboot entry with counter ≥ N (default 4):

- **VBUS present** → write `GPREGRET = 0x57` (`DFU_MAGIC_UF2_RESET`) and
  reset: the node comes up as the native `NICENANO` UF2 drive. Recovery is
  "plug it into anything and copy the release UF2" — no jumper, no
  disassembly, and the drive exposes `CURRENT.UF2` for forensics first.
- **On battery** → do NOT strand the node in the bootloader (it would drain
  with no recovery path). Park: System OFF with VBUS as wake source,
  retrying the application on wake / at bounded intervals. Plugging in USB
  is what unlocks the escape on the next failed attempt.

The counter and escape decision live in a pure `escape_policy.c` shared
verbatim between MCUboot and application builds, with host tests.

### 4. Why this is sufficient

OTA writes only slots 0/1. MCUboot and its escape logic are written
exclusively over USB, so the escape code cannot be corrupted by any OTA
outcome. Below it, the read-only native bootloader still honors the magic.
The remaining brick surface is physical flash failure or corruption of the
MCUboot region itself — both already require the jumper today, and the
region is rewritten by every guarded recovery UF2.

## Verification gates (bench, raw devboard, before any fleet exposure)

The faulting image is built from clean source with
`CONFIG_THREAD_RING_ESCAPE_HIL_BOOT_HANG=y` (an `EXTRA_CONF_FILE` or `-D`
on a normal signed build); it hangs right after the early watchdog arm.

1. Deliberately faulting test image (crash at the old pre-watchdog point):
   with USB attached, board must self-present as `NICENANO` within
   N × (hang-to-WDT) time; recover with the release UF2; boot trail must
   show the attempt count.
2. Same image on battery power: board must park in System OFF, then escape
   on USB insertion + next failure.
3. Watchdog inheritance in the native bootloader: enter UF2 mode with a
   running WDT and prove the drive survives past the timeout (bootloader
   feed), or force the escape path through a WDT reset (which disables the
   WDT) and prove that ordering.
4. Escape magic written from MCUboot context (not just the app) reaches UF2
   mode — GPREGRET bit-7 collision rules with the breadcrumb trace hold.
5. Full existing gate: 202 host tests, native_sim suite, fleet build size
   budget, forward/revert OTA qualification unchanged.

## Rollout

MCUboot cannot update itself over Thread. The new MCUboot rides the guarded
`0x26000..0x90000` quick-update/recovery UF2, so each board picks it up at
its next routine USB touch; `config/rings.json` tracks which boards carry the
escape-capable bootloader. Application-side changes ship as a normal signed
OTA release. No dedicated fleet-wide physical campaign is required.

## Follow-ups folded into this thrust

- OTA arm gate: require VBUS or (Rev B, once ADC exists) battery voltage
  above threshold before arming a swap; hold LED rails dark from arm to
  confirm.
- Unconfirmed first boots emit an early "alive, version X, unconfirmed"
  beacon so "armed then silent" is diagnosable instead of ambiguous.
- native_sim cross-version regression: boot release N against a settings
  partition written by release N+1 (the reverted-image case drifting-owl
  hit and v0.1.67 evidently did not survive).
