# Hardware test evidence

Structured results from hardware-in-the-loop runs on real rings, copied from
the gateway Raspberry Pi that ran them. One snapshot, `2026-07-20`, kept with
the Pi's original run-directory names. [`SUMMARY.md`](SUMMARY.md) tabulates
every run, including the ones that failed.

The snapshot covers the first two rings ("Opal" and "Feral") during the weeks
the boot chain, the OTA transaction and the signing chain were qualified:

- `boot-tests/` — 33 run directories: reboot soaks, forced-rollback trials,
  USB/UF2 recovery trials and read-only observations;
- `deployments/` — 23 transactional OTA and fleet-verification runs,
  including five uploads that were deliberately interrupted;
- `sync-tests/` — 9 camera-backed synchronisation runs (flash edges seen by a
  camera, so the measurement is of the LEDs, not of the firmware's own
  telemetry).

Each run has `invocation.json` (what was asked), `events.jsonl` (what
happened, timestamped) and `result.json` (the verdict). 119 JSON files and 81
JSONL files with 4,239 records.

## What was changed for publication

- USB serial numbers and Thread addresses are replaced with invented ones,
  consistently, so a device has the same invented identity in every file and
  in `config/rings.json`.
- Three always-empty fields that the gateway's dashboard adds for an unrelated
  system sharing the gateway (`scans`, `last_scan_ts`, `last_scan_uid`) are
  removed from `dashboard_node` records.
- Kernel logs, device state captures and harness stdout (raw host output, 270
  files) are not included. Dashboard snapshots were never archived.

Nothing else is altered: timings, hashes, classifications and failures are as
recorded.

## What this evidence does not cover

It ends on 2026-07-20, at firmware v0.1.18. Later qualification (the camera
run quoted in the top-level README, the mobility and rotation runs, the
recovery-ladder bench test, fleet rollouts) is described in
`docs/development-plan.md` and the investigation write-ups, with paths to
evidence that stayed on the gateway and is not in this repository.
