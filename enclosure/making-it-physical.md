# Making it physical

Everything here is printed on a **Bambu A1 Mini** (180 × 180 mm bed) in PLA at
0.20 mm layers on a textured PEI plate. That constrains design more than
anything else in this project, so it's worth knowing the rules before drawing.

## Caps

**Printed face-down.** The smooth visible face goes against the build plate.
This is why:

- the face comes out flat and evenly matte (the plate's texture adds a free,
  very fine diffusion)
- it avoids bridging a ceiling over a round cavity, which fails
- **only the inside of the cap can carry a pattern** — see
  [`diffuser-caps/`](diffuser-caps/README.md)

Recipe: **70 °C bed, 10 mm brim, no supports.**

⚠️ **Thin face-down discs warp.** A tightly packed plate of caps once curled at
the edges, then the nozzle dragged through them and smeared stringy plastic
across the whole print — despite the printer reporting success. The fix that
works is the hot bed plus a wide brim, which connects into a raft and pins
every face down. Don't reduce either.

**Material:** designed in opaque white PLA, recently reprinted in **translucent
white ("lithophane") PLA** for better glow-through. The translucent material
scatters more, which is expected to soften and compress the differences between
patterns — nobody has judged this by eye yet.

## Enclosures

Recipe: **65 °C bed, 20 mm brim, no supports.**

The wide brim replaced a 10 mm one after the same corner lifted repeatedly on
the larger parts.

Design so each part prints **open-face up with no supports**:

- Both ring-puck parts do this, which is exactly why it's two pieces. An
  earlier one-piece version had an internal ledge that had to bridge in mid-air
  and printed badly.
- Where an internal overhang is unavoidable, **slope it ~45°** so it
  self-supports. The funnel inside the ring-puck base exists for this reason,
  and it also prints cleaner than the flat version it replaced.
- The node enclosure's roof is the exception — it prints top-face-down and
  needs a few breakaway supports under the D-ring arch.

## Things that will bite you

- **Stringing across large openings.** Travel moves over the big cap aperture
  leave wisps. Mostly a filament-dryness and retraction issue rather than a
  geometry one, and it ends up hidden under the cap — but it's there.
- **Bed size.** 180 × 180 mm, and brims count. A 3-wide plate of caps at 53 mm
  pitch fits; wider spacing does not.
- **Not every problem needs a printed part.** A stray indicator LED on the
  module glowing up into the cap was solved with a dab of opaque paint, not a
  printed baffle. Worth remembering before adding geometry.

## Fasteners

- Ring puck: **M2 self-tapping**, driven from the top into printed pilot holes.
- Node enclosure: **M2.5 × 35 mm** socket head.

Printed pilot holes for self-tappers are ~Ø1.5 mm; clearance holes ~Ø2.2 mm.
