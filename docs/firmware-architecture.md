# Firmware architecture

The firmware is moving from a working prototype organized around `main.c` to
small modules with explicit state ownership and host-testable policy. Hardware
drivers and Zephyr scheduling remain at the edges; protocol, transition, and
rendering decisions should be pure C wherever possible.

## Responsibility map

| Area | Current owner | Target boundary |
|---|---|---|
| Boot evidence | `boot_status.c` plus `main.c` | Persistent events and boot-state policy |
| Application state | `app_state.c` | Pure BOOTING/SEARCHING/ISOLATED/HEALTHY/DFU/RECOVERY policy |
| Mesh health | `main.c`, `sync.c`, `groups.c`, `group_policy.c` | Pure membership, consensus, and lock policy |
| OTA lifecycle | `ota_policy.c` plus `main.c` | Pure confirmation/rollback timing and MCUboot side effects |
| OTA traffic quieting | `ota_quiet.c` plus `main.c` | Pure bounded timer and background-radio suppression |
| Control and telemetry | `control_protocol.c` plus `main.c` | Pure parsing/encoding and orchestration side effects |
| Clock protocol | `sync.c` (shim), `sync_engine.c`, `beacon_codec.c`, `clock_estimator.c` | Achieved: sockets/schedules in the shim; table, beacon acceptance, clone detection, and estimator stepping pure in `sync_engine` |
| Rendering | `social_scene.c`, `genome.c`, `groups.c`, `main.c` | Pure automatic scenes and manual bases consumed by the LED driver |
| Supervision | `main.c` | Progress-based watchdog health supervisor |

Module extraction should preserve the fleet build gate and proceed one state
owner at a time. Each seam needs host tests before hardware behavior changes.

## Application state machine

`app_state.c` owns the application-state decision and transition history. It
has no Zephyr dependencies: `main.c` samples uptime, reset class, Thread
attachment, infrastructure visibility, and recent DFU activity, then passes
that observation into the state machine. Rendering and side effects consume
the resulting state but do not create alternate transition rules.

Priority and transition policy:

1. During the first 2.4 seconds, an ordinary start is `BOOTING` and a
   watchdog, CPU-lockup, heal, or rollback start is `RECOVERY`.
2. After the boot marker, recent image-upload traffic selects `DFU`.
3. A detached node is `SEARCHING`.
4. An attached node without border-router Network Data remains `HEALTHY`
   during the existing 12-second loss grace, then becomes `ISOLATED`.
5. An attached node with infrastructure is `HEALTHY`.

The camera flash mode remains a diagnostic render override, not an
application state. This preserves the ability to measure clocks during other
states without hiding the underlying state from telemetry.

True partition/rejoin HIL uses a separate signed `partition-hil` build. Its
bounded detach command is compiled only when
`CONFIG_THREAD_RING_HIL_FAULT_INJECTION=y`, is exposed only on the physical USB
shell, and automatically re-enables Thread. The normal UDP control plane and
fleet image gain no partition command.

## LED rail power boundary

Carrier PCBs gate the WS2812 rail through active-high `EN_LED` on P0.17. Both
the standalone `ring-test` image and `thread-ring` describe that signal as the
`led-power` devicetree alias. Startup first configures the GPIO at its inactive
level, then enables the rail and waits 10 ms before rendering. A missing GPIO
controller is a startup failure rather than permission to send data into an
unpowered strip.

Before a controlled application reboot, `thread-ring` renders black, allows
the final strip update to settle, and disables the rail. The 1200-baud UF2
handoff also disables the rail before reset. The carrier's hardware pull-up on
the high-side switch keeps the rail off while P0.17 is high impedance during
reset, so software enable is always explicit.

The USB configuration describes the carrier truthfully as bus-powered and
requests the USB 2.0 500 mA high-power ceiling. Zephyr's defaults describe a
self-powered 100 mA device; retaining those defaults can make managed computer
ports or hubs current-limit a live ring even though dedicated supplies appear
reliable. Normal fleet scenes remain at the 45-percent bench cap. Use a
dedicated supply rather than a computer port for deliberately higher manual
brightness.

## MCUboot watchdog breadcrumbs

