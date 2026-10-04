# Thread LED Node development plan

This plan moves the project from a successful two-node field prototype to a
recoverable, reproducible fleet. Work is ordered by dependency and operational
risk: boot and update safety first, then reproducibility, automated testing,
architecture, fleet operations, security, and documentation.

## Working rules

- Never deploy an image that has not passed the build, size, and artifact
  checks.
- Roll out to one canary ring before the rest of the fleet.
- Preserve USB/UF2 recovery until an alternative has been proven on hardware.
- Treat generated binaries as release artifacts, not source files.
- Record every hardware test as structured output with the firmware version,
  image hash, device ID, reset cause, and timestamps.
- Do not rotate fleet credentials, rewrite Git history, or reprovision a
  bootloader without a rollback plan and an explicit maintenance window.

## Phase 0 — immediate safety fixes

Status: complete

Deliverables:

- Make OTA fail closed when upload, hash discovery, arming, reboot, or final
  verification fails.
- Confirm a test image only after border-router infrastructure is visible, not
  merely after joining any Thread partition.
- Bound all formatted telemetry before passing lengths to sockets.
- Add an automated flash/RAM budget check and establish an initial fleet flash
  ceiling of 92%.
- Maintain separate debug and fleet configurations; the fleet image keeps
  remote diagnostics and recovery but omits interactive shells and logging.
- Add regression checks for these behaviors that do not require hardware.

Exit criteria:

- A failed upload can never proceed to `update` or reset.
- A private ring-only Thread partition cannot confirm a test image.
- No formatted packet can read past its backing buffer.
- The fleet profile passes the 92% FLASH and 80% RAM gates with enough margin
  for boot instrumentation and maintenance work.

## Phase 1 — boot-chain diagnosis and recovery gate

Status: hardware exit gate passed; persistent breadcrumbs and direct UF2
handoff hardening completed on 2026-07-20

Progress (2026-07-19):

- Added `pi-tools/boot_test.py`, an identity-gated, observe-by-default harness
  that records dashboard state, direct status, USB/serial transitions, kernel
  logs, reset telemetry, and bounded recovery results as JSON/JSONL.
- Reproduced the failure with Opal only. The Pi saw USB reconnect about 1.7 s
  after reset, then every descriptor request timed out (`-110`) before the
  application entry marker. A simple pre-reset USB detach in v0.1.9 did not
  change the failure, ruling out a missed host detach as the primary cause.
- Fixed the pre-application deadlock in v0.1.10 by disabling Zephyr's
  POST_KERNEL USB auto-init, arming the watchdog immediately after the LED
  entry marker, and explicitly enabling USB from `main()`.
- Opal is active and MCUboot-confirmed on v0.1.10. Seven consecutive warm
  reboots recovered, with USB enumeration at roughly 3.0–3.2 s and full
  infrastructure recovery between 3.5 and 19.8 s. The passing kernel captures
  contain no USB descriptor timeouts.
- Opal then passed 43 additional resets (50 total); the new set recovered in
  3.45–16.75 s with USB at 2.98–3.17 s and no watchdog resets or descriptor
  timeouts. Feral was hash-verified, updated, and confirmed on the same
  v0.1.10 image, then passed a clean 50-reset run in 3.51–9.11 s.
- An initial Feral run stopped when the Pi lost bidirectional control to both
  rings while the RCP still reached them. Kernel evidence showed `wpan0`
  leaving/re-entering all-multicast mode; restarting OTBR restored the host
  path. The harness now uses the other ring as a sentinel and classifies a
  simultaneous outage as infrastructure rather than a target boot failure.
- A test-only v0.1.11 image deliberately suppressed confirmation and rebooted
  after eight seconds. Five consecutive Opal trials reverted to exact,
  confirmed v0.1.10 in 119.86–120.63 s with Feral continuously healthy.
- Five consecutive native UF2 recovery flashes passed. The bootloader appeared
  in 3.11–3.36 s, the 840 KiB combined image copied in 2.11–2.14 s, and exact
  v0.1.10 was active/confirmed afterward. Three returned directly in
  1.98–2.32 s; two needed the early watchdog and returned in 22.42–24.56 s.
  This bounded watchdog-assisted class is recorded explicitly and leaves a
  direct bootloader-to-application USBD handoff hardening item open.

Deliverables:

- Define retained boot-stage breadcrumbs for UF2 handoff, MCUboot entry, image
  validation, application entry, Thread attachment, and infrastructure reach.
- Add a minimal MCUboot-visible LED/GPIO marker where size permits.
- Build a Pi harness for power-on, pin reset, software reboot, watchdog reset,
  OTA test boot, confirmation, rollback, and UF2 recovery.
- Capture USB enumeration, serial, heartbeat, camera/LED state, reset flags,
  active slot, image version, and image hash in a run directory.
- Diagnose and fix the repeatable roughly 60-second pre-application blackout.

Exit criteria:

- Both current boards complete 50 consecutive software resets without an
  unexplained blackout.
- Every tested reset can be assigned to a boot stage and reset cause.
- OTA rollback and UF2 recovery each pass five consecutive forced-failure runs.

Result (2026-07-19): all three hardware exit criteria pass on v0.1.10. Detailed
evidence is stored on the Pi under
`/home/pi/boot-tests/v0110-opal-soak-remaining-043`,
`/home/pi/boot-tests/v0110-feral-soak-050-sentinel`,
`/home/pi/boot-tests/v0111-opal-rollback-final2-005`, and
`/home/pi/boot-tests/v0110-opal-uf2-recovery-final2-005`.

Follow-ups carried into Phase 2/6:

- Persist a multi-stage boot event ring rather than relying only on reset flags,
  retained reboot reason, LEDs, host USB transitions, and slot state.
- Reset/sanitize the nRF USBD peripheral before `usb_enable()` when the
  Adafruit bootloader jumps directly into the application, then require five
  direct UF2 returns without watchdog assistance.
- Add Pi-controlled pin-reset hardware if physical reset-pin testing is a fleet
  requirement; the current Pi can switch shared USB VBUS but has no dedicated
  reset connection to either ring.

Follow-up result (2026-07-20): the firmware persists an eight-event boot trail
and exposes it over UDP. The breadcrumbs isolated intermittent first-boot
failures to Zephyr's unbounded nRF52840 USBD READY wait. An initial v0.1.15
patch bounded and power-cycle-retried each attempt and passed five direct
handoffs, but a later run proved its busy-wait retry could still starve the
cooperative USB work queue. v0.1.16 uses a yielding bounded backoff and removes
the known-bad `otThreadBecomeDetached()` singleton rescan, which could deadlock
until the watchdog reset the other ring. Opal passed the initial transition
plus five consecutive direct UF2 handoffs; Feral then updated transactionally.
Evidence is in `/home/pi/boot-tests/v0116-yield-*` and
`/home/pi/deployments/v0116-*`.

## Phase 2 — transactional deployment tooling

Status: hardware exit gate passed

Result (2026-07-20):

- Added `pi-tools/deploy.py`, a standard-library transaction state machine,
  and `pi-tools/smp_transport.py`, a typed JSON boundary around `smpclient`.
  The shell OTA entry point is now only a compatibility wrapper.
- Added a machine-readable two-ring inventory, exact MCUboot header/TLV/hash
  validation, version policy, per-device locks, bounded attempts and timeouts,
  JSONL evidence, idempotent already-current handling, and canary-first fleet
  rollout that stops on the first non-success.
