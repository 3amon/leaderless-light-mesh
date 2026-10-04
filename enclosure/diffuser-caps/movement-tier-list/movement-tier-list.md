# Diffuser pattern movement tier list

Estimated apparent movement when a **spatially non-uniform animation** (chase, rotating gradient, uneven sparkle, etc.) moves around the 16-LED ring behind the cap. If all 16 LEDs change together with equal brightness, none of the patterns should spatially shift; they will only change color or overall brightness.

## Result

| Rank | Tier | Pattern | Score | Expected character |
|---:|:---:|---|---:|---|
| 1 | **S** | MICRO | 94.1 | fine ratchet crawl |
| 2 | **S** | HRING | 93.7 | reversing herringbone crawl |
| 3 | **S** | SPRAT | 92.7 | spiral screw crawl |
| 4 | **S** | RATCHT | 91.7 | ratcheting rim crawl |
| 5 | **S** | COMET | 90.2 | tangential comet orbit |
| 6 | **S** | RINGS | 89.6 | orbiting band |
| 7 | **A** | VERNR | 88.1 | vernier beat crawl |
| 8 | **A** | GEAR | 86.8 | rim crawl |
| 9 | **A** | CROWN | 83.8 | two-speed halo sweep |
| 10 | **A** | TRICRN | 82.3 | three-speed halo sweep |
| 11 | **A** | ORBIT | 82.2 | dotted orbit |
| 12 | **A** | TIDES | 81.7 | eccentric contour flow |
| 13 | **A** | TOPO | 70.7 | contour drift |
| 14 | **B** | BLOOM | 65.8 | breathing spin |
| 15 | **B** | DRIFT | 65.3 | counter-rotating sweep |
| 16 | **B** | BRAID | 64.4 | counter-sliding orbits |
| 17 | **B** | DASH | 62.8 | radial chase |
| 18 | **B** | DOTS | 61.8 | granular twinkle |
| 19 | **B** | LACE | 59.9 | petal scintillation |
| 20 | **B** | RIPPLE | 58.6 | wavefront travel |
| 21 | **B** | HEX | 58.5 | cell sparkle |
| 22 | **C** | PULSE | 58.3 | rotating breath |
| 23 | **C** | SUN | 56.0 | radial sweep |
| 24 | **C** | TWIST | 55.3 | loop circulation |
| 25 | **C** | WEAVE | 54.6 | woven sparkle |
| 26 | **C** | WHORL | 49.7 | fine spiral shimmer |
| 27 | **C** | PETAL | 48.9 | petal chase |
| 28 | **C** | CAUSTIC | 46.6 | flowing cell network |
| 29 | **D** | CONST | 42.9 | jumping twinkle |
| 30 | **D** | STAR | 42.5 | radial pulse |
| 31 | **D** | PINWL | 38.9 | coherent spin |
| 32 | **D** | ECLIPSE | 34.8 | crescent flicker |
| 33 | **D** | CIRCUIT | 32.1 | branch chase |
| 34 | **D** | GALAXY | 26.9 | spiral travel |

## How the estimate works

This is not a full optical simulation. Each production SVG is rasterized as the real 0.5 mm recessed transmission pattern. Sixteen near-field light lobes are placed on an 18.8 mm radius behind it. The model rotates both a one-LED chase and a five-LED soft wave, averaging effective light spreads of 5, 7, and 9 mm to bracket the 6 mm air gap plus scattering in white PLA.

The score combines adjacent-frame pattern change (34%), highlight-centroid step and span (36%), temporal contrast on recessed regions (16%), and total pattern-edge interaction (14%). Dense edges matter because temporal intensity change is strongest where a moving field crosses a spatial gradient. Scores and tiers are relative to these 34 designs, not calibrated physical units.

## Why these effects are plausible

- Classical moire amplifies small relative shifts when two similar spatial structures differ slightly in pitch or orientation. Our second 'structure' is the rotating 16-source illumination field rather than another printed grid.
- Shadow-moire research explicitly creates phase/intensity changes by moving a light source relative to a fixed grid and its projected shadow. The cap is not an opaque shadow grid, but its 6 mm source-to-pattern gap gives the same geometric ingredient.
- The first-order motion relation `temporal change = displacement × spatial gradient` explains why numerous clean edges tend to shimmer more than broad uniform regions.
- Kinetic-light artists use grids, moving lights, spacing, and viewer distance as compositional controls; their descriptions agree that the setup must be tuned physically.

## Sources

- [MIT moire motion amplification](https://ocw.mit.edu/courses/3-91-mechanical-behavior-of-plastics-spring-2007/4d90035d3e2562c311a6cbede79e2da3_17_expt.pdf)
- [Optica moving-light shadow moire](https://opg.optica.org/ol/abstract.cfm?uri=ol-14-22-1252)
- [Optica point-source shadow moire](https://opg.optica.org/ao/abstract.cfm?uri=ao-33-11-2163)
- [MIT intensity-gradient motion argument](https://people.csail.mit.edu/fredo/comp-photo-book/12-video-04-video-magnification.html)
- [Fading Shadows kinetic-light study](https://jorisstrijbos.nl/work/fading-shadows)

## Calibration plan

Film one fixed 16-step white chase and one rotating rainbow on every cap with locked camera exposure. Measure frame difference and centroid travel from the video, then fit the model weights to the physical results. Repeat after the lithophane PLA arrives because its scattering may compress the ranking toward the middle.
