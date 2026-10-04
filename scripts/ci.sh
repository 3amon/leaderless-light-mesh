#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"
python3 -m py_compile scripts/*.py pi-tools/*.py tests/*.py
bash -n scripts/*.sh pi-tools/*.sh firmware/thread-ring/*.sh
python3 -m unittest discover -s tests -v
python3 scripts/check_partitions.py
python3 scripts/check_workspace.py \
    --workspace "${ZEPHYR_WORKSPACE:-$(dirname "$REPO_ROOT")/zephyr-ws}" \
    --profile "${TOOLCHAIN_PROFILE:-local}" \
    --sdk "${ZEPHYR_SDK_INSTALL_DIR:-${ZEPHYR_WORKSPACE:-$(dirname "$REPO_ROOT")/zephyr-ws}/zephyr-sdk-0.17.0}"
git diff --check -- ':!firmware/thread-ring/build-*/**'
scripts/test_native_sim.sh
CI_KEY_DIR=$(mktemp -d)
trap 'rm -rf "$CI_KEY_DIR"' EXIT
CI_SIGNING_KEY="$CI_KEY_DIR/firmware-rsa-2048.pem"
"${ZEPHYR_WORKSPACE:-$(dirname "$REPO_ROOT")/zephyr-ws}/.venv/bin/python" \
    "${ZEPHYR_WORKSPACE:-$(dirname "$REPO_ROOT")/zephyr-ws}/bootloader/mcuboot/scripts/imgtool.py" \
    keygen -k "$CI_SIGNING_KEY" -t rsa-2048
chmod 600 "$CI_SIGNING_KEY"
MCUBOOT_SIGNING_KEY="$CI_SIGNING_KEY" scripts/build_firmware.sh fleet
MCUBOOT_SIGNING_KEY="$CI_SIGNING_KEY" scripts/build_firmware.sh provisioning
MCUBOOT_SIGNING_KEY="$CI_SIGNING_KEY" scripts/build_firmware.sh rollback
MCUBOOT_SIGNING_KEY="$CI_SIGNING_KEY" scripts/build_firmware.sh partition-hil