- Host tests cover upload failure, stale slot, delayed state visibility,
  reboot loss, rollback, confirmation timeout, and idempotent deployment.
- Five deliberately interrupted Opal uploads (2–6 second process cutoffs)
  all returned `upload_failed`, never armed, and left exact confirmed v0.1.10
  active. Evidence is under `/home/pi/deployments/v0112-interrupted-001`
  through `-005`.
- Five consecutive Opal transactions using monotonically increasing signed
  test builds v0.1.12–v0.1.16 all observed departure/return and exact active,
  confirmed hashes. Evidence is under
  `/home/pi/deployments/v0112-canary-success-001` through `-005`.
- Opal was returned to the production v0.1.12 artifact. A real fleet command
  then classified Opal `already_current`, continued to Feral, and upgraded
  Feral from v0.1.10 to the same exact confirmed v0.1.12 hash. Evidence is in
  `/home/pi/deployments/v0112-fleet-rollout`.

Deliverables:

- Replace `ota.sh`, duplicated stress logic, and human-formatted output parsing
  with one Python deployment library and CLI.
- Preflight the destination, signed image metadata, version policy, slot state,
  and reachability.
- Upload and verify the exact expected hash, arm that hash, observe the device
  leave and return, and verify active plus confirmed state.
- Use per-device locks, bounded retries, explicit timeouts, nonzero ambiguous
  exits, and JSONL event logs.
- Support canary rollout and stop-on-first-failure fleet rollout.

Exit criteria:

- Unit tests cover upload failure, stale slot, delayed state update, reboot
  loss, rollback, and confirmation timeout.
- Five interrupted uploads and five successful canary upgrades produce the
  expected classifications without manual intervention.

## Phase 3 — reproducible builds and releases

Status: complete. On 2026-07-20, clean tag `v0.1.16` produced two pristine
builds with identical application and bootloader content, independently
verified RSA-PSS signatures, and a complete manifest/provenance bundle. The
exact packaged UF2 and OTA binary were then validated on Opal and Feral, and a
final fleet pass found both devices active and confirmed on the release hash.

Deliverables:

- Pin Zephyr and module revisions in a west manifest and document the supported
  SDK, CMake, and Python dependencies.
- Provide single commands for debug, fleet OTA, and combined provisioning UF2
  builds.
- Generate versions from release tags plus commit metadata rather than editing
  `prj.conf`.
- Publish signed binaries, combined UF2, SHA-256 manifest, size reports, map
  files, configuration, and provenance for each release.
- Add CI for clean builds, formatting/static analysis, tests, partition-map
  consistency, and size budgets.

Exit criteria:

- A clean checkout reproduces identical application/bootloader content using
  the documented environment; signed outputs may differ only in RSA-PSS's
  intentional random signature salt, and every signature is independently
  verified against the pinned key.
- The release process refuses dirty, unversioned, oversized, or unsigned fleet
  images.

## Phase 4 — repository hygiene

Status: safe tranche complete on 2026-07-20; optional destructive history
rewrite deferred pending explicit coordination

Result:

- Removed 2,865 generated Zephyr build and Python cache files from Git
  tracking while preserving developers' local outputs.
- Added an automated repository-hygiene gate so ignored build/cache paths
  cannot silently re-enter the index.
- Added EditorConfig, the Zephyr C formatting profile, Ruff lint/format rules,
  and local pre-commit checks shared by GitLab and GitHub host CI.
- Kept all signed firmware in GitLab release/job artifacts; no `dist/` files
  remain tracked.
- Deferred any `git-filter-repo` rewrite because it would invalidate existing
  clones and requires a coordinated maintenance window.

Deliverables:

- Stop tracking all Zephyr build directories and add complete ignore rules.
- Move durable binaries to release artifacts.
- Add editor, formatting, lint, and pre-commit configuration.
- Decide whether the clone-size benefit justifies a coordinated
  `git-filter-repo` history rewrite; preserve a backup and migration notes if
  performed.

Exit criteria:

- Normal builds do not change `git status`.
- Source reviews contain no generated binary, object, map, or cache churn.

## Phase 5 — testable firmware architecture

Status: in progress. The beacon wire codec and explicit application-state
policy are now isolated and exercised as pure C from host tests. Rolling
beacon compatibility with v0.1.17 remains enabled. See
`docs/firmware-architecture.md`.

Deliverables:

- Split `main.c` into boot status, application state, mesh health, OTA
  lifecycle, control protocol, telemetry, and rendering orchestration modules.
- Define explicit BOOTING, SEARCHING, ISOLATED, HEALTHY, DFU, and RECOVERY
  states with owned transitions.
- Replace unconditional main-loop watchdog feeding with a health supervisor
  that checks render, command, gossip receive, Thread progress, and
  infrastructure liveness.
- Move tuning values into shared configuration consumed by firmware and tests.
- Encode beacons explicitly with a version, length, defined byte order, fixed
  numeric representation, and compatibility tests.

Exit criteria:

- Pure genome, group, clock, protocol, and state-transition logic runs in host
  tests without hardware.
- Module ownership and lock ordering are documented and checked in review.

## Phase 6 — testing ladder and observability

Status: in progress. Clock-freshness telemetry, a camera-independent Pi soak,
pure rendering/group-policy/clock-estimator tests, and strict typed UDP control
parsing with bounded telemetry are implemented. Native Zephyr simulation now
covers persistent boot evidence and the OTA confirmation/rollback lifecycle.
An indexed, retention-bounded Pi scheduler now packages strict boot,
freshness, and one-minute rotation observation jobs.

Deliverables:

- Host tests for deterministic rendering, membership hysteresis, consensus,
  estimator traces, packet parsing, telemetry bounds, and command validation.
- Zephyr native simulation tests where hardware drivers are not required.
- Pi hardware-in-loop suites for boot, OTA, partition/rejoin, group convergence,
  one-minute synchronized rotation, and long-duration clock behavior.
- Persistent boot counter and a small reset/boot-stage event ring on each node.
- Structured Pi event storage for state transitions, resets, updates,
  partitions, and timing metrics.

Exit criteria:

- Pull requests run host and build checks; nightly jobs run hardware/camera
  tests and retain artifacts with explicit thresholds.
- Field failures can be reconstructed without relying on a chronological prose
  note alone.

## Phase 7 — fleet configuration and security

Status: in progress; project firmware/release signing and authenticated,
replay-safe control are implemented. Opal, Feral, and Lucid have completed
control-key hardware qualification; key-backup custody, Thread credential
handling, fuller inventory metadata, and Pi service packaging remain.

Deliverables:

- Replace the MCUboot development signing key with a protected project key and
  document custody, backup, rotation, and recovery.
- Move Thread credentials into environment-specific provisioning inputs and
  rotate committed credentials if they are active.
- Version the control protocol and require exact parsing, request IDs,
  idempotency, and authentication for reboot/update/configuration commands.
- Maintain a machine-readable inventory mapping logical name, FICR ID, USB
  serial/by-id path, Thread address, hardware revision, installed bootloader,
  and physical location.
- Package Pi services, configuration, health checks, and deployment as code.

Exit criteria:

- Repository access alone cannot authorize a fleet image or destructive ring
  command.
- USB flashing resolves an explicit inventory device and never selects the
  first `/dev/ttyACM*` entry.

## Phase 8 — hardware contract and durable documentation

Status: in progress; P0.17 `EN_LED` control is implemented and qualified on a
carrier PCB. Battery scope and the remaining documentation consolidation are
open.

