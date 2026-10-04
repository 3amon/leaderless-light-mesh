#!/usr/bin/env bash
# Create/update the pinned west workspace and apply tracked dependency fixes.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=${ZEPHYR_WORKSPACE:-"$(dirname "$REPO_ROOT")/zephyr-ws"}
ZEPHYR_PATCH="$REPO_ROOT/patches/zephyr-usbd-ready-retry.patch"
MCUBOOT_PATCH=$(python3 -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["mcuboot"]["patch"])' \
    "$REPO_ROOT/config/toolchain.lock.json")
MCUBOOT_REVISION=$(python3 -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["modules"]["bootloader/mcuboot"])' \
    "$REPO_ROOT/config/toolchain.lock.json")

if [ ! -d "$WORKSPACE/.west" ]; then
    mkdir -p "$WORKSPACE"
    west init -l "$REPO_ROOT" "$WORKSPACE"
fi
cd "$WORKSPACE"
west update cmsis hal_nordic mbedtls openthread zcbor
if [ ! -d bootloader/mcuboot/.git ]; then
    west update mcuboot
fi
west zephyr-export

if git -C zephyr apply --check --reverse "$ZEPHYR_PATCH"; then
    echo "Zephyr USBD retry patch already applied"
elif git -C zephyr apply --check "$ZEPHYR_PATCH"; then
    git -C zephyr apply "$ZEPHYR_PATCH"
    echo "applied Zephyr USBD retry patch"
else
    echo "Zephyr checkout does not match the pinned USBD patch" >&2
    exit 1
fi

if [ "$(git -C bootloader/mcuboot rev-parse HEAD)" != "$MCUBOOT_REVISION" ]; then
    if [ -n "$(git -C bootloader/mcuboot status --porcelain=v1)" ]; then
        echo "MCUboot is dirty and cannot move to the locked revision" >&2
        exit 1
    fi
    git -C bootloader/mcuboot fetch upstream "$MCUBOOT_REVISION"
    git -C bootloader/mcuboot switch --detach "$MCUBOOT_REVISION"
fi

if git -C bootloader/mcuboot apply --check --reverse "$REPO_ROOT/$MCUBOOT_PATCH"; then
    echo "MCUboot watchdog trace patch already applied"
elif git -C bootloader/mcuboot apply --check "$REPO_ROOT/$MCUBOOT_PATCH"; then
    git -C bootloader/mcuboot apply "$REPO_ROOT/$MCUBOOT_PATCH"
    echo "applied MCUboot watchdog trace patch"
else
    echo "MCUboot checkout does not match the pinned watchdog trace patch" >&2
    exit 1
fi

python3 "$REPO_ROOT/scripts/check_workspace.py" --workspace "$WORKSPACE"
