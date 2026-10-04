# Rev A SuperMini socket and battery-power erratum

_Recorded 2026-07-22 from first-board bring-up._

## Summary

Rev A was laid out with two 1x12 sockets. The tested SuperMini nRF52840 has
13 positions on each side: an extra B- pin on the left and B+ pin on the
right precede the 12 Pro Micro-compatible positions at the USB end.

If B-/B+ are inserted into the first socket holes, every module connection is
shifted by one position. On the right side this produces the critical mapping:

| Carrier socket | Intended module pin | Misaligned module pin |
|---|---|---|
| J4-1 (RAW/J2+) | RAW | B+ |
| J4-2 (GND/J2-) | GND | RAW |

The module has a low-impedance power path between B+ and RAW, so this
misalignment effectively places RAW on carrier ground. A 4 V bench supply
immediately entered current limit. The short appeared as soon as the pins
engaged, was absent from the bare module and bare carrier, and could change
with PCB flex.

## Confirmed Rev A installation and operation

For USB or current-limited bench testing only:

1. Leave the module's B- and B+ positions hanging outside the sockets at the
   USB end.
2. The first socketed pins must be P0.06 on J3 and RAW on J4.
3. Before applying power, verify that J2+ to J2- is not shorted.
4. For bench power, use J2 as RAW/GND at 4.0 V with a 0.5 A current limit.

With that alignment, bring-up confirmed all of the following:

- The carrier's manual EN_LED jumper turns on D3.
- The module drives EN_LED from P0.17 with the ring-test firmware.
- D3 and the attached WS2812 ring turn on.
- USB power and USB firmware operation work.

## EN_LED power-switch contract

D3 is connected to switched VLED, so it stays dark when input power is present
but EN_LED is low. This initially looked like a failed power path; a manual
jumper driving P0.17/J3-5 high lit D3 and proved F1, Q1, Q2, the VLED rail, and
the indicator path.

The firmware contract is therefore mandatory:

1. Configure P0.17 as an active-high output with an inactive initial state.
2. Drive P0.17 high to turn on Q2/Q1 and VLED.
3. Wait for the switched rail to settle before transmitting WS2812 data. The
   current local firmware uses 10 ms.
4. Before turning the rail off or rebooting, send black where possible, hold
   the data path low, and then drive P0.17 low. This avoids stale pixels and
   parasitic powering through DIN.

The local ring-test and thread-ring changes add an `led-power` devicetree alias
for P0.17, enable it at application startup, and disable it on controlled DFU
or reboot paths. These changes were hardware-verified during this bring-up but
were still uncommitted when this erratum was written.

## Battery safety limitation

Do not connect a rechargeable battery to J2 on Rev A. J2+ is connected to
RAW, but the tested module exposes a separate B+ battery/charger terminal.
A battery must not be connected to RAW or directly bridged between RAW and
B+, because that can bypass the module's intended charging and reverse-current
paths when USB is present.

Rev A should be treated as USB-powered or current-limited bench-powered
hardware unless a specific rework is designed and electrically validated.
Any rework must isolate the existing J2+-to-RAW route before repurposing J2;
simply adding a B+-to-RAW jumper is not acceptable.

## Silkscreen erratum

The header pin labels are placed too close to the board edge and print partly
off the PCB, so some are clipped or unreadable. The next revision must pull all
header/connector labels fully inside the board outline with adequate silkscreen-
to-edge clearance, and re-check them after the one-position pin shift renumbers
the sockets.

## Requirements for the next carrier revision

1. Replace both 1x12 module sockets/footprints with the tested module's full
   1x13 footprint while preserving enclosure and USB-C clearance.
2. Assign the USB-end extra positions explicitly:
   - left position 1: B- / ground
   - right position 1: B+ / battery positive
   - left position 2: P0.06
   - right position 2: RAW
3. Route J2+ to B+ and J2- to B-/GND. Preserve connector polarity markings.
4. Keep RAW and B+ as separate nets. Never rely on them being the same pin.
5. Create a reverse-current-safe source mux for the fused LED/input rail:
   - RAW supplies it during USB operation;
   - B+ supplies it during battery operation;
   - USB must not bypass the charger into B+;
   - battery power must not backfeed USB/RAW.
   A pair of correctly oriented Schottky diodes or a validated ideal-diode
   solution are candidates; confirm voltage drop and current capacity for the
   LED load before selection.
6. Power U2 and the Q1/F1 LED path from the muxed rail, not directly from an
   ambiguous RAW/B+ net.
7. Sense battery voltage from B+, with the divider output still routed to
   P0.31. Update connector pin numbers after the one-position shift.

   **MEASURED 2026-08-30: there is no divider on P0.31 today — this item is
   genuinely required, not a nice-to-have.** A firmware build with AIN7
   sensing (ADC channel + `vbatt` node, commit `a544564`) was flashed to
   `dusty-lotus-shine` and read:

   | Power source | `bat` at the pin |
   |---|---|
   | USB (`usb=1`) | 221-254 mV |
   | Battery only (`usb=0`, running off a charged LiPo) | 250-258 mV |

   Identical either way. A 1:2 divider on a ~4 V cell would read ~2000 mV and
   would move when the supply changed; this does not move at all, so P0.31 is
   floating and the ~250 mV is ADC input bias. These are SuperMini nRF52840
   clones, not genuine nice!nano — do not assume the nice!nano's onboard
   divider is populated.

   Consequence: **battery percentage is unobtainable on current hardware by
   any firmware change.** It needs this Rev B divider (or a per-module rework).
   The firmware side is proven and cheap (+2.3 KB flash, +221 B RAM) and can
   be re-enabled the day a divider exists.