An application-started nRF watchdog continues across a warm reset and protects
MCUboot's image validation and move-swap work. The tracked MCUboot patch feeds
that inherited watchdog during each image-hash chunk, before signature
verification, and before every traced validation, swap, and application handoff
phase, in addition to the upstream erase/copy loop feeds. It records the current
boot phase in `GPREGRET2` and a sector/slot detail byte in `GPREGRET`, without
logging or writing flash. A successful handoff restores the
application's retained reboot reason. If the watchdog interrupts MCUboot, the
next boot freezes the breadcrumb until the recovered application reads and
clears it; the persistent `boot` reply reports it as `mcuboot=phase/detail`.

Phase values are:

| Phase | Last operation started |
|---|---|
| `d0` | MCUboot entry |
| `d1` | secondary validation or move-swap start |
| `d2`–`d4` | move-sector erase/trailer, copy, status write |
| `d5`–`d7` | swap state 0 primary erase, secondary-to-primary copy, status write |
| `d8`–`da` | swap state 1 secondary erase, primary-up-to-secondary copy, status write |
| `db` | post-swap finalization |
| `dc` | primary-slot validation |
| `dd` | application handoff |

The detail byte always has bit 7 set to avoid the UF2 `0x57` reset magic; its
low seven bits contain the sector or image-slot index, except that encoded
values `0xa8` and `0xb1` are native-bootloader BLE OTA DFU magics and are
aliased to the out-of-range sentinels `0xff` and `0xfe` (sectors `0x28` and
`0x31`) so a reset can never strand the board in invisible OTA DFU mode.
`00/00` means no frozen MCUboot failure was reported. The retained trace
localizes an interrupted operation; it does not replace the persistent
application boot trail or raw hardware reset cause.

## Boot-recovery ladder

`escape_ladder.h` (one header compiled verbatim into MCUboot via the patch,
the application, and the host tests — `tests/test_escape_ladder.py` enforces
the copies match) implements the buttonless-recovery ratchet decided in
`docs/boot-recovery-ladder.md`:

- MCUboot counts every boot attempt in retained SRAM (fixed address
  `0x2003d000`, at the base of a reserved region that also covers the
  native bootloader's top-of-RAM stack, carved out of every image's
  `sram0`), then makes
  sure a 15-second watchdog is counting before validation or swap work —
  including after a watchdog reset, which disables the nRF WDT and used to
  leave MCUboot plus early application init unprotected.
- The application arms/adopts the same watchdog as the first act of
  `main()`, before any settings, LED, or USB init, and clears the attempt
  count only after the USB control plane is up and the main loop has run
  for 30 seconds. Deliberate reboots clear the count in
  `mark_reboot_reason()`.
- At four consecutive failed attempts MCUboot stops retrying: with USB
  power present it writes the UF2 magic and resets into the native
  bootloader (the node self-presents as a NICENANO drive; recovery is
  copying the release UF2), on battery it parks in System OFF where USB
  insertion both wakes it and, if the failure persists, unlocks the escape
  within another four attempts. With a live inherited watchdog it spins
  unfed instead, letting the watchdog reset (which disables the WDT and
  retains SRAM) so the native bootloader is never handed a running
  watchdog.

The current attempt count is reported as `ladder=N` in the persistent `boot`
reply. The whole policy is pure C with host tests; only the register helpers
touch hardware.

## OTA lifecycle policy

`ota_policy.c` owns the infrastructure gate, five-second confirmation retry
cadence, unconfirmed-image rollback deadline, and whether infrastructure has
been observed during the current boot. `main.c` supplies uptime, Thread
attachment,
Network Data visibility, and MCUboot confirmation state; it performs only the
resulting trailer write, breadcrumb record, or clean reboot side effect. A
private ring-only partition is therefore never sufficient evidence that a
candidate remains remotely recoverable.

The `native_sim/native/64` boot/OTA test compiles the production policy and
`boot_status.c` against Zephyr's kernel, settings subsystem, NVS, CRC, and
flash simulator. It verifies confirmation gating/retry and the exact rollback
boundary together with persistent boot-stage reload and bounded formatting,
without USB, radio, LEDs, MCUboot hardware, or physical flash. `scripts/ci.sh`
runs this application before every signed fleet build.

### OTA quiet mode