Deliverables:

- Implement and verify P0.17 `EN_LED` power control and safe data behavior when
  the strip rail is off.
- Add battery measurement/calibration if battery operation remains in scope.
- Eliminate or automatically compare the duplicated application/MCUboot
  partition definitions.
- Replace stale README firmware notes with a developer quickstart and current
  architecture overview.
- Create runbooks for provisioning, OTA, boot recovery, key recovery, and Pi
  replacement, plus ADRs for the boot chain, flash layout, multicast protocol,
  grouping, and update confirmation policy.

Exit criteria:

- Firmware behavior matches the carrier-board power contract.
- A new maintainer can build, provision, update, diagnose, and recover a ring
  using versioned repository documentation.

## Release gates

Every fleet release must pass, in order:

1. Clean, pinned, reproducible build.
2. Host tests, static checks, partition check, and flash/RAM budget.
3. Signed-artifact metadata and hash verification.
4. Boot/reset matrix on the hardware canary.
5. OTA success and forced-rollback test on the canary.
6. Camera-backed group/synchronization smoke test.
7. Staged rollout with automatic halt on ambiguity or regression.

## Current execution queue

Progress 2026-08-15 (local session): `drifting-owl-spark` was recovered over
physical DFU, forensically snapshotted first, and is active/confirmed on
v0.1.69; queue item 1 is otherwise closed because `glowing-fern-breeze` was
intentionally powered off by the operator (not a wedge — audit it when next
powered). The wedge root cause and the resulting boot-recovery ladder
(escape to the native UF2 bootloader after repeated failed boots; watchdog
coverage of MCUboot and early application init) are implemented in source —
see `docs/boot-recovery-ladder.md` and the
`ladder=` field in the boot reply. Bench qualification ran the same night on
`drifting-owl-spark` (operator-approved): the hang image self-presented as a
UF2 drive in 51 s and was recovered by file copy. The first bench run failed
and exposed the native-bootloader top-of-SRAM stack trampling retained state;
the ladder moved to `0x2003d000` before the tag.

v0.1.70 was then released (strict attested `scripts/release.py` build,
GitLab release with artifacts, signed tag) and rolled out on 2026-08-16
(UTC): `drifting-owl-spark` via the release combined UF2 over USB (carries
the escape-ladder MCUboot), `humming-lantern-leaf` as OTA canary, then
`gentle-spark-light`, `howling-spark-pulse`, `molten-dune-glow`
transactionally over Thread. All five are active, confirmed, healthy on
partition `22ddd248` with `quiet off` issued; snapshot sha256
`6d691f57fb20…` under
`/home/pi/deployments/20260815-v0170-boot-recovery-ladder/`.
`glowing-wisp-frost` went offline mid-pass and was safely declined
(`state_unreachable`); it and the other powered-off targets take v0.1.70 as
they come online. OTA-updated boards still run the previous MCUboot — the
escape ladder arms per board at its next USB quick-update/recovery flash.

Two operational notes from the pass: the Pi `/opt/thread-led-node` checkout
is stale (bf2c6fd) with hand-updated inventory, and its `pi-tools`
worktree carries UNCOMMITTED client improvements (`mode`/`quiet` support,
71-byte datagrams) that exist only on the Pi — reconcile them into git
before the next tooling change. The repo inventory still flags powered-off
`opal-reef-tide` as the sole canary, which blocks `deploy.py fleet`;
per-device deploys were used instead.

The next work (as of 2026-08-16) is:

1. Recover `glowing-fern-breeze` and `drifting-owl-spark` over exact-identity
   USB, determine their actual active/rollback images, and bring them to the
   current signed release without guessing from their last dashboard record.
2. Update and version-audit the remaining inventoried targets as they are
   powered. Keep `config/rings.json` aligned with GitLab after every
   provisioning event, and maintain the control keys in a separate secure
   off-Pi backup rather than committing them.
3. Run a longer nine-node freshness/group soak and a camera-backed visual sync
   check on v0.1.69, then exercise staged live populations approaching the
   simulated 24-node cap. Preserve the acceptable one-to-two-minute convergence
   window rather than retuning for instant joins.
4. Validate the Rev B source-mux/battery design physically, including actual
   battery polarity, charge termination/current, USB/battery reverse current,
   System OFF current, rail sequencing, and thermal behavior. Keep `rope100`
   outside the carrier/module-VCC power budget.
5. Run the full attested `scripts/release.py` path with an SSH-signed source tag
   at the next deliberate release checkpoint. v0.1.69 has signed firmware
   artifacts, clean-source build/test evidence, and a GitLab-created tag, but
   is not represented as an attested source-tag release.
6. Continue the existing non-blocking backlog: richer Pi replacement/service
   packaging, signing-key backup custody, optional wider camera coverage, and
   the deferred coordinated history rewrite.

## MCUboot validation watchdog diagnosis — 2026-07-23

- Commit `2e7a77a` added retained-register MCUboot phase breadcrumbs without
  flash writes or logging. Raw devboard Opal, serial `DEC0DE0000000022`,
  passed four forward swaps (v0.1.35 through v0.1.39) plus a v0.1.40
  upgrade/revert cycle. Every recovered application reported
  `mcuboot=00/00`; those six directions did not reproduce the carrier failure.
- Commit `80b5014` additionally fed the inherited watchdog at every traced
  validation, swap, and handoff boundary. Its exact v0.1.41 provisioning
  image was installed and confirmed on Opal. The project-signed v0.1.42
  candidate then uploaded and hash-verified exactly, armed successfully, left
  the network, and ended as an ambiguous `reboot_loss`.
- Before recovery, Opal's Adafruit bootloader exposed a read-only
  `CURRENT.UF2` snapshot with SHA-256
  `3d2b1ebc964e76d7c5b7bd3095d860de30fdb67eff871524c7a1aed84a1ce203`.
  Its slot-0 bytes exactly match v0.1.41 and its captured slot-1 prefix exactly
  matches v0.1.42. MCUboot had not begun moving either image, localizing the
  failure to candidate validation rather than sector erase/copy/status work.
- MCUboot's image-validation implementation hashed the entire candidate in a
  chunk loop without feeding the inherited watchdog, then entered RSA
  verification. Commit `adbc228` adds a feed after every hash chunk and
  immediately before signature verification while retaining the phase feeds
  and frozen breadcrumb. This closes the evidence-backed watchdog gap without
  lengthening the application's 15-second health deadline.
- Recovery exposed a second independent defect: the original combined UF2
  erased only slot 0, allowing an interrupted slot-1 candidate to remain
  bootable. The initial attempt to clear all of slot 1 exposed an additional
  compatibility boundary: Opal's nice!nano v0.6.0 bootloader accepts
  application writes only below `0xAD000`. It wrote the accepted prefix of the
  `0x26000..0xEC000` image but rejected later blocks and never auto-reset.
  Readback proved MCUboot and slot 0 landed byte-for-byte while writes at and
  above the boundary did not.
- Recovery now erases exactly `0x26000..0x90000`: MCUboot, slot 0, and the
  first 4 KiB page of slot 1. This invalidates any interrupted secondary image
  header, stays within the native bootloader limit, preserves settings at
  `0xEC000`, and lets the recovered application return so SMP can erase the
  remainder. Regression tests tie this range to the real partition map,
  release verifier, and native bootloader ceiling.
