#!/usr/bin/env bash
# Hands-free flash for the SuperMini nRF52840 (Adafruit UF2 bootloader).
# Touches the app's CDC port at 1200 baud -> app reboots into bootloader
# -> NICENANO drive appears -> copy the UF2. First-ever flash (no app yet):
# board is already in bootloader, so this just copies.
set -euo pipefail

UF2="${1:-$(dirname "$0")/build/zephyr/zephyr.uf2}"
MOUNT="/media/$USER/NICENANO"

[ -f "$UF2" ] || { echo "no UF2 at $UF2 (build first)"; exit 1; }

if [ ! -d "$MOUNT" ]; then
    PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)
    if [ -n "${PORT:-}" ]; then
        echo "touching $PORT at 1200 baud..."
        stty -F "$PORT" 1200 2>/dev/null || true
    else
        echo "no serial port found — double-tap RST to enter the bootloader"
    fi
    printf "waiting for %s" "$MOUNT"
    for _ in $(seq 1 40); do
        [ -d "$MOUNT" ] && break
        printf "."
        sleep 0.5
    done
    echo
fi

[ -d "$MOUNT" ] || { echo "bootloader drive never appeared"; exit 1; }
cp "$UF2" "$MOUNT/" && sync
echo "flashed $(basename "$UF2") ($(du -h "$UF2" | cut -f1))"
