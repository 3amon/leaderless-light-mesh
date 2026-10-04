# Pi hardware-in-loop scheduling

The Pi HIL runner turns the existing one-off diagnostics into bounded,
indexed jobs without giving a timer permission to infer a target or perform a
destructive operation. Job definitions live in `config/hil-jobs.json`; ring
addresses and USB serials are expanded from `config/rings.json`, so dashboard
discovery cannot silently add a device to a test.

## Safety classes

- `observe` reads status or boot evidence and does not alter a ring.
- `diagnostic` may apply a bounded, cleanup-guarded test override such as
  camera flash mode. Diagnostic jobs can be scheduled only when explicitly
  enabled in the versioned configuration.
- `disruptive` includes reboot, partition, rollback, update, and recovery.
  The runner rejects a schedule on these jobs at configuration-load time.
  Manual execution also requires `--allow-disruptive`; the underlying harness
  still enforces exact identity, image, infrastructure, sentinel, and USB
  gates.

The checked-in nightly schedule enables only:

1. A strict ten-second Opal boot observation. It requires the inventory
   address, direct node name, v0.1.22 dashboard image, infrastructure, a parsed
   persistent boot trail ending in `confirmed`, and a healthy Feral sentinel.
2. The qualified five-minute freshness soak with at most six missing
   management replies (one percent of the 600-sample run) and zero stale
   samples, new freshness lapses, or boots.
3. A 130-second rotation probe requiring exact identities, stable boot
   counters, synchronized pattern samples, at least two pattern changes, and
   a 55–65 second interval. One sequential-poll boundary mismatch is allowed.

The camera steady-state definition passed a two-ring framing preflight at 30
fps and is available only as a manual job. It uses
inventory-bound addresses rather than dashboard discovery. The dedicated
camera unit stops the dashboard's periodic snapshot worker before measurement
and starts it again from `ExecStopPost` on success, failure, or timeout. A run
requires at least 90 percent of expected flash pairs, no more than 10 ms
absolute median offset, at least 65 percent of samples inside that offset
limit, 5 ms MAD jitter, and 2 ms/min absolute drift. The
canary reboot definition is disabled and has no schedule. Its stale-safe
expected image must be updated during a qualified release, and it refuses to
run unless Opal's inventory USB serial is present.
Rollback is release-artifact-specific and is available only through the
disabled `rollback-canary` job. Partition/rejoin uses a separate physical-USB
fault boundary and disabled manual job. Neither is delegated to a timer.

## Evidence and retention

Every invocation takes a non-blocking singleton lock and creates:

```text
/home/pi/thread-ring-hil/
  index.jsonl
  runner.lock
  runs/<job>/<UTC-run-id>/
    run.json
    stdout.log
    stderr.log
    evidence/...
```

`run.json` is written before launch and atomically finalized with status,
return code, duration, timeout result, expanded argument vector, Git revision
when available, and SHA-256 identities for the runner, job script,
configuration, and ring inventory. `index.jsonl` is append-only. Retention
keeps the newest 30 passing runs and 90 non-passing runs per job; every removal
is recorded in the index. The retention code rejects broad
roots, ignores symlinks and malformed directories, and only removes validated
children of its own `runs` directory.

## Installation

Install from a reviewed checkout at `/opt/thread-led-node`. Do not copy a
working tree with local changes.

```sh
sudo install -d -o pi -g pi -m 0700 /home/pi/thread-ring-hil
sudo install -d -m 0755 /etc/thread-led-node
sudo install -m 0644 config/hil-jobs.json /etc/thread-led-node/hil-jobs.json
sudo install -m 0644 deploy/systemd/thread-led-hil-nightly.service /etc/systemd/system/
sudo install -m 0644 deploy/systemd/thread-led-hil-nightly.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now thread-led-hil-nightly.timer
```

The camera service is installed separately and has no timer. Start it manually
when both rings are framed and the camera can be leased from the dashboard:

```sh
sudo install -m 0644 deploy/systemd/thread-led-hil-camera.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl start thread-led-hil-camera.service
```

The fixed privileged pre/post commands only lease the camera by stopping and
restarting `fleet-dashboard.service`; the HIL runner and camera process still
execute as `pi` under the unit sandbox.

Before enabling the timer, run the observation job and inspect its metadata:

```sh
python3 pi-tools/hil_runner.py --config config/hil-jobs.json --job boot-observe
jq . /home/pi/thread-ring-hil/runs/boot-observe/*/run.json
```

Manual disruptive execution is intentionally conspicuous:

```sh
python3 pi-tools/hil_runner.py --config config/hil-jobs.json \
  --job boot-reboot-canary --allow-disruptive
```

Never add `--allow-disruptive` to a systemd unit or timer.

### Group split/rejoin qualification