The authenticated UDP command `quiet <seconds>` suppresses sync transmissions,
scheduled heartbeats, and singleton-heal reboots while leaving Thread attached
and the control and SMP endpoints active. `quiet` queries the rounded-up
remaining duration and `quiet off` cancels it. The pure `ota_quiet.c` policy
owns the 30-second to four-hour deadline. Its state is deliberately RAM-only,
expires without operator intervention, and clears on reboot. Status replies
report `quiet=<seconds>s`. Incoming gossip remains enabled, allowing a node to
retain clock observations without contributing background multicast during an
OTA window.

## Clock and membership observability

The compact UDP `status` reply reports `nbr=locked/fresh/tracked`. The first
two values preserve the original locked/fresh interpretation; the third makes
the 90-second retained-neighbour table visible. `age` is the oldest retained
peer's beacon age in milliseconds, `lapse` counts fresh-to-stale transitions
since boot, and `sched=compat|scalable` exposes the active rolling-migration
profile. A starting lapse count is evidence about earlier uptime, while a
counter increase during a bounded soak is a new failure.

The `group` reply deliberately reports a different layer: each peer's
hysteretic membership state plus direct-radio visibility, `fresh`, `age`, and
RSSI when OpenThread has it. Membership may remain `MEM` during the
20-tick visibility-loss window (peer still heard, just not direct) or the
45-tick silence grace (nothing heard within the 15 s "hearing" window —
since v0.1.80 the two are discriminated by reception age, which makes the
grace actually reachable: pure silence departs at ~54 s, and a <50 s
reboot keeps membership). A member whose reception duty drops below ~25%
of a two-minute window is expelled as a ghost and must re-earn candidacy.
Keeping these facts in the same record makes that intended behavior
reconstructable rather than looking like an inconsistent group decision.

Sync trust weights follow the same reciprocity ladder scenes use (since
v0.1.80): agreed member 1.0, reciprocal-but-damping 0.6, one-way
visibility member 0.1, candidate 0.1–0.6 by progress, stranger (and any
peer without a group table slot, including over-cap) 0.05. Full clock
influence therefore requires the mutual agreement a scene requires — a
one-way rogue can earn membership but stays voiceless in the clock.

The Pi's `status_telemetry.py` is the canonical backward-compatible parser.
`freshness_soak.py` stores raw replies and parsed fields as JSONL, then writes
an explicit threshold result. It requires the new lapse counter rather than
silently passing older firmware that cannot prove the absence of brief gaps.
The qualified five-minute fleet check permits at most three lost management
replies per 600 polls (99.5% reply floor), while keeping firmware freshness,
lapse, and reboot gates at zero tolerance:

```sh
python3 pi-tools/freshness_soak.py --duration 300 --poll 0.5 \
  --max-missing-samples 3
```

Pi management clients create sockets through `thread_socket.py`. It excludes
Thread RLOC/ALOC-shaped `wpan0` addresses and binds to the border router's
ordinary OMR address in the ring destination prefix. This keeps host control
and SMP replies routable when a departing leader causes a partition change
and OTBR retains obsolete locator addresses. Systems without a matching
`wpan0` OMR address retain ordinary kernel source selection.

`group_policy.c` owns tick-based membership hysteresis, peer-presence aging,
reciprocal-topology resolution, damped agreement, and phase-lock level/time
hysteresis. Presence aging continues after `sync.c` forgets a peer, so the
departure grace cannot freeze at the 90-second sync-table lifetime. A retained
group peer is released once that sync lifetime has expired and membership
grace has removed it. Retirement deliberately does not wait for the old
damped-agreement bit: after small-ID collision repair that dependency would be
circular (the stale record can make topology incomplete, while incomplete
topology freezes the agreement needed to release it). The already-minted scene
and bounded handoff preserve visual continuity during the five clean-topology
consensus ticks.

USB charge-quiet mode only overrides rendering. The group control plane keeps
running behind the gentle charging display, allowing a quiet node to resolve a
small-ID collision and publish updated topology. Pausing group ticks here would
leave its sync beacons advertising a frozen identity indefinitely and could
hold every visible peer in incomplete-topology solo mode.

