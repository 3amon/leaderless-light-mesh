# Cap graphics

The diffuser cap is the face of the product. This folder has every pattern
drawn so far, as editable SVG, plus the 3D cap form they sit on.

## How a cap actually works

The cap is a shallow cup that presses down over the LED ring. Its top face is
**1.2 mm** of translucent plastic. Patterns are **shallow recesses cut into the
inside (LED-facing) face**, about **0.5 mm** deep — so a "drawn" region leaves
only ~0.7 mm of plastic and glows *brighter*, while untouched regions stay
thicker and dimmer.

```
        ↓ what you see (smooth, printed against the build plate)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  1.2 mm face
   ▔▔▔╲▁▁▁╱▔▔▔▔╲▁▁▁╱▔▔▔▔▔▔▔▔▔▔▔▔▔  0.5 mm recesses on the INSIDE = the pattern
              6 mm air gap
   · · · · ·  · · · · ·  · · · · ·  16 LEDs
```

Two consequences worth internalising:

- **The visible face must stay smooth.** It prints against the build plate,
  which is what makes it clean and evenly matte. Texture applied to the *outer*
  face does not survive printing. All pattern work happens on the inside.
- **Brightness is thickness.** You are not drawing ink, you are drawing
  *thinner plastic*. There are only two tones — recessed and not — so patterns
  are effectively 1-bit. Gradients have to be faked with dot/line density.

## Design rules (learned the hard way)

**The ring lights an annulus, not a disc.** Sixteen LEDs sitting at ~19 mm
radius make a bright *band*. The middle of the cap is dark, and light falls off
past the outer edge. This is the single most important fact about designing
these.

Which means:

1. **Put detail in the lit band**, roughly 12–20 mm radius. That is the only
   place a pattern can be seen well.
2. **Treat the centre as intentionally dark.** Leave it blank, or use it as
   negative space — a solid silhouette reads fine. Fine detail there is wasted.
3. **Favour radial and spoke motifs over concentric rings.** Anything that
   crosses the lit band radially catches the moving light. This is not a
   preference, it is a test result: a concentric-rings pattern was printed and
   **only the single outermost ring lit up**; everything inside sat in the dark
   centre and vanished.
4. **Hex, dot, and cell patterns only pop where cells land on the annulus.**
   Space them so cells fall in the band.
5. **Avoid counts of 16.** A feature count that matches the 16 LEDs locks to
   them and looks static. Counts like 15, 17, 19, 22, 23, 25 *beat* against the
   LEDs and produce apparent motion.

The plain 16-ray **sunburst** (`patterns-svg/sun.svg`) is the long-standing
aesthetic favourite — the shape that looked best in hand, chosen over a whole
round of fancier variations. Treat it as the baseline to beat.

## What's in here

| Folder | Contents |
| --- | --- |
| `patterns-svg/` | **34 individual patterns**, one SVG each. These are the editable source. Filenames are the short pattern label, lowercased. |
| `contact-sheets/` | Browse-everything sheets. `all-patterned-caps.svg` is every pattern at once. Start here. |
| `movement-tier-list/` | Patterns ranked by predicted apparent motion — `.md`, `.png`, `.svg`, plus raw scores. |
| `cap-3d/` | The physical cap form. `single-cap-reference-sunburst.stl` is one complete cap at the proven 1.2/6 geometry — open this to understand the shape. |

## The movement tier list — read the caveat

`movement-tier-list/` ranks all 34 patterns by how much they should appear to
*move* when an animation chases around the ring. Current top of the list:

| | Pattern | Score |
| --- | --- | --- |
| 1 | MICRO | 94.1 |
| 2 | HRING | 93.7 |
| 3 | SPRAT | 92.7 |
| 4 | RATCHT | 91.7 |
| 5 | COMET | 90.2 |
| 6 | RINGS | 89.6 |

**This is a model, not a measurement.** It rasterises each pattern, simulates
16 light lobes behind it, rotates them, and measures how much the image
changes. It has never been calibrated against a real lit cap. Worse, the six
newest patterns (MICRO, HRING, SPRAT, COMET, VERNR, TRICRN) were *designed
against this scoring function*, so their high placement is partly circular.

Scores are also **relative to the set** — adding strong patterns pushes
everything else down. Rank order is meaningful; absolute numbers are not.

Judging these by eye on a real lit ring is genuinely open work.

## Editing patterns

The SVGs are plain vector shapes. Filled regions = recessed = bright.

These files are *generated* by a parametric script that is not part of this
repository; the SVGs here are its output, not a round-trip editing source.

Keep new artwork inside a **20.5 mm radius** — beyond that it falls outside the
lit area and gets clipped by the cap wall.
