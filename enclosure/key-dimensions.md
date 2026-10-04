# Key dimensions

The numbers everything else is built on. Most of these are **fixed by physical
parts** and cannot be changed without re-testing fit.

## LED ring (bought part)

DIYmall WS2812B ring, 16 × 5050 addressable RGB LEDs.

| | |
| --- | --- |
| Outer diameter | **45.0 mm** |
| Inner hole | **32.0 mm** |
| PCB thickness | **1.6 mm** |
| LED height above PCB | **1.6 mm** |
| Outer extent of the LEDs | **21.9 mm radius** |
| LEDs sit at roughly | **19 mm radius** |

Wires are soldered to pads and exit the **underside**.

## Diffuser cap — proven, do not change casually

| | |
| --- | --- |
| Outer diameter | **48.8 mm** |
| Skirt bore | **Ø45.6** (22.8 mm radius) — slides over the 45 mm ring |
| Crush ribs | 3 ribs to **Ø44.7** → 0.15 mm/side interference on the ring edge |
| Skirt drop | **3.0 mm** below the top face of the ring PCB |
| Face thickness | **1.2 mm** |
| Air gap, LED top → face underside | **6.0 mm** |
| Pattern recess depth | **0.5 mm** |
| Max pattern radius | **20.5 mm** |

The 1.2 mm / 6 mm combination was chosen from a printed sweep of three
thicknesses × three gaps: taller gaps diffuse best, and 1.2 mm keeps it bright.
The three crush ribs gave a perfect friction fit first try.

Because the skirt drops 3.0 mm past the ring top but the ring is only 1.6 mm
thick, **1.4 mm of skirt hangs below the ring** — a useful ledge that enclosure
designs can grip.

## Microcontroller module (SuperMini nRF52840)

| | |
| --- | --- |
| PCB | **18.0 × 33.5 × 1.6 mm** |
| Height envelope, soldered directly | **4.2 mm** |
| Height envelope, on 2.54 mm sockets | 8.4 mm |
| USB-C receptacle | **9.0 × 7.3 × 3.2 mm** |
| Receptacle overhang past the PCB edge | **1.4 mm** |
| Chip antenna | 9.0 × 3.6 × 0.9 mm, opposite end from USB |

⚠️ The 8.4 mm figure is for a module on **sockets**. Direct-soldered it is only
**4.2 mm** — an easy way to make an enclosure 4 mm taller than it needs to be.

Keep plastic away from the chip antenna end where practical.

## USB-C opening (empirically corrected)

Measured relative to the **module PCB bottom face**:

| | |
| --- | --- |
| Opening width, inner face | **14.8 mm** |
| Opening width, outer face | **16.8 mm** (relieved, helps the plug self-centre) |
| Bottom of opening | **0.8 mm below** the PCB |
| Top of opening, inner | **7.2 mm above** the PCB |
| Top of opening, outer | **8.0 mm above** the PCB |

This is deliberately **not centred** on the connector. In an earlier build the
metal receptacle cleared fine but the moulded shoulder of an ordinary USB-C
plug caught the *upper* edge and needed 2 mm trimmed by hand. The clearance is
therefore stacked above the board.

## Ring puck enclosure

| | |
| --- | --- |
| Overall | **54 × 54 × 23.4 mm** |
| Wall / floor | 2.5 mm / 1.6 mm |
| Corner radius | 3.0 mm |
| Internal cavity | 24.7 mm radius (round inside a square shell, so the corners stay solid for screws) |
| Cap seat | ring of material from 22.8 mm radius out to the wall |
| Central wire hole | **Ø42 mm** |
| Ring lip | 21.0 → 22.8 mm radius, standing 1.0 mm proud |
| Fasteners | 4 × **M2 self-tapping**, from the top, ~12 mm |

## Node enclosure

| | |
| --- | --- |
| Body | ~**94 × 60 mm** |
| Fasteners | 4 × **M2.5 × 35 mm** socket head |
| Carrier PCB | 80 × 46 mm, mounting holes on a 68 × 34 mm pattern |