Link-local and realm-local beacons have independent randomized schedules.
Link-local traffic (1.8--2.6 seconds) supplies one-radio-hop contact evidence;
realm floods (8--12 seconds) carry mesh-wide clock/topology state without a
back-to-back multicast burst. A changed view schedules one extra realm
announcement over a randomized 1--5 second window. Direct evidence remains
fresh for 12 seconds, any-path evidence for 45 seconds, and records retire
after 90 seconds. Receipt of a fresh link-local copy is the production
proximity signal. It has no RSSI threshold: the weakest packet the radio can
receive is eligible. RSSI remains diagnostic telemetry and may be unavailable
when the directly heard device is not also an OpenThread protocol neighbour.

Each ring gossips its hysteretic direct-visibility view. The policy accepts an
edge only when both endpoints advertise it, then chooses the connected
component containing self. Components therefore partition the observed graph:
asymmetric links can produce a solo plus a pair but cannot produce overlapping
lit scenes. Realm-local multi-hop gossip alone never creates a proximity edge,
so it cannot collapse every reachable node at a venue into one giant group.
Unknown IDs, duplicate IDs, malformed views, or an incomplete snapshot fail
closed. Agreement freezes until the snapshot is complete, and a new scene can
commit only when its deterministic topology target exactly equals the agreed
bitmap and every member has a timing sample inside the 80 ms lock bound. A
currently lit certified scene remains committed for at most eight one-second
topology ticks while a new membership set converges. This bounded interval
absorbs the ordinary gap between reciprocal topology and damped agreement
without allowing malformed or permanently incomplete topology to remain lit
indefinitely.

`groups.c` supplies peer views and median offsets, then owns radio lookup,
group-scene minting, and rendering state. The lock policy requires timing
samples for every known non-self member; an empty or stale sample set is unlock
evidence, never an implied zero-offset lock. Lighting a group requires three
complete median-sample rounds below 80 ms. Five consecutive missing rounds or
offsets above 80 ms extinguish it. This level reflects hardware evidence:
beacon timestamp telemetry can report 20–70 ms excursions while camera
measurements show the actual LED clocks remain synchronized. The former
sub-6 ms acquisition gate left different members of five-ring groups stuck in
solo mode despite identical reciprocal agreement. A membership transition now
builds as a pending set while the previous certified group remains visible. If
the pending set becomes topology-complete and timed within eight ticks, the
renderer crossfades directly from the committed group scene to the new group
scene. A subsequent membership change waits for that three-second blend to
finish before replacing either layer, so staggered joins cannot discard a
still-visible scene. If the pending set does not certify within the bounded
handoff interval, the old lock expires and the existing two-second fail-closed
visibility fade returns the device to solo.

The Pi-side mobility qualification records `commit` (the scene membership
actually being rendered) and `hand` (elapsed committed-scene grace ticks) from
every target. It creates a reversible proximity split by changing one ring's
authenticated gossip transmit mode to realm-only, which leaves Thread, OTA,
clock receive, and management reachable. Restoring the default `both` mode
reforms the crowd. The run fails if a resident loses its group lock, falls
below full group visibility, omits another resident from its committed scene,
reboots, or never exercises the handoff path; cleanup restores `both` even when
the run fails.

## Social LED scenes

`social_scene.c` is a pure renderer driven by a seed, agreed member count,
disciplined global time, and LED index. It has no mutable random state. A
single ring uses its device seed; a lit group uses the existing XOR-derived
group seed. Every member of an agreed, phase-locked group therefore expands
the same plan and renders pixel-identical output without extending the gossip
wire format.

The member-count contract has three tiers:

| Agreed visible members | Palette | Motion period | Families |
|---:|---|---:|---|
| 1 | One hue; luminance variation only | 10–20 s | breath, halo, soft-shimmer |
| 2 | Two harmonized hues at least 60 degrees apart | 3.5–7 s | braid, opposing-arcs, two-tone-tide |
| 3+ | Three to five hues in a cohesive arc | 1.25–2.75 s | kaleidoscope, aurora-ripple, prism-sparks |

The scene plan selects a different family on every adjacent shared one-minute
epoch and crossfades over five seconds. Its anchor hue advances in bounded
steps rather than jumping to an unrelated color. Color interpolation follows
the shortest hue path instead of blending RGB channels, which could turn
separated hues into gray or pastel. Every automatic scene and the final
solo/group composite enforce a 78-percent HSV saturation floor while
preserving peak brightness. Crowd palettes remain within a 160-degree hue arc
and include moving dark space: this prevents a physical diffuser from
integrating simultaneous complementary pixels into a bright near-white field.
Host tests enforce a 35-percent saturation floor after averaging the complete
ring, including minute crossfades.

