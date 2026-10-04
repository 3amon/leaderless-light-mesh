# Join/Leave/Sync Gap Hunt — August 2026

> **Status (2026-08-16, v0.1.80):** the findings below were triaged and
> fixed the following day. G1 (clock trust gated on reciprocal
> agreement + stranger weight for over-cap peers), G2 (grace reachable:
> silence departs ~54 s, hearing/freshness decoupled), G3 (clone
> detection + counter), G6 (ghost expulsion by reception duty), C1
> (provenance rank), C3 (store migration), C4 (2^31 tie-break), C5–C9
> and the low/vestigial items. **G4 was deliberately NOT fixed**: the
> integrated sim (I6) showed timing demotion already stabilizes the
> healthy scene and drops the faulty node to solo — escalating to
> expulsion would only add rejoin/re-mint churn. Two documented
> inherent bounds remain: cold-boot majority capture and
> turned-incumbent (majority-of-established-members) capture, both in
> `tests/c/campaign/integrated.c`. The `sync_engine` extraction (§5.1)
> is scheduled after the USB round.
>
> **2026-08-17 addendum:** the extraction and tuning shipped as
> v0.1.82, which was then **withdrawn**: the wire_legacy field grew
> `struct sync_peer` past the 4 KB main stack (groups_tick's buffers)
> and every board wedged into the watchdog seconds after joining a live
> mesh. v0.1.83 (main stack 6144, groups_tick buffers static) is the
> corrected release, bench-verified. Moral: pure-code review and host
> tests cannot see embedded stack limits — audit stack when anything on
> the main-loop path grows, and bench-test on a live mesh (a quieted
> mesh hid this bug during the only 'healthy' v0.1.82 run).

Exhaustive adversarial campaign against the cohesion + clock protocol,
plus a two-lens code inspection of the firmware and Pi tooling.
**Report only — nothing here has been fixed yet.** Every simulated claim
below was measured with the real `group_engine` / `clock_estimator`
sources compiled into host harnesses (batches reproduced under
`tests/c/campaign/`).

Fleet context at time of writing: 4 bench rings on v0.1.79, 3 travelers
on v0.1.76, ~20 boards powered off across earlier versions.

---

## 1. Instruments built

| Harness | What it models |
|---|---|
| `campaign.c` (M1–M10) | Real group engine per node, real schedules/slot policy/freshness windows; directional loss, partitions, rogue beacons, reboots |
| `campaign2.c` (R-series) | Same, plus `group_engine_events` capture at an observer and per-second mint/size accounting |
| `campaign3.c` (M10r, H1, H2) | Lower-id sid collision; handoff-hold interruption; double-join |
| `integrated.c` (I1–I4, I2b) | **New coupled simulator**: every node runs the real group engine *and* the real clock estimator, wired exactly as firmware wires them — engine membership weights feed clock-vote weights, estimator per-peer deltas feed the 80 ms lock/demotion gates. Crystal ppm error, transit jitter, snap/rate dynamics all live. This closes the class of gap that previously hid the v0.1.75 rate-trim ratchet. |

~30 scenarios total; existing 9-scenario `cohesion_sim_test.c` and
4-scenario `clock_sim_test.c` suites still pass unchanged (208 host
tests green).

---

## 2. Confirmed protocol gaps (ranked)

### G1 — Membership is granted without reciprocity, and membership = full clock trust
**The most important finding of the campaign.** A node becomes a
*member* (clock-vote weight 1.0) after ~20 ticks of one-way direct
visibility. Reciprocity is only checked by the *topology/agreed* layer,
which gates scenes — not by membership, which gates clock weights.

Measured consequence (integrated sim, I4): among 6 nodes, **3
coordinated liars capture the honest crowd's clock to within 5 ms of a
10-second lie**, while 2-of-6 are fully resisted. The capture boundary
is *half the members*, not half the agreed set. Throughout the capture
the honest scene stays lit and looks perfect — a clock capture is
**invisible to the eyes**, exactly like the v0.1.75 trim incident.

Mitigating data: one sub-threshold liar (150 ms, inside
`outlier_reject_us`) produced no measurable pull beyond the known
collective wander (I3); a single big liar is voiceless. The gap only
opens at liar-majority-of-members, but membership is cheap to obtain.

Related: a stranger beyond the 24-peer cap never gets an engine weight
(sentinel −1 → shim skips `sync_set_peer_weight`) so its clock vote
**stays at the initial 1.0** — over-cap strangers vote at full weight
while in-crowd non-members are correctly discounted to 0.05.

### G2 — The 45-tick silence grace is dead code; real departure is ~31 s
Batch-2 R2 pinned it exactly: a member that goes silent is dropped at
**31 s** via the visible-loss path (12 s freshness window + 20
LEAVE_TICKS), never via the documented 45-tick GRACE_TICKS path, which
is unreachable for pure silence. Not a stability bug — the crowd never
darkens — but the documented grace semantics don't exist in practice,
and any future tuning of GRACE_TICKS would silently do nothing.

### G3 — A cloned node_id causes perpetual identity churn with no detection
Two nodes sharing a node_id (provisioning mistake, flash clone):
**116 scene mints in 600 s vs ~10 baseline** (R7) — the crowd re-rolls
its pattern every ~5 s forever. No darkness, no split, and *no
telemetry that says why* — from the outside it looks like "the crowd
can't settle." Nothing in the beacon path detects self-collision
(same node_id, different seed).

### G4 — A chronically drifting member is never expelled
Integrated I2/I2b: a member whose crystal error exceeds what `h` can
compensate (residual ~3000 ppm past the ±0.6 % clamp) is
timing-demoted (clock vote removed — correct) but **retains scene
membership and stays lit forever**, rendering visibly out of phase in a
0–160 ms sawtooth as its own snap re-converges it. Soft demotion is
doing its churn-prevention job perfectly (1 mint, 0 size flips in
600 s) — but there is no escalation path from "chronically demoted" to
"expelled," so a hardware-faulty ring degrades the look indefinitely.

### G5 — Pair-formation cliff under extreme loss
M1/R1b: two cold nodes join in 13 s at 0 % loss, 234 s at 85 %, and
**never within 900 s at 90 %** symmetric loss. Milder than feared
(85 % still converges), but at edge-of-range in the field two isolated rings
may simply never pair. Larger crowds are unaffected (mass cold start of
24 nodes converges in 29–36 s even with loss).

### G6 — Ghost membership (design tension, not a bug)
A flapper dark 88 % of the time retains full membership indefinitely
(freshness resets on each reappearance). The scene identity includes a
node that almost never renders it. Intended per the freshness rules,
but worth a deliberate decision.

### Observations (not gaps)
- `lock_on == lock_off == 80 ms` means zero *level* hysteresis; all
  hysteresis is temporal (3/5 ticks). Works in every scenario tested,
  but a node hovering at exactly 80 ms flaps its lit state at tick rate.
- Mint on admission is by design ≥1 per join; H2 confirms two joins
  10 s apart cost only 2 mints (no cascade).

---

## 3. What we tried to break and couldn't (clean bills)

| Scenario | Result |
|---|---|
| Cold newcomer (700 ms clock error) joins converged 5-crowd (I1) | 0 s incumbent darkness; six-lit at 53 s. The feared join-lock dip **does not exist** — candidacy outlasts clock convergence |
| Lower-id newcomer collides with incumbent's sid (M10r) | Incumbent repairs on the **first tick** (round 0→1), 0 s dark, admitted at 26 s |
| sid 31 (top bit) minting (R11) | Bitmask correct |
| Joiner dies mid-admission at every offset 5–40 s (H1) | 0 s dark, ≤2 mints, hold ticks bounded at 4 — no wedge |
| Slot exhaustion, 26 nodes → 2 low-id deaths (R6) | Excluded high-id node promoted 72 s after slots free (retention-bound) |
| Mass death 12-of-24 | 0 dark seconds, re-mint at 37 s |
| Mass cold start, 24 nodes | Converged 29–36 s, 8–20 sid repairs absorbed |
| One 150 ms sub-threshold liar (I3) | No pull beyond baseline wander |
| 2-of-6 coordinated 10 s liars (I4) | Fully resisted |
| Chains, flapping bridges, partitions/merges, rolling reboots, asymmetric links | All held (batch 1 + existing suite) |

---

## 4. Code inspection findings

Two independent review passes (API/consistency lens; concurrency/state
lens) over `firmware/thread-ring/src` and `pi-tools`, verified against
source. Ranked by what would actually bite us.

### High

**C1 — Mixed-era v2 echo reverts operator duty/wave fleet-wide.**
v0.1.78+ transmits v3+v1 only. A v0.1.74–77 node learns the register
from the v1 copy (no duty), keeps its stale local duty (≈50), and
re-gossips it as **v2 at the same priority**. On a v3 node the duty
tie-break ("nearest 50 wins") lets that echo beat a deliberate duty 80
— and beat duty **100 (solid)**. While any 74–77 node is alive, duty
and wave are effectively uncontrollable, and the reverted value
re-propagates as v3. Directly relevant to the 3 travelers on v0.1.76.

**C2 — `mode ultra` (blink=0) renders differently on every era.**
On v0.1.78+, blink 0 = no effect → solid fast ultra; on 71–77, blink 0
= default → 320 ms hard blink. The *default form of the ultra command
splits the fleet visually*, and an old node commanding ultra silently
inverts its intent on new nodes (they render solid). Only
`ultra blink=320` renders identically everywhere. Any blink on
normal/party is invisible on ≤77 (effect was ultra-only there).

**C3 — Persisted mode register is silently discarded across the
v0.1.77→78 boundary.** `mode_store` grew 16→18 bytes with
`MODE_STORE_VERSION` left at 1; `load_cb` rejects on size mismatch in
*both* directions. After OTA to 78+, the first power cycle boots every
node at normal/priority-0 — register and priority gone. **This
directly affects the planned USB update round**: boards coming from
≤77 will forget their mode on their first post-update power cycle
(they'll re-learn from mesh gossip if a live peer holds the register,
but an all-at-once power cycle of the whole fleet loses it).

### Medium

- **C4** `network_mode_policy.c` compare() is non-antisymmetric at
  priorities exactly 2³¹ apart — both sides reject the other →
  permanent two-faction stall until a fresh command. Unreachable via
  +1 stepping, but nothing guards against divergent islands.
- **C5** `g_flash_period_ms`: shell `ring flash off` can zero it
  between the main loop's check and modulo → 64-bit divide-by-zero UB.
- **C6** `lonely_leader_heal()` holds the OpenThread mutex across an
  NVS `settings_save_one` (up to hundreds of ms with sector GC) —
  the one violation of the documented OT-mutex discipline, and it
  stalls the radio exactly while the node is lonely.
- **C7** Shell-thread `ring hb` shares the beacon fd non-atomically:
  interleaved `setsockopt(hops)`/`sendto` can send a heartbeat with
  hop-limit 1 that silently dies — dashboard shows a missed heartbeat
  with nothing logged.
- **C8** Shell thread writes render state (`g_pattern_override`,
  `g_brightness`, layout/power) and does LED I/O concurrently with the
  main render loop, contradicting the documented single-writer claim —
  transient garbage frames on `ring layout`/`ring power`.
- **C9** `ringctl` auto-priority is an unatomic read-modify-write
  (two operators race to the same bid) **and never checks
  `result=rejected/unchanged`** — the loser sees success-shaped output
  and exits 0.

### Low (selected; full agent reports in the session log)

- Registers with duty 0 vs 50 render identically but never converge
  byte-wise (compare()==0 both ways) — permanent benign gossip
  disagreement visible to any future digest/telemetry.
- First frame of a freshly minted scene renders with default
  party/ultra/effect flags (set-before-tick ordering) — one ~30 ms blip.
- `g_dfu_last_chunk` torn-read window at DFU start can bounce the LED
  rail for one frame mid-upload.
- v1-echo inherit has a microsecond TOCTOU that can revert a
  just-adopted duty; RX-thread NVS write under `g_mode_mutex` stalls
  beacon stamping fleet-wide on simultaneous adoption.
- Shell commands in the first tens of ms after USB-up touch
  uninitialized mutexes/engines; `ring status` can index
  `genome_base_names[-1]` via a double-read race.
- `--allow-legacy-control` is a dead flag (firmware unconditionally
  requires auth for mutations); ringctl scrapes *effective* values from
  reply text and re-submits duty 50 for stored 0; a bare
  `ringctl <t> mode party` wipes a configured blink/duty/wave.
- `network_mode_blink_period_s(0)`'s 320 ms-default branch is dead in
  firmware but still asserted by a host test — a trap for any future
  unguarded caller (it turns "no effect" into a blink), and the test
  cements the pre-v3 semantics.
- Vestigial: `SOCIAL_SCENE_ULTRA_BLINK_PERIOD_S` (dead),
  `network_mode_encode_v2` (test-only — its absence from TX is the
  direct cause of C1), `sync.c` `gtx` fallback socket (known
  nonfunctional, fd never closed), `social_scene_plan.ultra`
  (write-only in firmware).
- `group_engine.c` peer-weight array: init loop is bounded by
  `SYNC_MAX_PEERS` but the main loop writes `peer_weight[i]` for
  `i < n` unbounded — host-side OOB if a harness passes n > 23.
- Operational: stale build trees (`build-partition-hil`,
  `build-provisioning`, `build-rollback`, others) predate the
  0x2003d000 ladder carve — reflashing those *artifacts* would
  re-create the RAM collision. Current source overlays all agree.

### Verified sound (worth recording)
Client/firmware auth command lists agree exactly; datagram-96
arithmetic and token caps are consistent end-to-end; parse-error
replies keep the signed envelope; `sync.c` core locking (estimator,
peers, stats, spinlocked doubles) is clean with no lock inversions;
escape-ladder arming is literally `main()`'s first statement and the
RAM carve matches all three overlays; v1-echo duty protection works
(it is specifically the v2 path that doesn't, C1).

---

## 5. Refactor recommendations (for discussion, not done)

1. **Extract `sync_engine`** — the neighbor-table glue in `sync.c`
   (dedupe by `last_raw`, slot eviction, weight application, freshness
   stamping) is the last decision-bearing code that is Zephyr-bound and
   therefore *unsimulated*. Same treatment as `group_engine` /
   `clock_estimator`. The integrated simulator is ready to consume it.
2. **Gate full clock weight on agreed (reciprocal) membership**, not
   visibility-membership, and give over-cap strangers the 0.05 stranger
   weight instead of default 1.0 (G1). This collapses the capture
   boundary from "half the members" to "half the agreed set."
3. **Self-collision detection**: drop/flag beacons carrying our own
   node_id with a foreign seed; a counter in `ring group` status would
   have made G3 diagnosable from the dashboard.
4. **Retire or realign GRACE_TICKS** with reality (G2), and decide
   ghost-membership policy (G6) deliberately.
5. **Chronic-demotion escalation** (G4): N consecutive
   demote/readmit cycles or M minutes cumulative demotion → drop to
   candidate.
6. **Mixed-era register hygiene** (C1/C2/C3): bump
   `MODE_STORE_VERSION` with a migrating `load_cb`; consider having v3
   nodes re-encode inherited-duty echoes rather than tie-breaking them;
   document `ultra blink=320` as the only era-safe ultra until the
   fleet is homogeneous.
7. **Shell-thread audit** (C5/C7/C8/C9 cluster): either mutex the
   handful of shared scalars or route shell mutations through the
   control-plane queue the main loop already drains.
8. Delete the vestigial inventory (§4 low list) and the stale
   pre-carve build trees.

---

## 6. Interactions with the planned USB update round

- Boards on ≤ v0.1.77 will **lose their persisted mode register** on
  first power cycle after update (C3) — expect them to boot
  normal/priority-0 and re-learn from mesh gossip; fine while any
  up-to-date peer is alive, surprising if the whole fleet power-cycles
  together.
- Until the travelers (v0.1.76) are updated, avoid non-default duty and
  fade fleet-wide (C1) and prefer `blink=320` if ultra is wanted (C2).
- Do not flash from the stale pre-carve build trees; build fresh or use
  the released artifacts only.
