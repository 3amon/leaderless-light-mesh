#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=${ZEPHYR_WORKSPACE:-"$(dirname "$REPO_ROOT")/zephyr-ws"}
BUILD_ROOT=${BUILD_DIR:-"$REPO_ROOT/ci-build/firmware"}
TEST_SOURCE="$REPO_ROOT/tests/zephyr/boot_ota"
TEST_BUILD="${BUILD_ROOT}-native-sim-boot-ota"
PATH="$WORKSPACE/.venv/bin:$PATH"
export PATH

(cd "$WORKSPACE" && .venv/bin/west build --pristine=always \
    --board native_sim/native/64 --build-dir "$TEST_BUILD" "$TEST_SOURCE")
(cd "$TEST_BUILD" && ./zephyr/zephyr.exe)
