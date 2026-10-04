# Sync/group cohesion investigation — all-solo collapse

Started 2026-08-16. Motivating field symptom: nodes occasionally desync and
the entire crowd falls back to solo mode despite good radio contact. Two
prior instances were root-caused and fixed (stale departed-collider record,
v0.1.68; frozen charge-quiet control plane, v0.1.69); this thrust attacks
the remaining *mechanisms* rather than chasing instances, with a simulator
first so every fix lands evidence-first.

## Collapse mechanisms in the v0.1.70 design

Confirmed against source and, where noted, reproduced deterministically by
`tests/c/group_engine_test.c` characterization tests:

1. **Duplicate small IDs freeze everyone** *(reproduced on host)*. Topology
   completeness is component-scoped — a non-reciprocal stranger's malformed
   view bits are correctly ignored (the harness disproved the first, broader
   form of this hypothesis) — but `group_topology_component()` marks the
   snapshot incomplete on a duplicate `present[]` sid among ANY heard peers,
   or any inconsistency *inside* the reciprocal component (a stale member
   bit). Agreement freezes for every node in range; past the 60-tick handoff
   the whole healthy crowd goes dark. A sid-squatting rogue with a higher
   node_id than its victim creates the condition permanently, because only
   the higher ID yields in collision repair.
2. **The sid space invites those duplicates.** `hash % 25` across a 29-board
   inventory guarantees collisions; each repair transits a duplicate-sid
   window (mechanism 1) for everyone in range, and the repair round counter
   is RAM-only so the same collision replays after every reboot. Above 24
   simultaneous nodes, excluded high-ID nodes chase the few free sids.
3. **One bad clock darkens the crowd** *(reproduced on host)*. The render
   lock takes the worst median across all agreed members; a single member
   holding >80 ms (rejoining after drift, or simply broken) unlights every
   node after the 5-tick hysteresis. There is no outlier-demotion path;
   "agreed member" and "phase-locked member" are conflated.
4. **The clock estimator averages with a weighted mean, not a median**
   (`clock_estimator.c`), despite TIME-SYNC.md listing median-not-mean as a
   required hardening. A rogue or insane member clock drags the
   neighbourhood.
5. **Joining is fragile under loss.** One 12-second gap in direct-beacon
   evidence resets the 20-tick join run to zero (`CANDIDATE → STRANGER` on a
   single bad tick), so a lossy link can hover at the threshold indefinitely.

## S1 (done): the pure engine + first simulator

`group_engine.[ch]` is a verbatim transplant of the groups.c decision core
— sid repair, view construction, reciprocal-topology agreement, seed
matching, the median phase-lock gate, bounded handoff, scene crossfade
state, and retirement — with no Zephyr dependencies. `groups.c` is now a
thin shim (uptime, sync snapshot, OT RSSI in; view/weight pushes and log
lines out) that adds no decisions of its own. Firmware, host tests, and the
simulator compile the same engine file, so a cohesion bug found in
simulation is by construction the bug the fleet ships.

`tests/c/group_engine_test.c` carries the first mini-simulator (N engines,
synchronous lossless snapshot exchange) plus convergence, departure, and
tick-gating tests and the two collapse characterizations. Those
characterizations originally asserted the v0.1.70 failure behavior and
were flipped by the S3 fixes; they now pin the healthy behavior.

## S2 (done): the adversarial simulator

`tests/c/cohesion_sim_test.c`: deterministic, seeded, discrete-event. Every
node runs the real engine over a modeled sync layer built from the real
pure modules — beacon schedules (`sync_schedule.c`), slot eviction
(`peer_slot_policy.c`), the 12 s direct / 45 s any / 90 s retention
freshness windows — with a per-link lossy asymmetric radio, partitions,
flapping, reboots, rogue beacon injection, and per-node clock offsets.
Nine scenarios; ~30 simulated node-hours run in well under a second.

Baseline result against the v0.1.70 engine: the five churn scenarios
(lossless/lossy convergence, asymmetric links, a 30 s flapper, rolling
reboots) already passed — the ordinary machinery is robust — while the
sid-squat rogue scenario failed exactly as mechanism 1 predicts. The
bad-clock scenario failed per mechanism 3 (previously reproduced in
`group_engine_test.c`).

## S3 (done): fixes, each demanded by a failing scenario

1. **Localized completeness (mechanism 1)**: duplicate small-id records are
   quarantined from topology and seed selection; only the canonical
   claimant — the lowest node_id, matching what collision repair converges
   to — is visible, and claims on the local node's own sid are quarantined
   unconditionally. Quarantined peers keep membership/timing state and
   return when the duplicate resolves. The squat scenario now holds the
   healthy crowd lit through a 200 s permanent squatter.
2. **Timing demotion (mechanism 3)**: a member whose median offset stays
   above the 80 ms bound for 10 consecutive ticks is dropped from the local
   view (never from membership); the healthy majority re-mints behind the
   bounded handoff and the outlier returns after 5 good ticks. Simulated:
   crowd of five loses one 200 ms member, four stay lit, full crowd
   re-forms after the clock heals. Mixed-fleet note: pre-v0.1.71 nodes
   never demote, and reciprocal-component transitivity means their vouching
   keeps the outlier in upgraded nodes' agreed sets — the fix reaches full
   strength once the local crowd is upgraded, and is never worse than the
   old behavior meanwhile.