Group membership changes retain the existing three-second crossfade, but the
solo layer is now the complement of the *sum* of old and new group coverage.
Complementary group-to-group fades therefore never expose the members'
individual scenes at their midpoint. Solo still fades normally when a group
forms from or dissolves to one member. A directly heard candidate that has not
joined may modulate solo luminance by at most eight percent, but cannot inject
a second hue. Manual
`pattern` forcing continues to render the original bare genome bases and
bypasses automatic social scenes.

The `group` reply exposes raw view, deterministic target, topology
completeness, damped agreement, lock, visibility, active tier, and visible
member count. The heartbeat retains the compact agreement/tier/size fields.
Host tests enforce deterministic synchronized rendering, tier palette and
tempo bounds, strict solo-to-duo-to-crowd motion separation, RGB bounds,
continuity across minute boundaries, per-pixel saturation, and diffuser-aware
whole-ring saturation across hundreds of thousands of seed/time/pixel samples.

`clock_estimator.c` owns the disciplined clock state, peer phase/raw sample
windows, step detection, min-delay phase selection, slew/snap correction,
raw-clock regression, rate bounds, and lock classification. `sync.c` owns
socket I/O, arrival timestamps, peer identity and freshness, mutex protection,
and passes fresh peer inputs into the estimator. Host replay uses this same
policy for clean convergence, packet loss and recovery, phase steps, a
300-ppm hardware skew, and partition/merge behavior.

The sync and membership tables retain 23 peers, enforcing a 24-node fleet/group
ceiling including self. The 25-ID bitmap deliberately keeps one spare hash
position for small-ID collision repair; it is not a 25th member slot. A full
table first replaces its stalest record. If more than 24 simultaneously fresh
nodes are visible, every node keeps the deterministically lowest node IDs
rather than an arrival-order-dependent set; the incoming peer is ignored when
it loses that rank. This keeps supported fleets reciprocal and makes true
over-capacity behavior stable and diagnosable.

### Viral fleet mode

Every node persists a `(mode, priority)` pair, initially `normal/0`. The
authenticated `mode party <priority>` and `mode normal <priority>` commands use
the exact caller-supplied unsigned 32-bit priority; firmware never increments
it. A candidate replaces the local value only when its priority is higher. At
equal priority, normal wins as a deterministic conservative tie-break. Thus a
later `normal/20` permanently dominates delayed or rebooted `party/10` state.

The winning pair travels in a separate 12-byte auxiliary frame on the existing
gossip port. This leaves the v3 clock/membership beacon byte-for-byte unchanged
during rolling upgrades; older firmware ignores the unknown-size frame. A
change produces three independently jittered link- and realm-local sends, then
45-to-60-second anti-entropy. Nodes persist a newly adopted winner before
rendering or forwarding it. The Thread network credential is the peer trust
boundary, while the initiating Pi command retains per-device HMAC and replay
protection.

Normal and party each have three deterministic scene families for solo, duo,
and crowd tiers. Party plans use faster motion, dense saturated palettes, and
stronger localized highlights within the configured per-layout brightness.
Party scenes also add a group-synchronized, hue-preserving accent flash: one
smooth 90 ms rise and 280 ms fall every 4.25 to 5.75 seconds. The low cadence,
soft envelope, and lack of white/color inversion deliberately avoid hard or
rapid strobing. Mode gossip is independent of the 24-node sync peer table, so
an adopted winner continues epidemic forwarding through any supported
topology.

## Control protocol boundary

`control_protocol.c` owns the Zephyr-independent UDP command grammar and
telemetry encoding. A datagram is at most 71 printable ASCII bytes and uses
exact command names, argument counts, ranges, and single-space separators.
Malformed, oversized, unknown, or prefix-matched input returns an explicit
error and cannot reach a command side effect. Responses and status records use
checked, bounded formatting; overflow produces an error instead of a partial
record.

Legacy and v1 read-only `status`, `group`, `boot`, and `auth` queries remain
available. Mutations require the v2 envelope
`v2 <16-hex-sequence> <command...> <24-hex-tag>`. The tag is the first 12 bytes
of HMAC-SHA256 over the exact prefix before the tag. Replies use
`v2 <sequence> ok|error <payload> <tag>` and are authenticated the same way.
Lowercase hex and canonical single-space tokenization are required.

