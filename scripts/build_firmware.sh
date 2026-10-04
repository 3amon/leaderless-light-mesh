#!/usr/bin/env bash
# Reproducible entry point for the supported debug and fleet build profiles.
set -euo pipefail

PROFILE=${1:-fleet}
case "$PROFILE" in
    fleet|debug|provisioning|rollback|partition-hil) ;;
    *) echo "usage: $0 [debug|fleet|provisioning|rollback|partition-hil]" >&2; exit 2 ;;
esac

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
FW="$REPO_ROOT/firmware/thread-ring"
ZEPHYR_WORKSPACE=${ZEPHYR_WORKSPACE:-"$(dirname "$REPO_ROOT")/zephyr-ws"}
ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-"$ZEPHYR_WORKSPACE/zephyr-sdk-0.17.0"}
export ZEPHYR_SDK_INSTALL_DIR
export PATH="$ZEPHYR_WORKSPACE/.venv/bin:$PATH"

WEST="$ZEPHYR_WORKSPACE/.venv/bin/west"
SIZE_TOOL="$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-size"
BUILD_DIR=${BUILD_DIR:-"$FW/build-$PROFILE"}
BOARD=promicro_nrf52840/nrf52840/uf2
PRISTINE=${PRISTINE:-auto}

mkdir -p "$(dirname "$BUILD_DIR")"

[ -x "$WEST" ] || { echo "west not found: $WEST" >&2; exit 2; }
[ -x "$SIZE_TOOL" ] || { echo "size tool not found: $SIZE_TOOL" >&2; exit 2; }

python3 "$REPO_ROOT/scripts/check_workspace.py" \
    --workspace "$ZEPHYR_WORKSPACE" \
    --profile "${TOOLCHAIN_PROFILE:-local}" \
    --sdk "$ZEPHYR_SDK_INSTALL_DIR"
python3 "$REPO_ROOT/scripts/check_partitions.py"

VERSION_CONF="${BUILD_DIR}.version.conf"
trap 'rm -f "$VERSION_CONF"' EXIT
version_args=()
if [ -n "${FIRMWARE_VERSION:-}" ]; then
    version_args=(--override "$FIRMWARE_VERSION")
fi
python3 "$REPO_ROOT/scripts/version_info.py" "${version_args[@]}" --format conf \
    > "$VERSION_CONF"

cmake_args=()
max_flash=100
if [ "$PROFILE" = fleet ] || [ "$PROFILE" = provisioning ] || \
   [ "$PROFILE" = rollback ] || [ "$PROFILE" = partition-hil ]; then
    if [ -z "${MCUBOOT_SIGNING_KEY:-}" ]; then
        echo "MCUBOOT_SIGNING_KEY is required for $PROFILE builds" >&2
        exit 2
    fi
    python3 "$REPO_ROOT/scripts/signing.py" \
        --workspace "$ZEPHYR_WORKSPACE" --key "$MCUBOOT_SIGNING_KEY" >/dev/null
    extra_conf="$FW/fleet.conf;$VERSION_CONF"
    if [ "$PROFILE" = provisioning ]; then
        extra_conf="$FW/fleet.conf;$FW/provisioning.conf;$VERSION_CONF"
    elif [ "$PROFILE" = rollback ]; then
        extra_conf="$FW/fleet.conf;$FW/rollback-test.conf;$VERSION_CONF"
    elif [ "$PROFILE" = partition-hil ]; then
        extra_conf="$FW/fleet.conf;$FW/partition-hil.conf;$VERSION_CONF"
    fi
    cmake_args=(-- "-Dthread-ring_EXTRA_CONF_FILE=$extra_conf")
    cmake_args+=("-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$MCUBOOT_SIGNING_KEY\"")
    max_flash=92
else
    cmake_args=(-- "-Dthread-ring_EXTRA_CONF_FILE=$VERSION_CONF")
fi

cd "$ZEPHYR_WORKSPACE"
"$WEST" build --sysbuild -p "$PRISTINE" -b "$BOARD" -d "$BUILD_DIR" "$FW" \
    "${cmake_args[@]}"

python3 "$REPO_ROOT/scripts/check_firmware_size.py" \
    --size-tool "$SIZE_TOOL" \
    --max-flash-percent "$max_flash" \
    "$BUILD_DIR/thread-ring/zephyr/zephyr.elf" \
    | tee "$BUILD_DIR/size-report.txt"

echo "signed image: $BUILD_DIR/thread-ring/zephyr/zephyr.signed.bin"
if [ "$PROFILE" = fleet ] || [ "$PROFILE" = provisioning ]; then
    "$FW/make_ota_uf2.sh" "$BUILD_DIR"
fi