- Commit `41ee875` encodes the native bootloader ceiling in the release check
  and corrected recovery artifact. Opal auto-reset from the exact v0.1.44
  combined UF2, attached on boot 73 with `reset=uf2`, confirmed the expected
  active image, and reported `mcuboot=00/00`. The combined-UF2 SHA-256 is
  `b5721b73b18b94b4da4d47a6c9701583b3fd2ec797102eab9ab1cf91657ab6da`,
  signed-bin SHA-256 is
  `9fbb28d0b21c08596669988994d5a3cd86a8359b4509a2f14998d7fcaa9e034d`,
  MCUboot image hash is
  `EC485A186E3CC4DBDBF74122852E9C76620677B4D70A74FECEBCA48F086DFA9B`,
  and MCUboot-hex SHA-256 is
  `b705cd27829a6e7eb8f5ccfa332aa066048c3335dda7fd935ddaaa03e7b295ef`.
- The raw-board qualification gate is complete. Project-signed v0.1.45
  (file SHA-256
  `170e5ec5884e9a13fd941ea19a7001e11e3f30013a15e7e50f1166468e0075d0`,
  image hash
  `DF9173DD22D8C06BE3B1BF47C2E7B02623FFAC5E05218650A0F463573A28A2C3`)
  uploaded and verified exactly, became active, and confirmed on boot 74 with
  `reset=ota` and `mcuboot=00/00`. Project-signed rollback v0.1.46
  (file SHA-256
  `81e052c8fe5f4dcc5bd52d65735e9722ba4adf6120efb48a84fcfc99ed7327cd`,
  image hash
  `A220BE80AD2C2F1B2518E3219377D2DFE48120F5BACC82C9A26E94CD67D198A7`)
  was observed active and unconfirmed on boot 75, then reverted to confirmed
  v0.1.45 on boot 76 with `reset=rollback` and `mcuboot=00/00`. Slot 1 was
  erased after the test.
- Durable transaction records, signed artifacts, and the interrupted flash
  snapshot are under
  `/home/pi/deployments/20260723-mcuboot-watchdog-trace/`, including
  `v0144-native-boundary-recovery/`, `v0145-validation-feed-deploy/`, and
  `v0146-forced-rollback-deploy/`. Failed preflight attempts are retained
  alongside successful runs rather than removed.

## USB host-power diagnosis and v0.1.34 canary — 2026-07-23

- The v0.1.33 render-lock canary and staged rollout put Opal, Feral, and Lucid
  on the exact confirmed fleet image. Glowing's exact candidate upload then
  disappeared during reboot, so the transactional rollout stopped before
  Dusty. Dusty had independently stopped emitting heartbeats about fifteen
  minutes earlier and had received no mutation.
- Both rings recovered confirmed v0.1.32 after physical power changes. Their
  normal boot trails and the shared correlation with computer USB power
  identified a separate host-power/startup boundary rather than corrupt
  application images.
- Live descriptor inspection found Zephyr's defaults on the carrier:
  self-powered with `MaxPower 100mA`. The hardware is bus-powered and drives
  the 16-pixel rail from USB VBUS. Commit `8d6741e` sets
  `CONFIG_USB_SELF_POWERED=n` and `CONFIG_USB_MAX_POWER=250` for thread-ring
  and ring-test, adds a regression test, and documents the 500 mA computer-port
  boundary. The complete local gate passed all host/native tests and all four
  signed firmware profiles.
- A project-signed v0.1.34 recovery UF2 on Glowing enumerated immediately with
  `bmAttributes 0x80` and `MaxPower 500mA`, joined Thread, and confirmed. Two
  transactional attempts to replace that recovery profile with the exact
  v0.1.34 fleet candidate both watchdog-reset during MCUboot swap and safely
  rolled back. Stable USB enumeration during both attempts proves that the
  swap-watchdog defect is independent of the corrected descriptor.
- Installing the exact v0.1.34 fleet payload through a trailer-clearing native
  UF2 did not start while Glowing remained connected to the computer. The same
  already-written image booted normally after moving to a dedicated supply:
  boot sequence 10, reset `00000000/none`, healthy infrastructure, active and
  confirmed MCUboot hash
  `FCFB6EB11345A27B2168285F46BA55E48BCB93E2C198FBB1F3A3BF981C9F8FE0`.
  This proves a computer-attached boot/handoff trigger remains in addition to
  the independent MCUboot swap defect.
- Durable build and transaction evidence is under
  `/home/pi/deployments/20260723-v0134-usb-power/`. The fleet artifact file
  SHA-256 is
  `e802e649f509222a001f97dbb86275b841012b6b084c3962645bfbe6c7456ae4`;
  the fleet recovery UF2 SHA-256 is
  `ce6547b3f52ccf4bc7b37d87e6ac240facd3af923de8958e4cf5f281c6a6f003`.

## RSSI-independent direct-visibility grouping — 2026-07-23

- Production proximity now means receipt of a fresh `ff02::1` link-local
  beacon copy. That packet cannot cross a radio hop, so it is stronger evidence
  of physical contact than mesh-wide gossip and broader than OpenThread's
  protocol-neighbour table.
- RSSI no longer participates in join, leave, candidate weight, or approach
  rendering. Any directly heard packet is eligible. RSSI remains best-effort
  telemetry when OpenThread has a matching neighbour record.
- Ten seconds of sustained direct visibility joins a peer. Five seconds of
  fresh gossip without direct visibility removes it, while a completely silent
  member retains the existing fifteen-second obstruction grace. Damped
  reciprocal topology still prevents asymmetric nodes from rendering
  overlapping groups, and mesh-wide gossip alone cannot create a proximity
  edge.
- Removed the authenticated and USB `thresh` controls entirely. The mobility
  harness is now a read-only operator-guided physical observer, so fleet
  firmware has no hidden test grouping mutation.
- Compact group telemetry reports the peer count and, as space allows,
  membership/direct/fresh/age/RSSI details without overflowing the bounded
  control reply as the inventory grows.
- Rotation and freshness observers now default to all five inventoried rings.
  Host traces cover interrupted contact, weak-link-independent membership,
  fresh-but-not-direct departure, silent obstruction grace, return, exhaustive
  directed three-node asymmetry, and malformed/incomplete topology.
- The complete local gate passed pre-commit, 125 host tests, three native
  Zephyr boot/OTA tests, workspace and partition checks, and fleet,
  provisioning, rollback, and partition-HIL builds. Fleet usage is 79.02
  percent FLASH and 41.61 percent RAM; partition-HIL remains below its budget
  at 85.87 percent FLASH and 43.45 percent RAM.

## Mobility and vivid-scene hardening — 2026-07-23

- Decoupled group peer presence from the shorter sync-table lifetime. Missing
  peers now complete the fifteen-second departure policy even after their
  clock record expires, and retained records are released only after damped
  agreement also removes them.
- Replaced independent bitmap-majority decisions with a deterministic
  reciprocal-link graph component. Incomplete, duplicate, unknown, and
  malformed topology snapshots fail closed; no directed three-node proximity
  graph can produce overlapping lit groups.
- A scene locks only when the complete topology target exactly matches damped
  agreement and timing exists for every member. Topology changes invalidate
  the lock immediately, while bounded render visibility fades avoid hard
  visual cuts.
- Added `group_mobility_test.py`; its current automated path uses authenticated
  realm-only gossip transmit on one wanderer while leaving Thread, clock
  receive, OTA, and management traffic online. The live three-ring run passed
  crowd → Opal solo plus Feral/Lucid
  duo → restored crowd in 28.4 seconds, with unchanged boot sequences and
  verified restoration to `join=-72 leave=-82`.
