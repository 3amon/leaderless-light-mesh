# Leaderless time sync + pattern composition — design notes

Design for the wandering-wearable use case: nodes drift around (live events),
form ad-hoc Thread meshes as people get close, and patterns must look
synchronized to anyone standing nearby. No master, no elected leader — every
split/merge/death is a non-event.

The design was first worked out in two browser simulations (a sync-only
model, then a combined model with sync, genome patterns and group minting
for 24 wanderers). Those pages are not part of this repository; the numbers
quoted from them below are labelled as simulated. The simulators that ARE
here compile the firmware's own C modules: see `tests/c/`.

Lineage: this is GTSP (Sommer & Wattenhofer 2009) in spirit. FTSP is the rooted
cousin; Mirollo–Strogatz "firefly" sync only aligns phase-within-a-beat, which
is weaker than a full shared clock (no shared timeline → only periodic patterns).

## Core idea

There is no correct time, only agreement. Patterns are pure functions of a
shared logical clock, so any value everyone converges to works. Each node keeps

    global(t) = h · local(t) + θ

`local` = free-running hw counter (never touched), `θ` = learned offset,
`h` = learned rate trim (starts 1.0). It's a PLL per node whose reference input
is the average of its neighbors: θ-loop = phase, h-loop = frequency. Frequency
lock is what makes phase lock cheap, and what keeps two groups that separate for
an hour only tens of ms apart when they rejoin.

## Beacon (independent local and realm multicast, unacked)

    { my_global, my_raw_local, my_h, my_pattern_seed }

Production uses independently randomized 1.8–2.6 s link-local beacons and
8–12 s realm floods, with longer freshness gates. During a rolling upgrade,
legacy traffic temporarily selects the corrected 450–550 ms compatibility
cadence. The raw clock MUST be in there — see the frequency loop below for why.

## On receive (timestamp arrival as early as possible — radio event, not app)

    la = local_now
    append (la, msg.raw)    to nbr.raw[]    # ~40 entries, ~20 s
    if |(msg.global − la) − (prev.global − prev.la)| > 40 ms:
        clear nbr.phase[]                    # they stepped their clock; history is lies
    append (la, msg.global) to nbr.phase[]  # ~10 entries, ~2.5 s
    nbr.h = msg.h; nbr.seed = msg.seed

## Periodic update (~every beacon interval)

**Phase (the whole consensus):**

    for each fresh neighbor:
        delta_j = max over phase[] of (stamp − (h·la + θ))   # min-delay filter:
                                                # transit delay only ever reads LOW
        # if samples within the window disagree by >25 ms, the neighbor is
        # mid-correction — trust the NEWEST sample instead of the max
    m = mean(delta_j)          # ← consider MEDIAN, see hardenings
    if |m| > 80 ms:  θ += m/2                      # genuine merge: jump
    else:            θ += clamp(m/2, ±60 ms/s)     # slew, invisible

The ½ is load-bearing: two nodes that each jump the full gap swap places
forever; each moving halfway meet exactly. Converges in any connected mesh;
neighbors converge fastest (gradient property — exactly what eyes compare).

**Frequency:**

    for each neighbor with ≥8 raw[] samples spanning ≥8 s:
        slope = LSQ slope of raw-stamp vs local-arrival    # their hw rate vs mine
        if residual RMS < ~(5 ms + jitter): rate_j = slope × nbr.h
    h += 0.1 × (mean(rate_j) − h);  clamp h to ±6000 ppm
    if isolated (no neighbors): h decays slowly toward 1.0

**⚠ The lesson learned twice in simulation: regress on the RAW clock, never the
logical one.** A neighbor mid-slew produces a perfectly linear tilt in its
logical clock — indistinguishable from frequency error by any residual test
(it railed trims at 30× true crystal drift before the raw/logical split fixed
it). The raw clock is never stepped or slewed, so it cannot lie.

## Window sizing rule

