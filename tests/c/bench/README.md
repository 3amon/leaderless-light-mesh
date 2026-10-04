# Scene bench (host renderer)

Measurement harnesses for iterating on `social_scene.c` without hardware.
They print measurements and assert nothing.

- `solo_bake.c` — renders normal-mode solo families through main.c's pixel
  pipeline (vivid guard, overflow rescale, 45% brightness, slewed 45/LED
  power cap) and emits JSON frames (16 LEDs, 20 fps, 16 s, three minutes
  per family) plus mean load/LED and how often the cap engaged.
- `solo_loadprobe.c` — sweeps every hue and minute and prints the per-frame
  load distribution per family (mean, p50, p90, max, share over the cap).
  Whole-ring scenes are the rail's worst case; keep p90 at or under 45.
- `bench_head.html` + `bench_tail.html` — wrap `{...old..., ...new...}` JSON
  into the Solo Scene Bench page (shipped vs working tree, diffuser sim,
  display exposure, metrics).

Build against the working tree, and against a frozen copy of the shipped
renderer (`git show vX:firmware/thread-ring/src/social_scene.c`) for the
"before" side:

    SRC=firmware/thread-ring/src
    cc -std=c11 -O1 -DTAG='"new"' -I$SRC $SRC/social_scene.c $SRC/led_budget.c \
       tests/c/bench/solo_bake.c -lm -o /tmp/bake_new && /tmp/bake_new > new.json
    cc -std=c11 -O1 -I$SRC $SRC/social_scene.c tests/c/bench/solo_loadprobe.c -lm \
       -o /tmp/lp && /tmp/lp

Then `python3 -c` merge old.json + new.json into one object and concatenate
head + JSON + tail into the page.