- Removed RGB interpolation from automatic palette and minute transitions.
  Hue-aware interpolation plus a final 78-percent HSV saturation floor now
  covers both scene and group membership crossfades. Sampled host tests span
  all tiers, many seeds, every LED, and multiple minute boundaries.
- The complete clean local gate passed pre-commit, 121 host tests, native
  Zephyr simulation, workspace/partition checks, and all four signed firmware
  profiles. Fleet flash is 79.12 percent and RAM is 41.57 percent.
- The project-signed v0.1.30 fleet image built from source commit `57db6e1`
  has file SHA-256
  `bf642f58bd5cf4714e4c411bf01aa6026647a671044c8dedeec41fe626ae0aa9`
  and MCUboot image hash
  `1362660F4C38AF8A391C7B8E37A28FD144BBD622EB4E1BF08EC2327464AAB251`.
  Opal passed canary qualification and Feral updated normally. Lucid's first
  arm watchdog-reset before the application entry breadcrumb and safely
  rolled back to confirmed v0.1.29; an idempotent re-arm of the already
  verified candidate then returned normally and confirmed v0.1.30.
- All three rings now report the exact v0.1.30 image, healthy infrastructure,
  control authentication, topology-complete three-member agreement, and
  unchanged production thresholds. The explicit three-ring 135-second
  rotation run passed 270 cycles with two changes 60.59 seconds apart, one
  missing management reply, one half-second transition mismatch, and no boot
  counter changes. Fixed-exposure frames show synchronized saturated
  green/amber output without pastel lift.
- Deployment, rollback, mobility, rotation, and camera evidence is under
  `/home/pi/deployments/20260723-v0130-mobility-vivid/`.
- Corrected `rotation_test.py` so its default fleet list includes Lucid. The
  old two-ring default had allowed a manually named `rotation-3ring` evidence
  directory to omit the third ring unless `--rings` was explicit. The
  camera-independent freshness soak now defaults to the same complete fleet;
  the camera harness retains an explicit two-ring default because blob count
  is part of that physical test setup.
- A later physical Lucid departure forced the remaining rings and OTBR from
  Thread partition `5afdb0ae` to `3576cfbc`. Opal and Feral correctly
  converged to the same topology-complete duo with unchanged boot counters,
  but all host-originated management replies initially failed while
  ring-originated multicast heartbeats continued.
- OTBR-internal pings proved zero-loss reachability. Linux had selected a
  retained old `wpan0` Thread locator (`...:fc38`) as the outbound source;
  explicitly binding to OTBR's ordinary OMR address restored every reply.
  Added shared stable-source selection to authenticated control, deployment,
  boot/freshness/rotation/mobility/sync tools, and the pinned SMP transport.
  Three live status/group rounds against both remaining rings and a read-only
  exact-image SMP query passed with the stale locator still present, proving
  the fix rather than relying on OTBR restart side effects.

## Social LED scenes — 2026-07-22

- Added deterministic automatic scenes selected by agreed visible member
  count. Solo rings use one-hue breath, halo, or soft-shimmer families; pairs
  use two-hue braid, opposing-arc, or tide families; groups of three or more
  use faster three-to-five-color kaleidoscope, aurora, or prism families.
- The shared global clock selects a different family on every adjacent
  one-minute epoch and crossfades for five seconds. A phase-locked group uses
  its shared seed, so all members remain pixel-identical while rotating.
  Manual `pattern` selection still bypasses the social renderer.
- Added pure-C host coverage for deterministic output, RGB bounds, palette and
  tempo contracts, negative/long-running time, minute-boundary continuity,
  and guaranteed adjacent-minute family changes. The complete local gate
  passed 117 host tests, native simulation, repository/partition checks, and
  all signed firmware profiles.
- Hardware qualification used the existing authenticated `thresh` control to
  exercise solo, pair, and crowd membership without adding a test-only group
  mutation to fleet firmware. All three devices were restored to production
  `join=-72 leave=-82` thresholds afterward.
- A v0.1.28 three-ring soak exposed intermittent visual fallback when measured
  beacon offsets briefly reached 20--70 ms. Group acquisition remains strict
  at three consecutive samples below 6 ms, while visual-lock loss now requires
  five consecutive missing samples or offsets above 80 ms. Host traces cover
  the exact production policy, bounded excursions, sustained loss, and
  reacquisition.
- The project-signed v0.1.29 fleet image built from source commit `0209217`
  has file SHA-256
  `e664f7aa6fc21389a50a80965bbb416c999fd9dce5bedbdf728296918cbe3c7f`
  and MCUboot image hash
  `34B3AED847F9DE8F70A12698A6B9C4DED3677F5A27463A6C65DF81641A69BFC4`.
  Opal was the canary; Feral and Lucid then completed transactional rollout.
  Every device reports the exact image active and confirmed with build
  `v0.1.17-78-g0209217`.
- The final 135-second three-ring rotation run passed all 270 polling cycles:
  two pattern changes exactly 60.000 seconds apart, zero pixel-pattern or
  identity mismatches, zero missing replies, and unchanged boot counters.
  Fixed-exposure camera frames show all three rings in the shared crowd scene.
  Evidence is under
  `/home/pi/deployments/20260723-v0129-social-scenes-final/`.

## Social-scene energy tuning — 2026-07-23

- Tightened pair motion from 4.5–9 seconds to 3.5–7 seconds per cycle so a
  two-ring interaction reads as more active without losing its relaxed,
  coordinated character.
- Tightened the 3+ member tier from 2.5–6 seconds to 1.25–2.75 seconds per cycle.
  This makes crowds visibly energetic across every deterministic family while
  retaining smooth motion, synchronized rendering, and the saturation floor.
- Added a host contract that every generated crowd plan is faster than its
  corresponding pair plan, and every pair plan is faster than solo.
- The complete clean local gate passed pre-commit, 126 host tests, native
  Zephyr simulation, workspace/partition checks, and the fleet, provisioning,
  rollback, and partition-HIL builds. Fleet flash remains 79.12 percent and
  RAM 41.57 percent.
- The project-signed v0.1.31 fleet image built from source commit `3a59a7e`
  has file SHA-256
  `816b66d65409070b0ba3f70d3b1fc9533d690a8082041981a9749d6e7543445e`
  and MCUboot image hash
  `9CB5515699834FFD7472052F0028F64637FEAD74C34EC4492E2A937383FD0F2A`.
- Opal passed an idempotent canary update and Feral completed normal rollout.
  Both report v0.1.31 active, confirmed, non-pending, authenticated, and
  topology-complete in the same duo on partition `3576cfbc`.
- A 70-second explicit Opal/Feral rotation run passed all 140 cycles with zero
  missing replies, identity mismatches, scene mismatches, or boot-counter
  changes, and one synchronized pattern transition. Fixed-exposure camera
  samples show saturated magenta/violet output and clearly changing arc
  positions without pastel or white lift. The requested 30 fps MJPEG capture
  yielded 241 frames at the Pi's practical 25 fps.
- The exact artifact, transactional deployment records, rotation evidence,
  MJPEG capture, and contact sheet are under
  `/home/pi/deployments/20260723-v0131-social-energy/`.
- Lucid came back online after the initial duo qualification. Its first SMP
  upload attempt timed out safely before arming; the bounded second attempt
  uploaded and hash-verified the exact candidate, then completed a normal
  39.65-second swap. Lucid reports v0.1.31 active, confirmed, non-pending,
  healthy, authenticated, and boot sequence 32 with no watchdog or rollback
  signature.
