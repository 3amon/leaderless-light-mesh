# Reproducible firmware builds and releases

The repository is the west manifest repository. `west.yml` pins Zephyr to an
exact commit and imports only the six modules used by this firmware; their
exact revisions and the supported host tools are recorded in
`config/toolchain.lock.json`. MCUboot is advanced after the Zephyr manifest
update to the exact v2.2.0 commit containing the upstream sector-erase
watchdog-feed fix. A tracked MCUboot patch feeds the inherited watchdog during
image-hash iteration, before signature verification, and at each validation,
swap, and handoff phase boundary. It records retained-register swap breadcrumbs
without flash writes or logging, while preserving application reboot reasons
on successful boots. The tracked downstream Zephyr patch
replaces an unbounded nRF52840 USBD READY spin with a power-cycle retry.

## Bootstrap

Install Zephyr SDK 0.17.0 and Python 3.12.3, create a virtual environment from
`requirements-build.txt`, then run:

```sh
ZEPHYR_WORKSPACE=/path/to/zephyr-ws scripts/bootstrap_workspace.sh
```

The checker accepts no dependency drift: every Git SHA must match, ordinary
module checkouts must be clean, and the Zephyr and MCUboot checkouts may contain
only their exact tracked patches.

Run the complete pre-merge gate locally with:

```sh
scripts/local_ci.sh
```

It creates an ignored dev-tools environment from the pinned development
requirements, runs pre-commit, bootstraps and verifies the pinned workspace,
executes the host and native Zephyr tests, checks partitions and repository
hygiene, generates an ephemeral non-fleet signing key, and builds all four
signed firmware profiles with size gates. This is the default validation path
while shared-runner compute is constrained.

GitLab retains equivalent host and firmware jobs, but pipeline creation is
opt-in. Set the pipeline or project variable `ENABLE_SHARED_RUNNER_CI=1` to
temporarily enable them. Firmware CI uses the project's ARM-only image from
`ci/Dockerfile`, pinned by registry manifest digest. The image uses a
digest-pinned base, checksum-verified Zephyr SDK 0.17.0 metadata and ARM
archives, and the exact Python requirements. Its SDK and tool versions have a
separate `ci` profile in the toolchain lock. The CI signing key is generated
for each job and is not a fleet release key.

The local `local` profile remains the authoritative environment for strict
releases. Equivalent GitHub Actions and opt-in GitLab CI definitions are kept
because the repository may be mirrored, while GitLab is the current configured
Git remote.

## Development builds

Each profile has one command:

```sh
scripts/build_firmware.sh debug
MCUBOOT_SIGNING_KEY=/secure/key.pem scripts/build_firmware.sh fleet
MCUBOOT_SIGNING_KEY=/secure/key.pem scripts/build_firmware.sh provisioning
FIRMWARE_VERSION=0.1.23 MCUBOOT_SIGNING_KEY=/secure/key.pem \
  scripts/build_firmware.sh rollback
FIRMWARE_VERSION=0.1.23 MCUBOOT_SIGNING_KEY=/secure/key.pem \
  scripts/build_firmware.sh partition-hil
```

`fleet` produces the signed OTA binary. `provisioning` also combines MCUboot
and slot 0 into a recovery UF2 that erases through the first page of slot 1 at
`0x90000`. This clears slot-0 state and invalidates an interrupted secondary
image while staying below the nice!nano v0.6.0 bootloader's `0xAD000`
application-write ceiling. The settings partition and provisioned identity are
preserved. Untagged development builds
receive a numeric MCUboot version derived from commit count and a diagnostic
Git build ID. A temporary hardware-test version can be explicit,
for example `FIRMWARE_VERSION=0.1.15 scripts/build_firmware.sh fleet`; the
strict release command never permits an override.

The `rollback` profile is a signed, fleet-sized test image that deliberately
never confirms and reboots after eight seconds so MCUboot restores the prior
confirmed slot. Its version is supplied by release qualification rather than
a stale checked-in Kconfig value. Never deploy this profile to the fleet.

The `partition-hil` profile is also signed and fleet-derived, but adds only a
bounded physical-USB shell command for Thread detach/rejoin qualification.
The command is compiled out of fleet and rollback images.

## Releases

Release commits must be clean and tagged exactly `vMAJOR.MINOR.PATCH`:

```sh
git tag -s v0.2.0 -m "thread-ring firmware v0.2.0"
scripts/release.py
```

The command verifies the workspace and partition map and performs two pristine
fleet builds plus two pristine provisioning builds with the commit timestamp
as `SOURCE_DATE_EPOCH`. The fleet profile supplies the OTA binary; the
provisioning profile supplies only the combined USB recovery UF2. It compares
each profile's application and bootloader payloads byte-for-byte and requires
the signed binaries and combined UF2s to differ only in the RSA-PSS signature
bytes. RSA-PSS intentionally uses random salt, so every signature is
independently verified against the pinned key instead of pretending those
bytes should be identical. It also validates both profiles' exact tag-derived
image version, then creates
`dist/vMAJOR.MINOR.PATCH/` containing:

- signed OTA binary and combined provisioning UF2;
- SHA-256 manifest;
- fleet and provisioning size reports and resolved Kconfig, plus the fleet
  linker map, MCUboot Kconfig, and resolved devicetree;
- JSON provenance containing source state, image metadata, toolchain lock,
  signing-key fingerprint, and artifact hashes.

It refuses dirty or unsigned-tagged source, missing or insecure private keys,
the MCUboot development key, dependency/patch drift, inconsistent partitions,
an unsafe recovery-UF2 address range, oversized firmware, unsigned images,
version mismatch, and non-reproducible output. It signs and immediately
verifies `SHA256SUMS` and `provenance.json`; see `docs/security.md`.

## v0.1.16 release validation

The first strict release completed from a clean detached checkout of annotated
tag `v0.1.16` at `e657acac423c775997df50c9b341d847d870c2ab` on 2026-07-20.
The release bundle is `dist/v0.1.16/`. Its important identities are:

- signed OTA binary SHA-256:
  `cc2f54015095ba36e9db617ee5178f7783e667ef5d6e8f49c1663b3ed74a4549`;
- combined provisioning UF2 SHA-256:
  `b958c216300110a185772bbaa10c89cb55cb78e2feff5c13305f31ce5382242d`;
- MCUboot image hash:
  `7FC6A4F47067946EDA9B9009800BDF57843C4A647C720F03E0FEE502998B0C7F`;
- fleet image size: 288,304/372,736 bytes FLASH (77.35%) and
  108,730/262,144 bytes RAM (41.48%).

The exact packaged UF2 passed a direct-handoff canary test on Opal. The exact
packaged signed binary then passed transactional OTA on Feral. The final fleet
verification classified both devices as `already_current`, active, confirmed,
and healthy on the same Thread partition. Evidence is retained on the Pi at
`/home/pi/boot-tests/v0116-release-uf2-canary-001`,
`/home/pi/deployments/v0116-release-feral-rollout`, and
`/home/pi/deployments/v0116-release-fleet-verification`.
The structured Pi evidence is also retained in the repository snapshot at
`evidence/hardware/2026-07-20`.

Branch `ota-thread`, tag `v0.1.16`, and the complete release bundle were
published as a GitLab Release on 2026-07-20.