Group mobility is a diagnostic proximity fault, not a Thread partition. Run it
manually from the Pi against every colocated online ring that appears in the
same group. The targets must already run the candidate firmware so their
`group` replies contain committed-scene telemetry:

```sh
python3 pi-tools/group_mobility_test.py \
  --rings opal-reef-tide,feral-tide-light,lucid-iris-light \
  --wanderer opal-reef-tide \
  --outdir /home/pi/deployments/group-mobility-001
```

After a fully converged baseline, the harness sends an authenticated `gossip
realm` to the wanderer. This removes its direct-proximity advertisement while
keeping Thread management reachable. It waits for a singleton plus the
resident group, restores `gossip both`, and waits for the original crowd to
reform. A `finally` cleanup repeats the restore on every exit. The result fails
if a resident loses its group lock or visibility, omits another resident from
its committed scene, reboots, does not exercise the handoff grace, or any
phase misses its deadline. `telemetry.jsonl` retains every half-second sample;
`result.json` records convergence snapshots, committed bitmaps, maximum grace
ticks, boot counters, command acknowledgements, and cleanup outcome.

Use `--manual` to retain the physical move-away/move-back workflow. Do not use
this diagnostic as evidence for a real Thread partition; that qualification
uses the USB-only profile below.

### Artifact-bound rollback qualification

Build a new-version rollback candidate with the project signing key:

```sh
FIRMWARE_VERSION=0.1.23 MCUBOOT_SIGNING_KEY=/secure/key.pem \
  scripts/build_firmware.sh rollback
python3 pi-tools/deploy.py inspect-image \
  firmware/thread-ring/build-rollback/thread-ring/zephyr/zephyr.signed.bin
```

Create a reviewed profile outside the repository. The baseline values must be
the exact active, confirmed canary slot; candidate values come from
`inspect-image`. Relative artifact paths resolve from the profile directory.

```json
{
  "schema": 1,
  "target": "opal-reef-tide",
  "sentinel": "feral-tide-light",
  "baseline": {
    "version": "0.1.22",
    "image_hash": "8C7BBF3BDA11E16898D87B79E79AC791355BC0833140BDE0D0DA14365BA1AAC1"
  },
  "candidate": {
    "artifact": "/home/pi/releases/v0.1.23-rollback/zephyr.signed.bin",
    "file_sha256": "replace-with-64-hex-digits",
    "version": "0.1.23",
    "image_hash": "replace-with-64-hex-digits"
  },
  "trials": 5
}
```

Copy the artifact and profile to the Pi, connect the inventoried Opal USB
serial, and invoke the disabled disruptive job explicitly:

```sh
python3 pi-tools/hil_runner.py --config config/hil-jobs.json \
  --job rollback-canary --allow-disruptive \
  --input rollback_profile=/path/to/reviewed-rollback-profile.json
```

The runner hashes the profile into `run.json`. The harness independently
validates inventory/canary/sentinel identity, exact artifact file and MCUboot
hashes, exact confirmed baseline, local USB presence, and absence of a pending
slot. It snapshots the signed artifact into evidence, stages that snapshot
without arming, re-reads both slots, and only then starts the rollback trials.

### Physical-USB partition/rejoin qualification

A real Thread partition is produced by a canary-only signed image, not by
blocking Pi/OTBR traffic or changing gossip destinations. Build and
transactionally deploy the `partition-hil` profile to Opal with a new image
version. It adds `ring hil-partition <5-300|cancel>` only to the physical USB
shell. Fleet images do not contain this command. The fault automatically
re-enables Thread at its deadline; the watchdog and USB recovery remain
independent backstops.

Create a reviewed profile whose image fields exactly match `deploy.py
inspect-image` output:

```json
{
  "schema": 1,
  "target": "opal-reef-tide",
  "sentinel": "feral-tide-light",
  "image": {
    "artifact": "/home/pi/releases/v0.1.23-partition-hil/zephyr.signed.bin",
    "file_sha256": "replace-with-64-hex-digits",
    "version": "0.1.23",
    "image_hash": "replace-with-64-hex-digits"
  },
  "duration_seconds": 30,
  "rejoin_timeout_seconds": 120
}
```

With Opal USB connected, run:

```sh
python3 pi-tools/hil_runner.py --config config/hil-jobs.json \
  --job partition-rejoin-canary --allow-disruptive \
  --input partition_profile=/path/to/reviewed-partition-profile.json
```

The harness requires the exact artifact to be active, confirmed, non-pending,
and locally USB-connected. It requires a sustained target-only outage, a
continuously healthy Feral sentinel, restored infrastructure and fresh group
membership, an unchanged boot sequence, and the same exact confirmed image
after rejoin. It always sends the bounded-fault cancel command during cleanup.
Restore the ordinary signed fleet artifact transactionally immediately after
qualification; keep its provisioning UF2 available for the existing recovery
harness.