8. Preserve the MCU signal assignments P0.06 (LED data), P0.17 (EN_LED), and
   P0.20 (button); only their socket position numbers change.
9. Add an explicit pull-down on the Q2/EN_LED gate so the LED rail remains off
   while the MCU pin is high-impedance during reset or module removal. Validate
   the value against P0.17 drive and leakage.
10. Validate that DIN remains low while VLED is off. Add a hardware pull-down
    or choose a buffer with explicit output-enable control if firmware-only
    sequencing cannot guarantee this across reset and module-removal states.
11. Move all header/connector silkscreen labels fully inside the board outline
   with adequate edge clearance so nothing is clipped off the PCB; verify after
   the pin-position renumbering.
12. Update the schematic, PCB silkscreen, README, assembly drawings, BOM/CPL,
   enclosure fit reference, and continuity-test documentation together.

## Rev B design decisions (locked 2026-07-22)

Concrete choices made against the requirements above, grounded in the Rev A
schematic/PCB (board 80.1 x 46.1 mm, 2-layer; sockets vertical, USB end at top,
antenna keepout at bottom). Ring load is the 16-element WS2812B ring on J1.

- **Sockets:** J3/J4 change from `PinSocket_1x12_P2.54` (LCSC C7509522) to a
  1x13 footprint. Board grows ~2.5 mm taller (46.1 -> ~48.6) since pin 1 is
  already ~2.7 mm from the top edge; needs a real placement pass, not an
  in-place footprint swap.
- **Net split:** new `B_PLUS` net (J4/1, J2/1, D2 anode, divider top R2/1) kept
  separate from `RAW` (J4/2, D1 anode). RAW no longer touches J2 or the divider.
- **Source mux:** dual Schottky OR-ing into a new `VIN_MUX` node -> F1 -> VLED_F.
  D1 anode = RAW, D2 anode = B_PLUS, both cathodes = VIN_MUX. Part: SS34 (LCSC
  C8678, Basic, SMA, 3A/40V). Diodes only source the rail, never inject into
  RAW/B+, so they are safe even with the module's internal B+/RAW coupling.
  Accepted trade: on battery the LED rail = B+ - Vf; at dim/capped currents Vf
  ~0.2 V so a 3.7 V cell gives ~3.5 V at the LEDs (workable). Mild dimming/color
  shift only in the bottom ~20% of the cell; revisit a low-Vf/ideal-diode
  battery leg only if bring-up shows it is objectionable.
- **Battery:** design around the Adafruit 1200 mAh 1S protected LiPo
  (62x34x5 mm, JST-PH). Runtime on the 16-LED ring: ~6-13 h moderate dim,
  up to ~18 h very dim; ~1-2 h if run bright (do not).
- **J2 polarity swap:** Rev A has pin 1 = + / pin 2 = -. Rev B swaps to
  pin 1 = GND/B- (mark -), pin 2 = B+ (mark +) to match the Adafruit JST-PH
  convention (reversed from generic/SparkFun). J2+ routes to module B+, never
  RAW. MANDATORY GATE: buy one exact battery and continuity-check its red/black
  leads against the final footprint before fabrication.
- **Charging (Option B):** rely on the module's single onboard charger; raise
  its rate to ~300-500 mA by changing the module's charge-current PROG resistor
  (per-board module rework, not a carrier change). No carrier-side charger -
  two chargers on one cell would conflict. TODO: identify the module's charge IC
  and PROG resistor to spec the exact value. At the default ~100 mA the 1200 mAh
  pack takes >12 h to charge, which is impractical for daily wear.
- **Guard resistors (0603):** R6 100k EN_LED->GND (Q2 gate pull-down, item 9);
  R7 ~22k LED_DIN->GND (item 10); R8 ~100k LED_DATA_MCU->GND so U2 actively
  drives DIN low during MCU reset / module removal. U2 stays on the always-on
  muxed rail per item 6; the residual "rail on but LEDs commanded off" case
  remains a firmware black-frame concern by nature.
- **Fuse:** retain the 1.1 A PTC (F1). 16 LEDs at full white is ~0.96 A, already
  under the fuse. The 16-pixel layout's 45% default is a runtime/comfort
  choice, not a protection one; the experimental 7-pixel Jewel layout can use
  its 100% default while remaining well below the original ring's load. A
  `rope100` assembly is outside this carrier power budget: power its strip from
  a suitably rated external 5 V USB battery path, share ground with the
  controller, and do not route the 100-pixel load through module VCC or F1.
  Firmware defaults `rope100` to 100%, whose full-white theoretical demand is
  about 6 A; size the battery, conductors, connectors, and power injection for
  that load.

## Validation gates for the revised board

- Unpowered continuity test with the module fully seated: no short between
  B+, RAW, VCC, and GND except paths explicitly documented by the module.
- Verify correct 13-position alignment visually and by pin-to-pad continuity.
- USB-only: verify RAW, B+, VCC, mux output, F1 input, VLED, charging voltage,
  and no battery-connector overvoltage.
- Battery-only: verify MCU boot, RAW behavior, mux output, VLED, and ring load.
- USB plus battery: measure charging current/termination and confirm that LED
  current does not flow through or bypass the charger.
- Check USB-to-battery and battery-to-USB reverse current.
- Exercise EN_LED off/on, worst-case intended LED current, source transitions,
  polyfuse behavior, and thermal rise.
- Repeat polarity and current-limit checks with the actual production battery
  pigtail before approving assembly.
