# Adversarial gap-hunt harnesses (August 2026)

Measurement-mode scenario batteries behind `docs/gap-hunt-2026-08.md`.
Unlike the sibling `*_sim_test.c` suites these print measurements and
assert nothing — they exist so the report's numbers are reproducible.

Build (from repo root):

    cc -std=c11 -O2 -Ifirmware/thread-ring/src -Itests/c \
       firmware/thread-ring/src/group_engine.c \
       firmware/thread-ring/src/group_policy.c \
       firmware/thread-ring/src/social_scene.c \
       firmware/thread-ring/src/peer_slot_policy.c \
       firmware/thread-ring/src/sync_schedule.c \
       tests/c/campaign/campaign.c -lm -o /tmp/campaign

`integrated.c` additionally needs `clock_estimator.c` (it couples the
real group engine to the real clock estimator); `i2b.c` includes
`integrated.c` so build it with `-Itests/c/campaign`.

## Harness notes (2026-09-29)

- The engine now ticks at exactly 1 Hz per node, as on the device. The old
  `now_ms % 1000 >= 400` gate only coincided with the 400 ms clock update
  every third second, so every tick-counted window (join 20, agree 5,
  demote 10) ran up to twice as long as in firmware and hid the two-node
  clock deadlock. Measurements from before this date were taken with the
  slower tick.
- `idrop_pct` drops that percentage of beacon copies in transit (a weak
  real link loses a meaningful share); `istranger_start` starts every peer
  clock at stranger trust instead of the estimator's 1.0 default, modelling
  the device thread order where the group tick runs before the first clock
  update. Both default off.
- `tests/c/pair_bootstrap_test.c` includes this file and asserts on the
  pair scenarios; it is part of the host suite via `tests/test_pair_bootstrap.py`.