- The final strict three-ring crowd run passed all 270 half-second cycles over
  135 seconds with zero missing samples or identity mismatches, two
  synchronized transitions 60.5 seconds apart, one permitted transition-cycle
  mismatch, and unchanged boot counters on every ring.
- Fixed-exposure 4:3 and 16:9 samples show energetic, saturated cyan/magenta
  crowd motion without pastel or white lift. The current camera framing
  contains only two of the three physical rings, so full three-object visual
  coverage remains optional follow-up; simultaneous live telemetry proves all
  three were topology-complete in crowd tier.

## Glowing Bloom Wave onboarding — 2026-07-23

- Installed the qualified project-signed v0.1.26 provisioning recovery image
  on local USB serial `DEC0DE0000000004`. The board derived identity
  `glowing-bloom-wave`, joined production Thread partition `3576cfbc` at
  `fd00:db8:0:1:a004:b004:c004:d004`, and was added at inventory rollout
  index 3 in MR !39.
- Created its private control key root-owned mode 0600 on the Pi. The key
  crossed the local USB boundary only through an encrypted in-memory pipe,
  was never printed or retained in evidence, and passed authenticated
  heartbeat verification with exactly one sequence advance.
- Three transactional Thread OTA uploads stopped safely before arming after
  transport timeouts at 66,815, 74,261, and 85,649 bytes. The device remained
  healthy on confirmed v0.1.26 throughout.
- Generated a v0.1.31 combined recovery UF2 from the already qualified
  project-signed fleet binary and matching MCUboot build. Verification proved
  the complete 295,252-byte signed application was embedded exactly and the
  recovery range was bounded to `0x26000–0x8f000`, preserving settings.
  Direct exact-serial USB recovery succeeded, and transactional readback
  classified v0.1.31 `already_current`, active, confirmed, non-pending,
  healthy, and authentication-ready.
- All four rings converged on topology-complete agreement `00802a00` in crowd
  tier. A 500 ms rotation run had zero scene/identity mismatches but failed
  its strict management-availability limit with 8 of 544 replies absent. The
  one-second retry passed 69 of 70 fully valid cycles with one isolated
  missing reply, one synchronized transition, and unchanged boot counters.
  Evidence is under
  `/home/pi/deployments/20260723-v0131-glowing-bloom-wave/`.

## Execution log — 2026-07-22

Phase 9 MCUboot swap-watchdog tranche:

- Reproduced neither the original v0.1.24 Lucid watchdog rollback nor a
  deterministic board dependency: the exact old bootloader completed one
  provisioning-to-fleet swap on Feral and a retry on Lucid. The original
  failure remains intermittent rather than a proven image or EN fault.
- Rejected an initial narrow downstream v0.1.25 erase-feed backport after its
  first Lucid qualification erased the candidate and returned to provisioning.
  The signed application images both verified correctly and fleet never
  entered, so that experimental bootloader was not shipped.
- Pinned the complete MCUboot v2.2.0 release at
  `2d61c318933819a0f4954fb2a5a957a62c6128ce`. It contains the coordinated
  sector-erase watchdog-feed refactor, while the project configuration makes
  the inherited Nordic WDT feed path explicit. Bootstrap now checks out that
  exact clean revision after Zephyr's imported manifest revision.
- Qualified v0.1.26 on Lucid with three consecutive exact-serial provisioning
  UF2 to authenticated fleet OTA transitions. All three returned with fleet
  hash `0F602E9264328A9EA66F3FAD370D4BFC9313ADB4E359173E7001DAEC6A760E06`
  active and confirmed, reset `00000002/ota`, and no watchdog flag. Feral
  remained the healthy sentinel. Evidence is under
  `/home/pi/deployments/20260722-v0126-mcuboot-v220/evidence/`.
- Installed the same exact v0.1.26 combined UF2 on Feral and returned it to the
  exact fleet hash through authenticated OTA. Its swap completed in 39.7
  seconds, active and confirmed with reset `00000002/ota`, while Lucid remained
  healthy as sentinel.
- Completed the rollout on Opal through exact USB serial
  `DEC0DE0000000022`. Direct recovery replaced its earlier bootloader and
  returned in 7.9 seconds with the exact v0.1.26 provisioning hash active and
  confirmed while Feral remained healthy. The authenticated same-version OTA
  safely resumed after three transport timeouts, verified the exact candidate
  before arming, and returned with the fleet hash active and confirmed,
  reset `00000002/ota`, no watchdog reset flag, and `2/2/2` membership.
  Evidence is under `opal-v220-uf2` and `opal-v220-swap-01` in the tranche
  evidence directory. All three rings now run the qualified MCUboot v2.2.0
  bootloader.
- Fixed two safety/idempotency defects exposed by the run: mixed app plus UF2
  USB enumeration now prefers the bootloader state, and `deploy.py` no longer
  destroys and re-uploads an already verified exact inactive candidate. The
  latter still repeats identity, active-slot, and candidate checks immediately
  before authenticated arm.
- v0.1.26 provisioning combined UF2 SHA-256 is
  `dc1cf48b3606643c63b555f89c12f0cbd6c5d68ed23e74ac28d16fb8dc3a2251`;
  fleet signed binary SHA-256 is
  `0b4466005589ebecdc172290f0f8666cac3fe3446817fd5c958b053daeb6ce20`.

Phase 7 authenticated-control tranche:

- Added a strict v2 HMAC-SHA256 request/response envelope with per-device
  keys, correlated 64-bit sequences, durable replay rejection, and exact-retry
  idempotency. Unprovisioned devices and legacy/v1 mutations fail closed.
- Limited key installation to a project-signed USB provisioning profile.
  Fleet, deployment, reboot, rollback, synchronization, and manual control
  tools use authenticated mutations by default.
- Added an exact-inventory provisioning tool that creates private key files,
  never stores key material in evidence, and proves agreement with an
  authenticated heartbeat. Host and native Zephyr tests cover fixed wire
  vectors, response authentication, persistence, replay, and provisioning
  safety.
- Qualified Opal on v0.1.23 with fail-closed legacy handling, wrong-tag
  rejection, exact-retry idempotency across reboot, a private root-owned key,
  transactional fleet OTA, and removal of the provisioning shell.
- Onboarded the new `lucid-iris-light` carrier as inventory rollout index 2.
  Its v0.1.24 key qualification passed the same wrong-tag, legacy, duplicate,
  and reboot-persistence checks. Evidence is under
  `/home/pi/deployments/20260722-v0124-lucid-iris-light/evidence/`.
- Updated `feral-tide-light` through its exact inventoried USB serial
  `DEC0DE0000000002`. The cable remained stable through provisioning and fleet
  UF2 handoffs; the project-signed v0.1.24 fleet hash
  `148A29C942EF2D05C67CD4F8A6F2444933CF79BCBADBEE9D7DD09309CCF135BE`
  is active and confirmed, its root-owned key is mode 0600, and the fleet USB
  shell is absent. Three authenticated fleet reboot cycles passed with USB and
  status down/up evidence and no Lucid sentinel outage. One intervening
  authentication-status datagram timeout safely refused a reboot before any
  mutation; the retry passed with a longer command timeout.

Phase 8 carrier-power tranche:

- MR !25 added active-high P0.17 `EN_LED` control to `ring-test`, fleet, and
  provisioning builds. Startup establishes the inactive GPIO level before
  enabling the rail; clean reboot renders black and disables it before reset.
