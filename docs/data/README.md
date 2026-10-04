# Simulated merge trace

`merge-convergence.csv` is the output of `tests/c/merge_trace.c`: six
simulated nodes, each running the firmware's `clock_estimator.c`, in two
groups of three that cannot hear each other for 90 s and then can. One group
starts 800 ms away from the other. It is the scenario asserted by
`test_split_halves_merge_despite_outlier_rejection` in
`tests/c/clock_sim_test.c`, with the same seed, sampled every 200 ms.

This is a simulation. The crystals (+/-300 ppm), the transit delay (2-8 ms,
with a 30 ms excursion on one packet in twenty) and the trust ramp are
modelled; the radio is not.

Regenerate from the repository root:

```sh
cc -std=c11 -O2 -Ifirmware/thread-ring/src -Itests/c \
   firmware/thread-ring/src/clock_estimator.c tests/c/merge_trace.c \
   -lm -o /tmp/merge_trace && /tmp/merge_trace > docs/data/merge-convergence.csv
```

The run is deterministic: the same sources give the same file.

| Column | Meaning (milliseconds unless noted) |
|---|---|
| `t_s` | seconds relative to the moment the groups come into range |
| `phase` | `split` or `merged` |
| `spread_all` | worst pairwise disagreement across all six clocks |
| `spread_a`, `spread_b` | worst pair inside each group |
| `gap_ab` | mean of group A minus mean of group B |
| `n0`..`n5` | each node's offset from the six-node mean |

What it shows: nothing happens for about ten seconds after contact, because a
newly heard peer starts with almost no vote (weight 0.05, ramping over the
20 s join run). Once the other group's votes carry weight the two groups
close an 820 ms gap in roughly fifteen seconds, and from 30 s on the worst
pair stays under 11 ms.

`../img/merge-convergence.png` is drawn from this file.