`control_auth.c` owns each device's independent 256-bit key and the persistent
sequence/tag record. It validates the MAC, saves a new sequence before the
side effect, and treats an exact retry as an acknowledged duplicate without
repeating the mutation. Older sequences and same-sequence/different-command
requests are rejected after reboot as well as during one boot. An
unprovisioned fleet image fails closed for mutations. Key installation exists
only in the physical-USB provisioning profile; ordinary fleet images cannot
read, install, or rotate keys.

The authenticated `layout ring16|jewel7|rope100` mutation stores a per-device
LED layout in the existing settings/NVS partition. `layout` is a read-only
query, and `status` also reports `layout` and `leds`. Switching first clears
the full 100-pixel transport so pixels removed from the logical chain cannot
remain latched. Render geometry scales to 16, 7, or 100 pixels. `jewel7`
selects a 100% default brightness, `ring16` selects the original 45% default,
and `rope100` selects a 100% default on each boot or layout change. The
ordinary `bright 1..100` command remains a RAM-only override.

`status` and the periodic heartbeat also expose `usb=0|1`, sampled from the
nRF52840 `USBREGSTATUS.VBUSDETECT` bit. This is deliberately named USB
presence rather than charging state: it works without USB enumeration and is
a useful indication that the onboard charger has input power, but the carrier
does not route the charger's status/current signal to the MCU, so firmware
cannot prove that current is flowing or that charging has completed.

`usb_display.c` owns the display policy around that signal. USB present at
boot, or a first/late connection, selects quiet mode. Quiet mode renders three
900 ms green pulses and then a steady low green marker, independently of the
layout brightness setting. A falling VBUS edge restores the normal social
scene immediately; a rising edge no more than 20 seconds later keeps that
normal scene active for the remainder of the USB session. The override is
deliberately RAM-only, so power loss returns to the quiet default. Boot,
recovery, explicit camera-flash, and OTA-progress displays retain priority over
the quiet marker. `rope100` is explicitly an externally USB-battery-powered
installation rather than a charging flower, so it bypasses quiet mode and
reports `usbquiet=0` while continuing its active social scene whenever VBUS is
present. USB presence, OTA safety, and the USB wake gesture are otherwise
unchanged.

`led_power_mode.c` stores a separate per-device LED rail backend. The
authenticated `power carrier|module-vcc` mutation persists the selection in
settings/NVS; bare `power` is read-only. A missing or invalid setting defaults
to `module-vcc`, which drives the SuperMini's P0.13 VCC-enable signal for
low-LED-count assemblies whose LED VCC is wired directly to the module's
switched 3.3 V output. A valid saved choice still takes precedence, so existing
`carrier` targets continue to drive the established P0.17 carrier enable across
firmware updates and reboots. A transition blackens the strip and disables
both candidate rails before enabling the new one, preventing overlap.

`usb_sleep.c` recognizes two VBUS pulses of 250 ms to 8 seconds, completed
within 20 seconds and ending with VBUS absent. The first completed pulse arms
the gesture and renders two green acknowledgements. The second renders a short
green closing fade, clears the strip, disables both LED rails, holds the data
pin low, sanitizes USB, and enters nRF52840 System OFF. USB insertion is the
wake source. The gesture is ignored while an image is unconfirmed or DFU is
active. These timings are intentionally tolerant of manual cable operation;
long-lived charging connections do not count as pulses.

The authenticated `sleep` control mutation enters the same System OFF path
without requiring the two-pulse gesture. It is accepted only with VBUS absent,
no active DFU traffic, and a confirmed running image. The signed UDP response
`sleeping wake=usb` is queued before the green closing fade and shutdown. A
USB-present request fails closed because System OFF requires a subsequent USB
insertion edge to wake.

## Ownership and lock ordering

- `main.c` is the orchestration thread and the only writer of the application
  state machine, render buffers, infrastructure observation, and command
  overrides. Shell commands never touch the LED strip, the layout/power
  backends, or the shared heartbeat/beacon socket directly: they queue
  atomic requests the main loop applies between frames, and commands that
  read sync/groups/mode state wait for core init.