- The actual carrier at USB serial `DEC0DE0000000003` passed its EN-aware
  standalone ring test, joined Thread as `lucid-iris-light`, and booted the
  exact active/confirmed v0.1.24 fleet hash
  `148A29C942EF2D05C67CD4F8A6F2444933CF79BCBADBEE9D7DD09309CCF135BE`.
  Three authenticated fleet reboots completed in 3.9--4.3 seconds without
  dropping the Opal sentinel, and the fleet USB shell is absent.
- OTA upload safely resumed after three bounded transport failures and
  verified the exact candidate, but MCUboot watchdog-reset during image swap
  before application entry and reverted to provisioning. Installing the same
  fleet image through exact-serial UF2 booted successfully, isolating the open
  problem to the swap path rather than the fleet profile or EN initialization.

## Execution log — 2026-07-21

Phase 5 runtime-architecture tranche:

- Added a pure six-state application policy with explicit transition history
  and host coverage for ordinary boot, recovery boot, detach/reattach,
  infrastructure-loss grace, isolation, recovery, and DFU priority.
- Firmware telemetry now reports the selected application state. The signed
  fleet build passes at 77.75% FLASH and 41.48% RAM.
- The camera harness now defaults to the supported 30 fps mode, enforces its
  discovery deadline from a non-blocking control path, restores skew/flash
  state after setup failures, and rejects an expected blob-count mismatch.
- SMP diagnostics replace binary response fields with bounded length and
  SHA-256 metadata instead of writing firmware chunks into logs.
- A camera preflight after physical repositioning found only one distinct
  ring blob. The invalid run was stopped, both rings were restored to
  `flash=0`, and the dashboard was restarted; no two-ring result was accepted.
- MR !3 and its post-merge pipeline passed. A clean project-signed v0.1.21
  image from final merged commit `23fdc507` used 77.75% FLASH and 41.48% RAM.
  Its file SHA-256 is
  `765f95a8340d5e0afc24282c8876f9184989c4a0b93eff790cd9c04e4f970a85`
  and its MCUboot image hash is
  `943F805158C30FE527F067885EB18605FB6028871654597FC24DB52C3BA61C8A`.
- The deployment boundary was then hardened in MR !4 so failed helper output
  redacts Python byte literals before persistence. Its MR and post-merge
  pipelines passed. Opal required four bounded upload attempts; the first
  three failures produced only length/SHA-256 metadata in retained evidence.
  Feral uploaded on its first attempt. Both nodes booted the exact v0.1.21
  hash, restored infrastructure and mutual membership, and self-confirmed.
- A mixed v0.1.21/v0.1.20 soak crossed a one-minute pattern boundary with a
  stable shared group view and synchronized rotation. Final all-v0.1.21
  telemetry reported `state=healthy`, infrastructure present, no pending
  image, and the same pattern on both nodes. Durable evidence is under
  `/home/pi/deployments/20260721-v0121-runtime-architecture/evidence/`.
- After a later camera move, the strict two-ring visibility gate again found
  only one blob and safely restored both rings. This is retained as an invalid
  framing preflight, not synchronization evidence; the camera-independent
  Phase 6 freshness soak can proceed while only one ring is visible.

Phase 6 observability tranche:

- Status telemetry separates clock-locked, clock-fresh, and retained peers,
  reports the oldest retained peer age, and includes a cumulative fresh-to-
  stale transition counter. Group diagnostics report raw freshness and age
  beside hysteretic membership, so a grace-retained member is no longer
  indistinguishable from a currently heard member.
- The fresh/stale transition policy is a Zephyr-independent C module with host
  tests for exact-boundary, repeated-observation, recovery, and arrival-
  detected gaps.
- `pi-tools/status_telemetry.py` owns backward-compatible status parsing and
  threshold evaluation. `pi-tools/freshness_soak.py` records JSONL samples and
  a machine-readable result without requiring the camera; missing replies,
  stale samples, and new freshness lapses each have explicit limits.
- MR !5 and both its MR pipeline 2695331942 and post-merge main pipeline
  2695338329 passed. The final project-signed v0.1.22 artifact was built from
  merge commit `ad818ca4`, uses 77.86% FLASH and 41.53% RAM, and embeds build
  `v0.1.17-19-gad818ca`. Its file SHA-256 is
  `d7ea209b30ac11f9f9acf04109ec37cfff7751c4a32099d7cd25f481b8709077`
  and its MCUboot image hash is
  `8C7BBF3BDA11E16898D87B79E79AC791355BC0833140BDE0D0DA14365BA1AAC1`.
- Opal's upload succeeded on its third bounded attempt and Feral's on its
  first. Both booted the exact v0.1.22 hash, restored infrastructure and mutual
  group membership, and self-confirmed. A mixed v0.1.22/v0.1.21 probe crossed
  a one-minute pattern boundary with an unchanged agreed group view.
- The zero-management-loss baseline recorded one missed Opal status reply out
  of 600, while both rings had zero stale samples, zero new firmware freshness
  lapses, and unchanged boot counters. This distinguishes lossy diagnostic UDP
  from gossip-clock health. An independent run with a predeclared maximum of
  three missing replies passed 600/600 on both rings; maximum observed peer
  ages were 505 ms on Opal and 913 ms on Feral, versus the 2.2-second gate.
- The final idempotent fleet audit classified both nodes `already_current`,
  active, confirmed, healthy, mutually locked/fresh/tracked, and on the same
  pattern. Durable evidence is under
  `/home/pi/deployments/20260721-v0122-freshness-observability/evidence/`.
- The existing eight-event persistent boot-stage ring is now collected and
  parsed by `pi-tools/boot_test.py` before each trial and after recovery. Raw
  replies remain in structured evidence even when a future format is not
  recognized, preventing parser drift from erasing the diagnostic record.
- Deterministic rendering now runs in host CI across seed expansion, all base
  families, overlay variants, negative and long-running times, exact one-
  minute rotation boundaries, pixel bounds, repeatability, and crossfade
  continuity.
- Membership join/leave clocks, strict-majority bitmap consensus, and the
  phase-lock gate now live in Zephyr-independent `group_policy.c`. Host traces
  cover interrupted approaches/departures, strong-silent grace, exact
  thresholds, damped additions/removals, ties, missing timing samples, and
  long-running counter saturation. Missing samples can no longer be
  misclassified as a zero-offset lock. The full signed fleet build passed at
  77.94% FLASH and 41.53% RAM with 72 host tests.
- The phase and frequency estimator now lives in Zephyr-independent
  `clock_estimator.c`; `sync.c` retains only socket, peer-table, freshness, and
  locking ownership. Deterministic host replays cover clean convergence,
  packet loss/recovery, phase-window reset after a clock step, +300-ppm local
  skew compensation, and partition/merge convergence. The same module is
  compiled into firmware. The full signed fleet build passed at 77.99% FLASH
  and 41.55% RAM with 73 host tests.
- UDP control now crosses a strict, Zephyr-independent typed parser: exact
  command names and arity, bounded numeric ranges, printable 63-byte datagrams,
  and explicit errors replace prefix matching and status fallthrough. Existing
  exact commands and read-only queries remain compatible, while an optional
  `v1 <request-id>` envelope adds correlated bounded replies for migrating
  clients. Host tests cover every command, malformed and ambiguous input,
  integer overflow, embedded NULs, response correlation, and encoder overflow.
  The full signed fleet build passed at 77.40% FLASH and 41.55% RAM with 74
  host tests; authentication and mutating-command idempotency remain Phase 7.
