# Documentation index

Start with [`../TIME-SYNC.md`](../TIME-SYNC.md) (the protocol design) and
[`firmware-architecture.md`](firmware-architecture.md) (what the firmware
actually does). The rest is reference.

| Document | What it is |
|---|---|
| [`../TIME-SYNC.md`](../TIME-SYNC.md) | Design notes for the leaderless clock, the pattern genome and group formation, written before and during bring-up. Early sections describe the plan; where firmware later diverged (median instead of mean, no RSSI thresholds), `firmware-architecture.md` is authoritative. |
| [`firmware-architecture.md`](firmware-architecture.md) | Module map, state machine, beacon wire format, group policy, control protocol, lock ordering. |
| [`sync-cohesion-investigation.md`](sync-cohesion-investigation.md) | Why whole crowds fell back to solo, the simulators built to reproduce it, and the fixes each failing scenario demanded. Includes the v0.1.75 rate-trim incident. |
| [`gap-hunt-2026-08.md`](gap-hunt-2026-08.md) | An adversarial campaign against the protocol and a code inspection: confirmed gaps, things that could not be broken, and the withdrawn v0.1.82 release. |
| [`boot-recovery-ladder.md`](boot-recovery-ladder.md) | ADR: how a node that hangs before USB comes up recovers without a button. |
| [`releases.md`](releases.md) | Pinned toolchain, reproducible builds, what the release command refuses. |
| [`security.md`](security.md) | Firmware signing, release attestation, per-device control keys, the USB recovery boundary, and what none of it protects against. |
| [`deployment.md`](deployment.md) | The transactional OTA state machine and its classifications. |
| [`mcuboot-update.md`](mcuboot-update.md) | The guarded physical-USB bootloader update. |
| [`hil-scheduling.md`](hil-scheduling.md) | Hardware-in-the-loop job runner: safety classes, thresholds, retention. |
| [`onboarding-blank-boards.md`](onboarding-blank-boards.md) | Bringing a blank board into the fleet, with the failure signatures met doing it. |
| [`hardware-rev-a-errata.md`](hardware-rev-a-errata.md) | The Rev A board errors and the Rev B requirements. |
| [`development-plan.md`](development-plan.md) | The phased plan and a dated execution log. Long; it is the record of what was qualified when, with numbers. |
| [`look-book.md`](look-book.md) | Named effect presets and their measured periods. |
| [`kicad-notes.md`](kicad-notes.md) | First-hand KiCad, freerouting and JLCPCB lessons from making the board. |

## Reading these as a stranger

- Rings are named by a three-word hash of their device ID; the documents call
  the first few by their first word ("Opal", "Feral", "Lucid").
- "The Pi", "the gateway" and "the border router" are the same machine: a
  Raspberry Pi running OpenThread Border Router, the fleet tools in
  `pi-tools/`, and a small dashboard service that is not part of this
  repository.
- Paths under `/home/pi/` are evidence directories on that gateway. Only the
  2026-07-20 snapshot of them is in `evidence/`.
- "MR !n" refers to merge requests in the private development repository.
- A running handoff file, a parked-project note and a set of one-off rollout
  scripts existed in the development repository and are not included; a few
  sentences that pointed at them were reworded.