- OpenThread observations are copied while holding the OpenThread API mutex;
  that mutex is released before state updates, LED I/O, sleeps, logging, or
  reboot paths.
- `sync.c` owns its internal mutex and exports copied statistics and peer
  snapshots. Callers do not hold the OpenThread mutex while calling sync or
  group policy.
- The gossip receive thread never writes application or render buffers. It may
  atomically persist and publish a strictly stronger fleet-mode register before
  scheduling its bounded retransmission burst.
- `groups.c` is ticked and rendered by the application thread, so its render
  state requires no additional lock.

## Gossip beacon protocol

Version 3 is exactly 48 bytes. Multi-byte integers use network byte order,
signed clock values use two's-complement bit representations, and rate trim is
an integer in parts per billion. The decoder never casts a received byte array
to a packed C struct.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `GSY3` magic |
| 4 | 1 | Version (`3`) |
| 5 | 1 | Packet length (`48`) |
| 6 | 1 | Flags (currently zero) |
| 7 | 1 | Small group ID |
| 8 | 4 | Node ID |
| 12 | 8 | Disciplined global clock, microseconds |
| 20 | 8 | Raw local clock, microseconds |
| 28 | 4 | Clock rate trim, parts per billion |
| 32 | 4 | Genome seed |
| 36 | 4 | Membership view bitmap |
| 40 | 8 | IEEE 802.15.4 extended address |

Fleet mode uses a distinct auxiliary datagram on the same UDP port, never
interpreted as a clock beacon: `FMD1` magic, version, length, mode, blink
byte, and a big-endian 32-bit priority. Version 1 is 12 bytes; version 2
(v0.1.74) appends a duty byte; version 3 (v0.1.78) appends a waveform
byte and makes the effect MODE-INDEPENDENT: blink 0 (the default) means
no effect in any mode, else the period in 10 ms units applies to
whatever mode is active. The waveform is hard (square) or fade (a
raised-cosine breathe); for hard the duty (20–80, 100 = solid) is the
on-fraction, for fade it is the minimum-brightness floor the breathe
descends to. Nodes transmit v3 plus the legacy v1 copy each tick, so
older targets keep following mode changes and stay fully OTA-able; v2
remains decodable, and v1/v2-learned records inherit the local
duty/wave so old echoes cannot reset a configured effect. Full ties
converge calmest-first: no effect beats slower beats faster, fade beats
hard, then duty nearest 50%. Modes are normal (0), party (1),
and ultra (2) — ultra keeps party's scene identity while compressing every
period a further 0.6× and gating the final color through a shared-clock
half-duty square wave, so the whole crowd blinks in unison. Since v0.1.78 the
blink is a register-wide effect any mode can carry:
`mode <name> <priority> [blink_ms] [duty] [hard|fade]` (blink 0 = none,
else 130–2550 ms in steps of 10), or
`ringctl … mode <name> blink=<ms> duty=<pct> wave=<hard|fade>` with
auto-priority. Bare ultra is therefore SOLID: fast pacing, no blink.
The 130 ms floor is enforced at parse, encode, decode, AND persistence,
keeping every path far below the 15–25 Hz photosensitive band; ties on
priority and mode converge toward the slower blink.
Priorities compare with wrap-around serial-number arithmetic, so "current
priority + 1" wins forever and the scheme cannot exhaust; at equal priority
the calmer mode wins (normal > party > ultra). `ringctl <t> mode <name>`
with no priority queries the device and bids current + 1 automatically.
Nodes older than v0.1.72 fail-closed on mode byte 2 and simply keep their
last mode.

### Rolling migration

v0.1.64 transmits only v3; the preceding fleet release already accepts v3.
Every v0.1.64 node boots on a corrected 450--550 ms compatibility cadence and
stays there while it receives any legacy v1/v2 frame. Because v0.1.64 never
originates a legacy frame, that is unambiguous evidence of an older peer. After
120 seconds with no legacy traffic, each node independently enters the
scalable cadence above. An older node returning later immediately returns
v0.1.64 receivers to compatibility cadence. Legacy decoding therefore remains
without making the steady 24-node fleet pay the old multicast rate.

## Next extraction order

1. Render-plan orchestration and progress-based watchdog supervision.
2. Separate command side effects from the main render loop.
