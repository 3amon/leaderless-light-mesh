# Transactional firmware deployment

`pi-tools/deploy.py` is the only supported OTA state machine. It runs on the
Thread border-router Pi and uses `smp_transport.py` for typed SMP responses.
The old `ota.sh` name remains only as a compatibility wrapper.

## Pi layout

Keep these paths together so the default inventory and transport helper can be
found relative to the CLI:

```text
thread-ring-tools/
├── config/rings.json
└── pi-tools/
    ├── deploy.py
    ├── ota.sh
    ├── quick_update.py
    └── smp_transport.py
```

The helper runs with the Python interpreter from the installed `smpmgr`
launcher. This reuses its pinned `smpclient` dependency while keeping the main
deployment state machine standard-library-only and host-testable.

SMP uses a 192-byte UDP MTU by default. Fleet testing found this substantially
more resilient than 512-byte datagrams on lossy Thread links because each
request needs fewer 802.15.4 fragments. A lost upload request is retried with
the exact same offset, data, and SMP sequence up to three times, using a
three-second per-request timeout. This lets the device acknowledge either the
original write or its idempotent resend without resetting the upload session.
The controls are `--smp-mtu`, `--smp-chunk-attempts`, and
`--smp-upload-request-timeout`.

Process-level upload attempts are also resumable: a bounded retry asks the
inactive slot for its stored offset. The default eight attempts cover a
complete image even if a transient fault still ends several upload processes;
`--upload-attempts` accepts 1 through 12.

All host-originated Thread UDP paths use `thread_socket.py`. After a Thread
partition change, OTBR can retain old RLOC/ALOC addresses on `wpan0`; normal
Linux source selection may choose one even though rings cannot route replies
to that transient locator. The helper selects an ordinary OTBR OMR address in
the destination /64 and binds both control and SMP sockets to it. Off the Pi,
or when no safe matching address exists, it falls back to kernel selection.

## Commands

Inspect and validate a signed image without contacting hardware:

```sh
python3 pi-tools/deploy.py inspect-image zephyr.signed.bin
```

Deploy to the canary by its inventory identity:

```sh
python3 pi-tools/deploy.py deploy opal-reef-tide zephyr.signed.bin
```

Mutating commands are authenticated by default. The invoking account must own
the target's private key at
`/etc/thread-led-node/control-keys/<inventory-name>.key`, or pass
`--control-key-dir`. Provision a new key over exact inventoried USB with
`pi-tools/provision_control_key.py` before the first authenticated deployment.
The explicit `--allow-legacy-control` escape hatch is for the one-time upgrade
from pre-v2 firmware only.

Roll out canary-first, stopping at the first failure or ambiguity:

```sh
python3 pi-tools/deploy.py fleet zephyr.signed.bin
```

Firmware v0.1.65 and later can suppress their own background multicast before
a large rollout while keeping Thread control and SMP fully reachable. The
authenticated duration is in seconds, is RAM-only, and automatically expires:

```sh
sudo python3 pi-tools/ringctl.py opal-reef-tide quiet 1800
python3 pi-tools/ringctl.py opal-reef-tide quiet
sudo python3 pi-tools/ringctl.py opal-reef-tide quiet off
```

Quiet each OTA target sequentially before starting the fleet transaction. Each
successful command removes that node's sync beacons and scheduled heartbeat,
so delivery to the remaining targets becomes progressively easier. A quiet
node continues receiving Thread traffic, answering control requests, serving
SMP image state and upload requests, and running the normal MCUboot
confirmation policy. Quiet mode also suspends singleton-heal reboots. It lasts
30 seconds to four hours, clears on reboot, and resumes normal traffic when its
timer expires; it is therefore not a persistent configuration or a substitute
for the deployer's transactional gates.

Set or inspect the persistent viral fleet mode through any reachable node:

```sh
sudo python3 pi-tools/ringctl.py opal-reef-tide mode party 10
python3 pi-tools/ringctl.py opal-reef-tide mode
sudo python3 pi-tools/ringctl.py opal-reef-tide mode normal 20
```

The number is supplied explicitly and is never auto-incremented. Higher values
win across the mesh and across reboot; lower commands return the current winner
with `result=rejected`. Equal-priority disagreement resolves to normal.

For rapid bench churn with exactly one inventoried node connected to the Pi,
use the dedicated **fleet-profile** quick-update UF2 from the release bundle:

```sh
python3 pi-tools/quick_update.py \
  --artifact thread-ring.quick-update.uf2 \
  --artifact-sha256 "$(sha256sum thread-ring.quick-update.uf2 | cut -d' ' -f1)" \
  --inventory config/rings.json \
  --out evidence/quick-update
```

This path binds the physical USB serial to inventory, checks the artifact hash
and exact safe UF2 address range, flashes MCUboot plus the fleet application,
and waits for the same USB serial to return. It intentionally skips Thread and
post-boot image-state qualification. Never substitute
`thread-ring.combined.uf2`: that artifact is the provisioning profile for
first-time setup and recovery.

Downgrades and same-version/different-hash builds are rejected unless the
operator supplies `--allow-downgrade` or `--allow-same-version`. An exact image
that is already active, confirmed, reachable, and identity-matched is an
idempotent `already_current` success.

## Transaction gates

For each device, the tool:

1. Acquires a non-blocking per-device lock.
2. Validates inventory identity, direct reachability, `infra=1`, and the
   current confirmed slot.
3. Parses the signed MCUboot header and TLVs, checks its content digest, and
   enforces version policy.
4. Uploads with bounded attempts and timeouts.
5. Polls typed SMP state until the inactive slot has the exact expected
   version and MCUboot hash.
6. Rechecks identity, active state, and inactive hash immediately before the
   firmware self-arms that slot.
7. Sends the authenticated arm, observes the device leave and return, and
   succeeds only when the exact candidate is active, confirmed, and reports
   infrastructure reachability. If the self-reboot races the authenticated
   reply, it records the missing acknowledgement and reconciles the result
   from the observed departure and exact signed post-boot image state. It
   never resends the ambiguous mutation.

If the exact unconfirmed candidate is already present in the inactive slot,
the transaction skips the upload but still repeats the identity, active-slot,
and exact-candidate checks immediately before arm. This makes a retry after a
host or authorization interruption idempotent without weakening the arm gate.
Malformed, unauthenticated, or mismatched replies still stop immediately; only
an absent reply enters post-reboot reconciliation.

Every run writes `invocation.json`, `events.jsonl`, and `result.json`. Exit 1
is a definitive classified failure, exit 2 is local input/configuration error,
and exit 3 means device state may be ambiguous and requires inspection before
another mutation.

Important classifications include `upload_failed`, `stale_slot`, `rollback`,
`confirmation_timeout`, `reboot_loss`, `arm_ambiguous`, `success`, and
`already_current`.

`deploy.py stage TARGET IMAGE` uses the same identity, version-policy, upload,
and exact inactive-slot checks but never invokes the arm command. It returns
only after a final identity/baseline/candidate re-read, and is intended for the
artifact-bound rollback harness rather than normal fleet rollout.