- OTA confirmation retry timing, the infrastructure gate, rollback deadline,
  and prior-infrastructure observation now live in `ota_policy.c`; `main.c`
  retains MCUboot writes and clean-reboot side effects. A 64-bit Zephyr native
  simulation compiles the production OTA and boot-status modules against
  simulated NVS and verifies persistent reload, stage progression, bounded
  formatting, five-second confirmation retry, and the strict unconfirmed
  deadline without physical peripherals. It runs in the signed-build CI path.
  All 74 host tests and two native ztests pass; the signed fleet build uses
  77.45% FLASH and 41.55% RAM.
- The Pi HIL runner uses explicit inventory expansion, a singleton lock,
  per-job timeouts, atomic run metadata, append-only indexing, and count-based
  retention that preserves three times as many failures as successes. Nightly
  definitions cover strict boot health, qualified freshness, and synchronized
  one-minute rotation; camera is disabled pending framing, and disruptive jobs
  cannot be scheduled. A live observation-only Pi dry run passed against Opal
  and the Feral sentinel. A live 130-second rotation run then passed with two
  changes, a 60.095-second interval, no identity or pattern mismatches, stable
  boot counters, and three permitted missing UDP samples. Evidence is under
  `/home/pi/thread-ring-hil-validation/`. All 81 host tests, two native ztests,
  the hardened systemd-unit verification, and the signed fleet build pass.
- The first production-sandbox nightly invocation passed strict boot and
  synchronized rotation checks. Its freshness probe observed four missing
  Feral management replies out of 600 (0.67%), but zero stale peer samples,
  zero new firmware freshness lapses, and unchanged boot counters. The
  management-reply allowance is therefore calibrated to six per 600-sample
  run (one percent), while stale samples, freshness lapses, and reboots remain
  zero-tolerance failures. Evidence is retained under
  `/home/pi/thread-ring-hil/`.
- A second five-minute run from deployed merge `a5d64a15` passed the calibrated
  profile: Feral missed one of 600 management replies and Opal missed two,
  both reported zero stale samples and zero new lapses, boot counters remained
  14 and 31, and maximum peer age stayed below 513 ms.
- A manual camera-profile run restored both rings after libcamera lost a race
  with the dashboard's six-second snapshot worker; it was retained as an
  infrastructure failure, not framing evidence. The camera profile now uses
  inventory-bound addresses, and a dedicated systemd unit leases the camera
  by stopping the snapshot owner before the run and restarting it from
  `ExecStopPost`.
- The coordinated 30 fps preflight then passed for five minutes with two
  distinct blobs, 301 paired flashes, 3.59 ms median offset, 3.11 ms MAD,
  and -0.93 ms/min drift. The dashboard restarted and both rings returned to
  `flash=0`. The steady camera harness now fails outside explicit limits of
  90 percent paired samples, 10 ms absolute median offset, 65 percent of
  samples inside that offset limit, 5 ms MAD, and 2 ms/min absolute drift;
  the qualified weekly profile is enabled.
- A later manual production run from merge `5bb20469` passed 304 paired
  flashes with 4.42 ms median offset, 3.35 ms MAD, +0.65 ms/min drift, and a
  70.7 percent offset-inlier fraction. The operator chose manual camera
  qualification, so the versioned camera schedule and timer were removed.
- Disruptive HIL now accepts reviewed runtime files whose paths and SHA-256
  identities are recorded in run metadata. Rollback qualification derives all
  target addressing and USB identity from inventory, snapshots and inspects
  the exact signed artifact, stages without arming, re-verifies the confirmed
  baseline and inactive candidate, and only then begins trials. The stale
  hardcoded rollback version was removed; CI builds the fleet-derived signed
  rollback profile.
- A true partition boundary now uses a separate signed `partition-hil` image
  and a 5-300 second self-restoring Thread detach command available only on the
  physical USB shell. The HIL harness requires exact active artifact identity,
  local Opal USB, a sustained target-only outage, healthy Feral sentinel,
  infrastructure/group convergence, and no reboot. Fleet images contain no
  corresponding UDP or shell mutation hook. The profile builds at 84.29
  percent FLASH and 43.37 percent RAM.

Phase 5 beacon-codec canary validation:

- MR pipeline 14 passed its host and fleet-build jobs. The fleet image used
  77.63% FLASH and 41.48% RAM.
- Project-signed v0.1.18 and v0.1.19 canaries were verified locally and again
  on the Pi before transactional OTA. Opal booted the exact candidate hashes,
  restored infrastructure reachability, and self-confirmed both images.
- The v0.1.19 mixed fleet held a shared group view and completed five observed
  one-minute pattern changes with Feral remaining on v0.1.17. A 151-sample,
  five-minute probe found short 2.2-second clock-freshness misses in both
  directions, but no group membership loss or reset; this is now tracked as a
  Phase 6 observability/test-threshold concern rather than a wire-format-only
  effect.
- Migration transmission is biased 3:1 toward legacy v2 frames. This preserves
  packet count and size while leaving useful freshness margin for one lost
  legacy frame during the rolling update.
- Durable Pi evidence is under
  `/home/pi/deployments/20260721-v0119-beacon-codec/evidence/`. Camera attempts
  exposed two harness/infrastructure issues: failure before `try/finally` can
  leave flash mode enabled, and the discovery timeout blocks on `readline()`.
  The final attempt streamed at 30 fps but could not identify two rings because
  neither ring was physically visible in the camera frame. Feral rollout and
  MR readiness remained gated until a valid two-ring camera run.
- After both rings were placed in frame, the supported 30 fps mode found two
  distinct blobs. The clean v0.1.20 canary produced 60 paired flashes with a
  -0.32 ms median offset and 1.61 ms MAD. The 60 fps sensor mode reproducibly
  timed out and remains a Pi camera follow-up, not a firmware release blocker.
- The project-signed v0.1.20 artifact was built from clean source commit
  `cd8fea8`, independently verified on the workstation and Pi, and recorded as
  file SHA-256 `2600f42ae49e57ed2cd0a1031911413c9cadbdd03301ca9079ff77a9d3fe34af`
  with MCUboot image hash
  `07237B1A831A008F0B003421B117F81C68E022F4CBA82A5DDE56EB084799F834`.
  Opal and Feral both booted that exact hash, restored infrastructure and group
  membership, and self-confirmed.
- The final all-v0.1.20 camera run produced 60 paired flashes with a -0.50 ms
  median offset and 1.18 ms MAD. Durable release evidence is under
  `/home/pi/deployments/20260721-v0120-beacon-codec/evidence/`.

## Execution log — 2026-07-19

Completed in the first Phase 0 tranche:

- The current OTA wrapper stops after six failed uploads and cannot invoke the
  arming command in that state; regression tests cover failure and retry.
- Test-image confirmation now requires border-router Network Data and retries
  failed confirmation writes every five seconds.
- Heartbeat formatting is checked before its length reaches `sendto()`.
- The size checker reports FLASH and RAM against explicit fleet budgets.
- `fleet.conf` removes shell/logging weight while retaining OTA, UDP control,
  telemetry, watchdog, and USB/UF2 entry. It builds at 76.90% FLASH and 41.40%
  RAM, versus 98.94% FLASH for the debug image.
- `scripts/build_firmware.sh` makes the board, profile, SDK location, build
  directory, and size gate explicit for clean builds.