3. **Sid space and persistence (mechanism 2)**: `hash % 32` (the full view
   bitmap; ≥8 spare values at the 24-node cap) and the repair round now
   persists in settings (`ringgrp/sidround`), so a collision is fought once
   rather than after every reboot. Wire-compatible: sids only need to be
   distinct, not identically derived, and every shipped decoder handles
   bits 0–31.

Mechanism 4 closed on 2026-08-16 by the clock simulator
(`tests/c/clock_sim_test.c` — the REAL estimator per node over jittery
transit and per-node crystals). Its findings, each encoded as a scenario:

- Under the weighted MEAN, a fully-trusted member broadcasting a constant
  10 s lie did not merely drag the crowd — from cold start the crowd
  CONVERGED ONTO the lie outright (~9.97 s adopted, honest nodes mutually
  locked to the liar).
- The weighted median alone was insufficient: a cold-boot capture cascade
  remained (a node whose first fresh peer is the liar snaps onto it, its
  beacons poison others, and a captured majority is bistable under the
  median). Closed by a SNAP QUORUM: below one full member's weight of
  votes the phase step is slew-only.
- A liar's permanent seat still skewed the median's quantile (~ms/s crowd
  drift). Closed by MINORITY-OUTLIER REJECTION (votes >250 ms from the
  preliminary median are dropped, but only when they hold a minority of
  the vote weight — a 50/50 long-split reunion must still merge, and the
  reunion scenario proves it does).
- The leaderless clock's inherent collective wander (transit bias, a few
  ms/s, invisible to eyes) is documented as designed behavior; assertions
  measure distance-from-the-lie and mutual lock, never absolute truth.
- Scramble drills against a converged subgroup self-heal within seconds
  through each node's own sample window — the simulator reproduced the
  hardware scramble-500 heal dynamics exactly.

Rate consensus likewise moved from mean to median (the h_min/h_max clamp
had already bounded that axis). The v0.1.75 rollout then supplied its own
field incident: the rate median's even-count pick landed on the
UPPER-middle vote, a half-spread-high bias that compounded through
their_h — the live seven-node fleet's collective trim ratcheted
~+300 ppm/min toward the clamp while phase stayed perfectly locked, so
only ppm telemetry showed it. The clock simulator reproduced the runaway
deterministically, an A/B against the v0.1.74 estimator isolated the
regression, and v0.1.76 fixed it (middle-pair midpoint, same convention
as the phase median) with a long-run bounded-trim scenario as the
permanent regression gate. Live confirmation: trim flat within noise
over five minutes post-rollout, versus +1500 ppm in the same window
before the fix. The crowd's residual collective trim offset (~3100 ppm
after the incident) is anchor-free leftover and wanders harmlessly; a
tiny relaxation-toward-1.0 gain is noted as an optional future tune. Mechanism 5 (join-run fragility) remains
open as a tuning question best answered with live-fleet soak data.

## Live fleet qualification — 2026-08-16

v0.1.71 was released (strict attested build, GitLab release, signed tag)
and rolled to all six live nodes (humming canary first, then per-device
transactional OTA). The crowd re-formed as one six-node group under the
widened sid space (sids 1,13,15,16,17,18; agreed/commit `0007a002`,
worst offsets 3–7 ms).

Live persistent-outlier drill (evidence:
`/home/pi/deployments/20260816-v0171-cohesion/live-drill2.log`):
`molten-dune-glow` was scrambled 500 ms every 2 s for two minutes.
Within 24 s the healthy five demoted it (agreed dropped to `0003a002`)
and re-minted lit at size 5 — where v0.1.70 held the whole crowd dark for
the duration. The victim stayed correctly solo, and the full six-crowd
re-lit within 16 s of the abuse stopping and held for the whole watch.
Observed artifact: because gossip sync heals each scramble within
seconds, the victim oscillated demoted↔readmitted during the drill
(size 5↔6 with two brief dark blips at unlock/demotion transitions); a
constant-offset outlier — the real field case — stays stably demoted as
in simulation. If damping is ever wanted, lengthen READMIT_TICKS.

Also caught during the drill setup: mutating ringctl commands (scramble)
require sudo for the control keys in /etc/thread-led-node/control-keys/;
a first drill run silently no-op'd on permission errors. Verify a
mutation landed before trusting a null result.

## Invariants

The simulator's scenario assertions encode these; keep new scenarios
honest against them:

- I1: no all-solo while a fresh connected subgraph of ≥2 healthy nodes has
  existed longer than the convergence budget;
- I2: re-convergence within 2 minutes after churn stops;
- I3: no two overlapping lit scenes;
- I4: per-node solo time bounded under continuous fault injection.

## Still open (future candidates)

- Median or trimmed mean in the clock estimator (mechanism 4); needs the
  simulator to model estimator dynamics, not just measured offsets.
- Join-run fragility under loss (mechanism 5); tune with live soak data.
- View-generation counter in the beacon for instant stale-bit retirement
  (wire change; needs operator sign-off and the rolling-migration
  machinery).
