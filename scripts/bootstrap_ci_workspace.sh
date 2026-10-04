#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOCK="$REPO_ROOT/config/toolchain.lock.json"
WORKSPACE=${ZEPHYR_WORKSPACE:-"$REPO_ROOT/.cache/zephyr-ws"}
ZEPHYR="$WORKSPACE/zephyr"
PROFILE=${TOOLCHAIN_PROFILE:-ci}

readarray -t LOCK_VALUES < <(python3 - "$LOCK" "$PROFILE" <<'PY'
import json
import pathlib
import sys

lock = json.loads(pathlib.Path(sys.argv[1]).read_text())
profile = lock["tool_profiles"][sys.argv[2]]
print(lock["zephyr"]["revision"])
print(lock["zephyr"]["patch"])
print(lock["zephyr"]["patched_file"])
print(lock["zephyr"]["patched_file_sha256"])
print(profile.get("image", ""))
print(lock["modules"]["bootloader/mcuboot"])
print(lock["mcuboot"]["patch"])
PY
)
ZEPHYR_REVISION=${LOCK_VALUES[0]}
PATCH=${LOCK_VALUES[1]}
PATCHED_FILE=${LOCK_VALUES[2]}
PATCHED_SHA256=${LOCK_VALUES[3]}
EXPECTED_IMAGE=${LOCK_VALUES[4]}
MCUBOOT_REVISION=${LOCK_VALUES[5]}
MCUBOOT_PATCH=${LOCK_VALUES[6]}

if [ -n "${CI_JOB_IMAGE:-}" ] && [ "$CI_JOB_IMAGE" != "$EXPECTED_IMAGE" ]; then
    echo "bootstrap: CI image does not match the lock" >&2
    exit 1
fi

mkdir -p "$WORKSPACE/.venv/bin"
ln -sfn "$(command -v python3)" "$WORKSPACE/.venv/bin/python"
ln -sfn "$(command -v west)" "$WORKSPACE/.venv/bin/west"
ln -sfn "$(command -v cmake)" "$WORKSPACE/.venv/bin/cmake"

if [ ! -d "$ZEPHYR/.git" ]; then
    mkdir -p "$ZEPHYR"
    git -C "$ZEPHYR" init
    git -C "$ZEPHYR" remote add origin \
        https://github.com/zephyrproject-rtos/zephyr.git
    git -C "$ZEPHYR" fetch --depth=1 origin "$ZEPHYR_REVISION"
    git -C "$ZEPHYR" checkout --detach "$ZEPHYR_REVISION"
fi

ACTUAL_REVISION=$(git -C "$ZEPHYR" rev-parse HEAD)
if [ "$ACTUAL_REVISION" != "$ZEPHYR_REVISION" ]; then
    echo "bootstrap: cached Zephyr revision does not match the lock" >&2
    exit 1
fi

if [ ! -f "$WORKSPACE/.west/config" ]; then
    (cd "$WORKSPACE" && "$WORKSPACE/.venv/bin/west" init -l zephyr)
fi

(cd "$WORKSPACE" && "$WORKSPACE/.venv/bin/west" update --narrow -o=--depth=1 \
    cmsis hal_nordic mbedtls mcuboot openthread zcbor)

MCUBOOT="$WORKSPACE/bootloader/mcuboot"
if [ "$(git -C "$MCUBOOT" rev-parse HEAD)" != "$MCUBOOT_REVISION" ]; then
    if [ -n "$(git -C "$MCUBOOT" status --porcelain=v1)" ]; then
        echo "bootstrap: MCUboot is dirty and cannot move to the locked revision" >&2
        exit 1
    fi
    git -C "$MCUBOOT" fetch --depth=1 upstream "$MCUBOOT_REVISION"
    git -C "$MCUBOOT" switch --detach "$MCUBOOT_REVISION"
fi

if git -C "$MCUBOOT" apply --check --reverse "$REPO_ROOT/$MCUBOOT_PATCH"; then
    echo "bootstrap: MCUboot watchdog trace patch already applied"
elif git -C "$MCUBOOT" apply --check "$REPO_ROOT/$MCUBOOT_PATCH"; then
    git -C "$MCUBOOT" apply "$REPO_ROOT/$MCUBOOT_PATCH"
else
    echo "bootstrap: MCUboot checkout does not match the pinned watchdog trace patch" >&2
    exit 1
fi

ACTUAL_PATCHED_SHA256=$(sha256sum "$ZEPHYR/$PATCHED_FILE" | cut -d' ' -f1)
if [ "$ACTUAL_PATCHED_SHA256" != "$PATCHED_SHA256" ]; then
    if [ -n "$(git -C "$ZEPHYR" status --porcelain=v1)" ]; then
        echo "bootstrap: Zephyr is dirty but the required patch is absent" >&2
        exit 1
    fi
    git -C "$ZEPHYR" apply "$REPO_ROOT/$PATCH"
fi

python3 "$REPO_ROOT/scripts/check_workspace.py" \
    --workspace "$WORKSPACE" \
    --profile "$PROFILE" \
    --sdk "${ZEPHYR_SDK_INSTALL_DIR:-/opt/toolchains/zephyr-sdk-0.17.0}"
