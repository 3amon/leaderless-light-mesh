# The Look Book

Curated effect recipes, each verified on camera on 2026-08-16 with the
fixed-exposure ring view and `pi-tools`' `look_probe.py`-style analysis
(per-ring brightness waveforms, measured periods, cross-ring phase). Scene
colors rotate on shared minute boundaries — the recipe sets the *motion
and light dynamics*, not the palette. All commands auto-pick priority:

    ringctl <any-live-node> mode <recipe>

Or use the preset wrapper on the Pi — one word, finds a live node itself:

    sudo ~/ringlook.py shimmer      # or deepbreath / surge / torrent /
                                    # slowwink / strobeparty / calm

| Recipe | Command tail | Measured | Character |
|---|---|---|---|
| **Deep Breath** | `normal blink=2000 duty=30 wave=fade` | 2.00 s breathe, ~50% swing | The resting state: slow synchronized inhale/exhale over calm scenes. |
| **Surge** | `party blink=1300 duty=20 wave=fade` | 1.33 s breathe, deep floor | Party motion swelling hard and fast — dramatic without strobing. |
| **Shimmer** | `ultra blink=750 duty=60 wave=fade` | 0.75 s breathe, shallow | Fast dense scenes with a quick gentle pulse riding on top. |
| **Torrent** | `ultra` | solid, ~0.6 s pattern motion | Bare ultra: maximum motion energy, steady light. |
| **Slow Wink** | `normal blink=1500 duty=80 wave=hard` | 1.5 s, 80% on | Calm scenes that blink off for a beat every 1.5 s — playful, readable sync. |
| **Strobe-Party** | `party blink=320 duty=50 wave=hard` | 0.33 s square, 92% swing | The original ultra feel: full-crowd hard flash. Loudest look; use sparingly. |
| *(calm)* | `normal` | solid | No effect. The default. |

Safety rails baked into firmware: 130 ms period floor (≈7.7 Hz max, far
below the photosensitive band), duty bounded 20–80 or exactly 100 (solid),
viral ties resolve calmest-first.
