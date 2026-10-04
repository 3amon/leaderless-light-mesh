# Firmware and release signing

The release chain uses separate keys for separate trust decisions:

- MCUboot accepts an application only when its RSA-PSS signature matches the
  project firmware public key embedded in MCUboot.
- Git accepts release tags and commits signed by the maintainer SSH key listed
  in `config/release-signers`.
- Release consumers can verify detached SSH signatures on `SHA256SUMS` and
  `provenance.json` using the same release identity.

The firmware private key and release private key must never be committed.
Keep them outside the repository, for example:

```text
/secure/thread-led-node/firmware-rsa-2048.pem
/secure/thread-led-node/release-ed25519
```

`config/release-signers.example` shows the allowed-signers format; copy it to
`config/release-signers` with your own public key and identity.

Both files must have mode `0600`. Fleet and provisioning builds fail closed
without `MCUBOOT_SIGNING_KEY`, reject MCUboot's public development key by its
public-key hash, and record only the project public-key identity in release
provenance.

## Build and release

Build a project-signed provisioning image with:

```sh
export MCUBOOT_SIGNING_KEY=/secure/thread-led-node/firmware-rsa-2048.pem
scripts/build_firmware.sh provisioning
```

Strict releases additionally require a signed exact-version tag and the
release attestation key:

```sh
export RELEASE_SIGNING_KEY=/secure/thread-led-node/release-ed25519
git tag -s v0.1.17 -m "thread-ring firmware v0.1.17"
scripts/release.py
```

The release command verifies the tag, performs two pristine builds, verifies
both firmware signatures, enforces the exact recovery-UF2 address range, and
creates verified SSH signatures for the checksum manifest and provenance.

## Control-plane authentication

Firmware signing decides which application may boot. A separate per-device
256-bit key decides who may send a mutating UDP command. Do not reuse either
key for the other purpose.

Read-only diagnostics remain available without a key. Reboot, update, and
configuration commands require this exact v2 form:

```text
v2 <16-lowercase-hex-sequence> <command...> <24-lowercase-hex-tag>
```

The tag is the first 12 bytes of HMAC-SHA256 over the exact text preceding the
tag. Responses carry the same sequence and their own HMAC. The device commits
the accepted sequence and request tag to CRC-protected settings before the
side effect. An exact retry returns `duplicate` without repeating the side
effect; an older sequence or different request at the same sequence is a
persistent replay error. This provides authenticated, at-most-once mutation:
a power loss after the durable acceptance record but before the side effect
can conservatively lose an operation rather than execute it twice.

Keys live at `/etc/thread-led-node/control-keys/<inventory-name>.key` by
default. The directory must be private and each file must be a regular,
non-symlink, owner-only `0600` file containing exactly 64 lowercase hex
characters. `THREAD_LED_CONTROL_KEY_DIR` or each tool's `--control-key-dir`
option may select another protected installation path.

After installing a project-signed provisioning image on exactly one
USB-connected ring, provision and verify it from the Pi with:

```sh
sudo python3 pi-tools/provision_control_key.py --target opal-reef-tide \
  --out /var/lib/thread-led-node/provisioning/opal-control-auth.json
```

The tool resolves the logical name, Thread address, and USB serial through
`config/rings.json`; it never chooses a positional tty. It creates or reuses a
private key, sends it only through that USB shell, then proves key agreement
with an authenticated heartbeat. Its JSON evidence contains only a SHA-256
key fingerprint. Fleet images omit the key-install command and fail closed for
all UDP mutations until provisioned. `--allow-legacy-control` exists only to
cross the one-time old-firmware migration boundary and must not be used after
the authenticated fleet image is installed.

These symmetric keys prevent an attacker with mesh access or repository
access from authorizing mutations. They do not resist physical flash
extraction, a compromised Pi account that can read the key files, or a
compromised running firmware image. Back up the key directory encrypted in
two controlled locations. Rotation requires physical USB access to each
explicit inventory device, canary-first verification, and retention of a
known-good recovery UF2.

## USB recovery boundary

The Adafruit/nice!nano UF2 bootloader occupies `0xF4000..0x100000`, but the
v0.6.0 build accepts application writes only below `0xAD000`. The combined
recovery UF2 is therefore required to write exactly `0x26000..0x90000`:
MCUboot, a fresh slot 0, and slot 1's first 4 KiB erase page. Clearing the
secondary image header makes an interrupted candidate invalid before MCUboot
can retry it. A larger UF2 that tries to erase the secondary trailer is
silently rejected above `0xAD000` and never auto-resets. Once the recovered
application runs, the normal SMP recovery flow erases the remainder of slot 1.
Thread settings and the USB bootloader remain untouched.

Consequently, a bad application, wrong MCUboot public key, lost signing key, or
failed OTA trust transition can be recovered by entering the Adafruit
bootloader and flashing a new combined UF2 over USB/serial DFU. Recovery is not
guaranteed if the Adafruit bootloader region itself is overwritten, its entry
mechanism is disabled, or the hardware fails. The release tool refuses a UF2
whose address range could touch that region.

`pi-tools/uf2_recovery_test.py` additionally requires the target name, Thread
address, and currently attached USB serial to resolve to one record in
`config/rings.json`; it also requires a different inventoried ring as the
healthy sentinel. It never chooses the first serial device by position.
`pi-tools/mcuboot_update.py` wraps that primitive for fleet migration using a
reviewed, artifact-bound profile and refuses to run when any other inventoried
ring is locally USB-attached. See `docs/mcuboot-update.md`.

## Custody and rotation

The workstation copies are an initial custody mechanism, not the final fleet
design. Before scaling the fleet:

1. store encrypted backups of the firmware, release, and control keys in two
   independent locations;
2. move firmware signing behind an offline or hardware-backed signer;
3. allow signing only from reviewed, protected release tags;
4. rotate by provisioning one canary with a recovery UF2 containing the new
   MCUboot public key and an application signed by the matching private key;
5. prove USB recovery and OTA on that canary before updating other devices.

Do not reuse the firmware key for Git authentication, tags, or attestations.

## Fleet transition result

On 2026-07-20, the USB-connected Opal canary was provisioned with the exact
v0.1.17 combined UF2. MCUboot reported the project-signed v0.1.17 image active
and confirmed. Opal then accepted and confirmed a temporary v0.1.18 image over
Thread, proving the new trust root, before the native UF2 path restored the
exact published v0.1.17 image. The recovery preserved the Adafruit bootloader
and used the already documented watchdog-assisted USB handoff path.

After swapping the physical USB connection, Feral was resolved through its
inventoried serial `DEC0DE0000000002` and received the same exact v0.1.17
provisioning image. It then accepted and confirmed the project-signed v0.1.18
probe over Thread and returned directly to the published v0.1.17 image over
USB. Final transactional fleet verification found both rings healthy,
active, and confirmed on the exact v0.1.17 release hash.