A window should be as long as the quantity sampled stays trustworthy:
- phase[] ~2.5 s — the logical clock is continuously corrected; older stamps
  rot (staleness = relative-ppm × age, plus the neighbor's own corrections)
- raw[] ~20 s — free-running, drift is linear, longer = strictly better
Scale both with the beacon interval (keep ~5 phase samples in window).

## Tuning lessons from the combined sim (2026-07-15, hard-won — read these)

Found by instrumenting in-group clock gaps in the combined simulation (worst
pair within a lit group hovered ~15-25 ms until all four fixes landed;
p50 ≈ 10 ms, p95 ≈ 28 ms after — and real HW with SFD timestamping should
beat the sim, whose jitter is app-level exp(5 ms)):

1. **Weight the phase mean by a TRUST LADDER, not equally and not by raw
   proximity**: group members (past join hysteresis) = 1.0; approaching
   candidates = graded (~0.1 + 0.5·smoothed-RSSI) so they PRE-CONVERGE
   during the radio-before-visual window; distant strangers = whisper.
   Equal weights → every passer-by drags the nearest member (~10 ms wobble
   dragged through the group). Near-zero stranger weight is ALSO wrong:
   candidates then join cold at full vote and yank the group (measured:
   made everything worse). Normalization means a sole neighbor converges
   at full speed regardless of weight — the ladder only matters in mixed
   company.
2. **Phase-lock gate on the group layer**: don't light until every member's
   measured delta < ~6 ms; extinguish only above ~14 ms (hysteresis so it
   doesn't flap). Members can measure this from state they already have
   (peer.lastD). Makes "lit group ⇒ visually coherent" true by
   construction; joins keep the layer dim until the newcomer locks.
3. **Deadband ~1.5 ms**: below perception, stop correcting — otherwise
   nodes chase jitter forever at the floor.
4. **Window-disagrees fallback = min-delay over the newest FEW samples**,
   never the single newest (a lone raw sample is pure jitter, biased low
   by its full transit delay).
Also: check the RSSI join threshold against how far apart people actually
stand in a huddle — the sim's group formation starved until "close" was
sized larger than the huddle radius.

## Measured in simulation (12 nodes, 20 % loss, 5 ms jitter tails, ±300 ppm)

- steady state: 5–20 ms worst *linked-pair* gap under constant churn
  (LED beat of 800 ms → ≤3 % — imperceptible; sine/sparkle patterns tolerate
  50–100 ms before anything reads as off)
- full ±3 s clock scramble: back to ~5 ms within seconds (snap-halving)
- two groups of 6, separated ~8 min: reunite only ~24 ms apart (h-loop's doing)
- radio range ≫ visual range (~10×): reconciliation finishes while people are
  still specks to each other — nobody ever *sees* a correction

## Assumptions / how it breaks (engineer against the first two)

1. **Bidirectional links.** A node heard-but-deaf (strong TX, blocked RX) is
   never pulled — it becomes an accidental master everyone chases. Body-worn
   802.15.4 is notoriously asymmetric. Fix: beacon carries recently-heard
   neighbor list (or hash); only accept influence from neighbors who list you.
   Thread MLE already tracks two-way link quality.
2. **Mean has no immune system.** One wedged/buggy node drags its whole
   neighborhood. Use MEDIAN of deltas instead of mean — near-free, same
   convergence, ignores a lone liar.
3. Radio must be rx-on-when-idle to overhear multicast — SEDs can't gossip.
   Fine here: LEDs dwarf the radio budget (~6 mA on nRF52840). If sleep ever
   matters: CSL (Thread 1.2).
4. Min-delay filter needs occasional luck, not low loss. 802.15.4 ch 25/26
   avoids Wi-Fi. Floor bias is symmetric per-pair → cancels pairwise; the
   common-mode drift of network time is unobservable.
5. Frequency loop needs ~8 s stable links — brief passing contacts get phase-only sync
   (graceful).
6. Global agreement diffuses at O(diameter²) updates — irrelevant for
   proximity-blended patterns; matters only for network-wide one-shot cues.
7. No absolute time; consensus value wanders. If ever needed (align to a
   venue's music/GPS): give a time-source node small extra self-weight — a soft
   anchor, not a master; two disagreeing anchors = bounded gradient, no fight.

## Pattern library

Hardware: 16-LED WS2812 ring per node (Amazon B0B2D5QXG5), possibly behind
lamp/flower diffusers of varying strength — so patterns must read as
aggregate color+brightness over time, with spatial motion as a bonus.

**Fourteen seeded families** (pattern fns are stateless
`f(t_global_ms, led_index, variant) → rgb`, transplantable to firmware as-is)

Rules the families follow: tempo band 2.5–30 s, rise times ≥0.4 s (no
strobes), never fully black, restricted hue spans (blues/violets/teals/
greens + two warm accents; no rainbows), twinkles sparse and ~2 s each.

**Genome (2026-07-14):** the 14 families are BASES, not the whole space.
`seed → hash → { base: seed%14, palette hue from a weighted wheel (mostly
cool, rare warm), overlay: 1 of 5 textures (none/twinkle/roving-arc/
shimmer/counter-glow), overlay amount, tempo ±20%, phase, salt }` — tens of
thousands of on-brand looks from 14 functions. Hash the seed before slicing
bits, or small seeds collapse the variant space. Family hue constants hang
off `v.H` so the palette can repaint any base.

**Group patterns:** `group_seed = XOR over members of hash32(seed_i)` —
commutative + associative, so every member computes the identical group
seed from seeds already in the sync beacons; feed it through the same
genome fn. Every distinct set of people mints a distinct pattern, identical
on all of them, gone when they part. Pairs are just |group|=2.
⚠ Membership must AGREE (XOR is discontinuous — one disputed member =
totally different pattern). The design (all four layers, they stack):
1. Candidate view from direct link-local reception with hysteresis: join after
   ~20 s sustained contact; require ~20 s beyond the 12 s direct-freshness
   window before leaving, so burst loss does not dissolve an established set.
2. GOSSIP THE MEMBERSHIP, not a hash of it: with ≤32 known nodes, assign
   5-bit IDs at flash time and the whole view is a 25-bit bitmap — same
   4 bytes a hash would cost, but it RESOLVES disagreement instead of just
   detecting it. Each beacon carries the sender's current view.
3. Agreed group = per-bit consensus over strong neighbors' bitmaps
   (member iff majority of views include them), damped — adopt a bit only
   after it's stable across a few beacon rounds, so the feedback loop
   can't oscillate. Light the group layer only while strong neighbors'
   bitmaps match yours; XOR the agreed set's seed-hashes for group_seed.
4. Crossfade the group layer ~3 s on any change and keep the solo pattern
   underneath at partial weight — gating on agreement turns wrong-pattern
   into no-pattern at the boundary; fade + hysteresis keep that from
   reading as flicker.
Render composite: solo·0.45 + group·0.85, then hue-preserving overflow
scaling (proportional, never per-channel clip — clipping washes to white).

## Pattern composition (the fun part)

Every pattern is a pure function `f(global_clock, seed, led_index)`. The seed
rides every beacon, so when two wearers meet, **each renders the other's
pattern locally** — identical output on both, zero pixel streaming.

- Blend weight = low-passed RSSI. Treat RSSI exactly like the clock: garbage
  instant-to-instant, fine filtered — slew it, never step it, hysteresis at
  the edge so nobody strobes in/out, and key it to STRONG (near-field) RSSI so
  blending means "actually together," not "in radio earshot 60 m away."
- **Pair-emergent pattern:** both nodes know both seeds →
  `pair_seed = hash(min(sa,sb), max(sa,sb))` renders a third pattern neither
  wears alone — exists only while that specific pair is together. Free.
- Compositing is additive light; when channels overflow, scale proportionally
  (preserve hue) rather than clipping to white — groups of 4 wash out otherwise.
- Keep pattern periods multiples of a shared base beat so same-type patterns
  visibly lock when their wearers meet.

## Device role & power (nRF52840, e.g. SuperMini / Amazon B0F1KG283T)

- Role: **FTD, router-eligible (REED), rx-on-when-idle** —
  `CONFIG_OPENTHREAD_FTD=y`, NOT sleepy. Two reasons: (1) gossip requires
  overhearing peer multicasts, impossible for a SED (and SSED/CSL doesn't
  help — it schedules parent→child windows, not peer overhearing); (2) a
  wandering cluster of end devices with no router can't form a network —
  all-REED means any group self-forms a Thread partition, and partitions
  merge natively when groups reunite. Thread's elected partition Leader is
  routing bookkeeping only; clocks/patterns stay leaderless.
- Power: radio RX ≈ 5-6 mA (DC/DC) — under 10% of budget. 16 WS2812 draw
  ~1 mA EACH at idle (~16 mA doing nothing) + ~30-80 mA average for dim
  night patterns. LEDs dominate; don't optimize the radio.
- Sleepy experience still pays off as a MODE: daytime/pocket deep sleep
  (accel-still ~10 min or lux threshold → LEDs off via EN_LED gate, radio
  down/polling). Rx-on while the costume is live.
- Firmware deliberately caps a fleet/group at 24 nodes (23 peers plus self),
  comfortably below Thread's active-router ceiling.

## Prototype bring-up findings (2026-07-17, SuperMini clone on the bench)

Working stack, PROVEN end-to-end (`firmware/ring-test/`):
- vanilla Zephyr v4.1 workspace at `~/zephyr-ws` (uv venv: west +
  cmake<4 — CMake 4.x breaks Zephyr 4.1), minimal SDK 0.17.0
- board target **`promicro_nrf52840/nrf52840/uf2`** (UF2 start 0x26000 for
  the S140-based Adafruit bootloader; the non-uf2 target links at 0x1000
  and would clobber the SoftDevice)
- **⚠ THE BIG ONE: `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` (+_500PPM).**
  These clones omit the 32.768 kHz crystal the board target assumes.
  Without RC the kernel hangs at the first sleep: no timers, no USB, app
  "runs" but everything looks dead. Cost us the whole first hour.
  Sync implication: k_uptime ticks off calibrated RC (~±250 ppm) instead
  of a 20 ppm crystal — well inside what the gossip handles (tested to
  ±3000 ppm), but consider deriving the sync clock from an HFXO-fed TIMER
  if we ever chase precision.
- **Do NOT add a second CDC-ACM node** — the board dts already provides
  `board_cdc_acm_uart` and points `zephyr,console` at it; reference that
  node for the DTE-rate callback.
- **Hands-free DFU WORKS on this bootloader** (nice!nano UF2 0.6.0):
  app watches CDC for a 1200-baud open → `NRF_POWER->GPREGRET = 0x57` +
  reset → NICENANO drive → copy UF2. `firmware/ring-test/flash.sh` does
  the whole loop. (Older v1-era bootloaders do not honour the
  1200-touch/GPREGRET entry; this vintage does.)
- Clone has no visible user LED on P0.15 — don't use blinky as a liveness
  check; use the ring or USB enumeration.
- Flash-verify trick: in bootloader mode, `CURRENT.UF2` on the drive is a
  readback of flash — parse blocks and diff against your UF2 to prove an
  image actually landed.
- Pro Micro pin caveat: module silk labels nRF port numbers (006, 017…);
  RAW (header, diode-OR'd rail) vs B+ (battery pads via charger) are
  distinct — power the ring from RAW on USB.

## Stage-2 bring-up findings (2026-07-17 evening — Thread WORKING end-to-end)

`firmware/thread-ring/` is live: joined the ring-net mesh as an FTD router,
heartbeats on the gateway dashboard as **led-ring**, remotely commandable over
the mesh (UDP :4242 — flash <ms>/off, scramble <ms>, pattern <n|auto>,
bright <pct>, hb, status), photographed glowing via ring-gateway's camera.
Field notes:
- **Partition merge is minutes-lazy.** A leader with the same dataset a
  meter away does NOT merge quickly; only a fresh attach (detached state)
  joins promptly. Firmware fix that works: lonely-leader self-heal — a
  leader with zero neighbors for 60 s calls otThreadBecomeDetached() to
  force a re-scan. Essential for remote boards (else unreachable).
- **`ot discover` on the Zephyr shell can wedge the OT CLI** (commands
  echo, no output, radio degraded) — avoid; reboot clears it.
- **Dashboard name rule**: node names must be word-word[-word],
  lowercase alpha ("led-ring" ok, "ledring" rejected as ghost).
- Send heartbeats to ff03::1:1234 with IPV6_MULTICAST_HOPS=8, format
  HB:<seq>:<name>@<ml-eid>@v<fw>@up<s>@rssi<dbm>@hwok.
- Node addresses: ML prefix comes from the MERGED partition's active
  dataset (OTBR's provisioned fdca:... prefix, not our compile-time
  fdde:ad00:beef default). ML-IID persists across reboots.
- **Pi-side gotchas hit today**: (1) ring-gateway's working tree had 87
  files zeroed (power loss / root-git-op during update) — `git restore .`
  healed; consider an fsck+restore step in the updater. (2) MG24 RCP RX
  can hard-wedge (TX fine, RxTotal frozen) surviving watchdog AND
  container restart — only a physical USB reseat fixed it; the watchdog
  can't detect deaf-RX (add frozen-RxTotal check). (3) udev hotplug →
  otbr-provision recreate works great after reseat.
- Board #2 TODO: derive NODE_NAME + pattern seed from the factory MAC
  (FICR) instead of compile-time constants, so one binary serves all
  boards distinctly.
- **Camera timing instrument WORKING** (`pi-tools/flash_timing.py`, deployed
  to /home/pi on ring-gateway; `pi-tools/ringctl.py` = name-resolved mesh
  remote control). First run vs the live board: 60 fps sustained, period
  measured 1999.4ms vs commanded 2000ms — i.e. the node's RC clock error
  (~300 ppm) measured OPTICALLY — with 30-40 µs MAD edge jitter. Plenty of
  precision to validate ms-level sync. Known quirks: a ring can split into
  2 blobs (merge-by-proximity TODO), and specular reflections register as
  extra blobs with ~10ms lag (shield or auto-reject by response shape).
  Picamera2/numpy/opencv now installed on ring-gateway. Backlit window makes
  daylight harder; night is ideal.

## BASELINE MEASUREMENT (2026-07-17, the "before" number)

Two boards (opal-reef-tide + feral-tide-light, identical thread-ring fw,
both children of the OTBR, both in camera view), flash-mode 2000 ms,
45 s / 22 flash pairs at 60 fps:

- **Ring-to-ring flash offset: 47.8 ms ± ~0.3 ms** — the unsynchronized
  arbitrary phase difference. THIS is the number the gossip engine must
  collapse to single-digit ms and hold.
- Both boards' RC clocks: 1999.43-1999.45 ms measured vs 2000 commanded
  (~280 ppm fast, matching calibrated-RC expectations) → relative drift
  between the two boards only ~10 ppm ≈ 0.6 ms/min offset wander.
- Instrument precision: 0.3 ms MAD across pairs; per-ring period jitter
  0.04-0.09 ms. Reflections register as extra blobs ~30-50 ms laggy —
  identify rings by centroid row (top of frame).

NEXT MILESTONE: implement the gossip sync engine (this doc, top half) in
thread-ring; success = this same measurement reads < 10 ms and stays there
through `scramble` disturbances.

## Deployment gaps — what this doc does NOT specify (firmware-session checklist)

The algorithm layer above is complete and sim-verified; these are the
ordinary scaffolding decisions still to make:

- [ ] **Wire format**: exact beacon bytes (suggest packed LE struct:
      u32 magic/ver, u64 logical_us, u64 raw_us, i32 rate_ppb, u32 seed,
      u32 view_bitmap ≈ 32 B) + UDP port choice.
- [ ] **Network config**: pre-shared OpenThread operational dataset
      (network key, PAN, channel 25/26) compiled in — up to 24 nodes, no
      commissioning ceremony. `CONFIG_OPENTHREAD_FTD=y`, rx-on.
- [ ] **Identity**: 5-bit node ID + 32-bit pattern seed generated at first
      boot (or flash-time), persisted in NVS/settings.
- [ ] **Time source**: one monotonic µs counter for `local` (never reset;
      k_uptime via TIMER, or otPlatAlarmMicroGetNow). int64 µs everywhere;
      float only inside pattern rendering (FPU handles it).
- [ ] **Stamping**: v1 = stamp in send path + timestamp in recv callback
      (sim shows this is good enough); v2 option = SFD capture via
      RADIO→PPI→TIMER if we ever chase sub-ms.
- [ ] **LED glue**: led_strip SPI on P0.06, EN_LED P0.17 high after boot,
      gamma table, global brightness/power cap, 30-60 fps render loop
      calling the pure pattern fns with logical time.
- [ ] **RSSI**: per-neighbor smoothed RSSI from received beacons feeds the
      soc/hysteresis layer (thresholds must be calibrated on-body — sim
      lesson: size "close" against real huddle distances).
- [ ] **Ops**: button behavior (P0.20), VBAT sense (P0.31/AIN7), daytime
      deep-sleep mode, UART/RTT debug shell that dumps peer table + deltas
      (the firmware twin of the sim's __np hook), watchdog.
- [ ] **Bring-up plan (5 boards first)**: 1 node alone (patterns render,
      singleton partition) → 2 nodes (clock lock, measure deltas via debug
      shell, expect <5 ms) → 3+ (group mint fires, phase-lock gate) →
      scramble test (reset one node's clock, watch heal) → then 25.
      The combined simulation is the reference implementation to diff against.

## Thread/Zephyr porting notes (this board: nRF52840 SuperMini carrier)

- Gossip on `ff02::1` (link-local multicast) — inherently single-hop; no
  routing or flooding wanted.
- Stamp TX as late as possible; timestamp RX in the radio event. nRF52840 can
  capture SFD timestamps via RADIO→TIMER PPI for µs accuracy — but ms-level
  software stamping in the receive callback is already ~10× better than needed.
- OpenThread's native network time sync (`OT_NETWORK_TIME_SYNC`, 802.15.4 MAC
  timestamps) is more precise but hangs off the Thread Leader — exactly the
  dependency this design exists to avoid. Right for static installs, wrong
  for this.
- The constants above mirror the original browser simulation.
- Firmware estimator source: `firmware/thread-ring/src/clock_estimator.c` is
  pure C and is replayed by `tests/c/clock_estimator_test.c`; `sync.c` supplies
  stamped, freshness-gated peer samples but does not duplicate estimator math.
