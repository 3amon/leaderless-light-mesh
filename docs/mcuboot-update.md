# One-time MCUboot fleet update

`pi-tools/mcuboot_update.py` performs the physical-USB MCUboot update and the
following OTA verification as one fail-closed transaction. Run it on the
border-router Pi with exactly the selected inventoried board attached by USB.
Other rings may remain on dedicated power; one must be selected as the healthy
Thread sentinel.

The command:

1. verifies every profile hash and parses all signed-image metadata;
2. requires the recovery UF2 to cover every 256-byte block in exactly
   `0x26000..0x90000`;
3. verifies the signed slot-0 image embedded in that UF2;
4. resolves the USB CDC device and UF2 disk by the target's inventoried serial,
   never `/dev/sda` position;
5. requires a healthy target, healthy sentinel, confirmed baseline, and no
   pending image;
6. snapshots the artifacts into the evidence directory;
7. installs the recovery UF2 and verifies the exact recovered image;
8. erases stale slot-1 state, transactionally deploys the forward image, and
   verifies its exact active/confirmed hash;
9. optionally observes a rollback candidate active and unconfirmed, proves the
   exact forward image is restored, and erases the test image; and
10. requires final `stage=confirmed`, `mcuboot=00/00`, expected reset reason,
    healthy infrastructure, healthy sentinel, and an empty secondary slot.

## Reviewed profile

Keep the profile with the release artifacts, outside the repository. Relative
artifact paths resolve from the profile directory.

```json
{
  "schema": 1,
  "recovery": {
    "artifact": "thread-ring-v0144-recovery.uf2",
    "file_sha256": "replace-with-combined-uf2-sha256",
    "signed_file_sha256": "replace-with-embedded-signed-bin-sha256",
    "version": "0.1.44",
    "image_hash": "replace-with-embedded-mcuboot-image-hash",
    "start": "0x26000",
    "end_exclusive": "0x90000"
  },
  "forward": {
    "artifact": "thread-ring-v0145-forward.signed.bin",
    "file_sha256": "replace-with-forward-file-sha256",
    "version": "0.1.45",
    "image_hash": "replace-with-forward-mcuboot-image-hash"
  },
  "rollback": {
    "artifact": "thread-ring-v0146-rollback.signed.bin",
    "file_sha256": "replace-with-rollback-file-sha256",
    "version": "0.1.46",
    "image_hash": "replace-with-rollback-mcuboot-image-hash"
  }
}
```

The profile is rejected before hardware mutation if any file, embedded image,
version, hash, range, block sequence, or version ordering differs.

## Carrier canary

Use the rollback qualification on the first carrier PCB:

```sh
sudo -n python3 pi-tools/mcuboot_update.py \
  --profile /home/pi/releases/mcuboot-v0145/profile.json \
  --target feral-tide-light \
  --sentinel opal-reef-tide \
  --qualification \
  --allow-disruptive \
  --smpmgr /home/pi/.local/bin/smpmgr \
  --out /home/pi/deployments/mcuboot-v0145/feral
```

Success returns zero and writes `result.json`, step logs, nested transaction
records, and immutable artifact snapshots beneath `--out`. A failed or
ambiguous step stops before the next board.

The arm reply can be lost when a board reboots faster than the radio can
deliver its authenticated acknowledgement. The transaction does not resend
the command. It accepts the update only after observing the board depart and
return with the exact expected signed image active and confirmed.

## Remaining boards

After the carrier gate passes, connect each remaining board to the Pi one at a
time and omit `--qualification`:

```sh
sudo -n python3 pi-tools/mcuboot_update.py \
  --profile /home/pi/releases/mcuboot-v0145/profile.json \
  --target lucid-iris-light \
  --sentinel opal-reef-tide \
  --allow-disruptive \
  --smpmgr /home/pi/.local/bin/smpmgr \
  --out /home/pi/deployments/mcuboot-v0145/lucid
```

Never reuse an evidence directory. If a run fails, preserve it and choose a
new output path after diagnosing the recorded stopping point.
