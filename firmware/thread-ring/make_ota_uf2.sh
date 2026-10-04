#!/usr/bin/env bash
# Build one UF2 for first-time USB provisioning of an OTA-capable board:
# MCUboot (0x26000) + signed app (slot0, 0x32000) in a single file.
# After this initial flash, updates go over Thread via mcumgr — USB is
# only the rescue path (1200-baud touch/double-tap -> NICENANO drive).
# The build directory is explicit when supplied, e.g.:
#   ./make_ota_uf2.sh build-debug
set -euo pipefail

WS=${ZEPHYR_WORKSPACE:-~/zephyr-ws}
FW_DIR=$(cd "$(dirname "$0")" && pwd)
B=${1:-"$FW_DIR/build-ota"}
B=$(cd "$B" && pwd)
PY="$WS/.venv/bin/python"

[ -f "$B/mcuboot/zephyr/zephyr.hex" ] || { echo "build first (west build --sysbuild -d build-ota)"; exit 1; }

"$PY" - "$B" <<'PY'
import sys
from intelhex import IntelHex
b = sys.argv[1]
ih = IntelHex(f"{b}/mcuboot/zephyr/zephyr.hex")
app = IntelHex()
# Zephyr invokes imgtool separately for .bin and .hex. RSA-PSS intentionally
# salts each signature, so those two otherwise-equivalent files do not contain
# the same signature bytes. Make the OTA binary the single source of truth and
# embed those exact bytes in the USB recovery image.
with open(f"{b}/thread-ring/zephyr/zephyr.signed.bin", "rb") as signed:
    app.frombytes(signed.read(), offset=0x32000)
ih.merge(app, overlap="error")

# Fill every byte of MCUboot and slot0, plus slot1's first erase page
# (0x26000..0x90000), that the images do not cover with 0xFF. Erasing the
# secondary header makes an interrupted candidate invalid before MCUboot can
# retry it. The nice!nano v0.6.0 USB bootloader rejects application writes at
# 0xAD000 and above, so a UF2 that tries to clear all of slot1 never completes
# or auto-resets. The recovered application can erase the remainder over SMP.
# Settings (0xEC000+) and the USB bootloader (0xF4000+) remain untouched.
lo, hi = 0x26000, 0x90000
covered = ih.segments()
addr = lo
for s, e in sorted(covered):
    s, e = max(s, lo), min(e, hi)
    if s > addr:
        ih.puts(addr, b"\xff" * (s - addr))
    addr = max(addr, e)
if addr < hi:
    ih.puts(addr, b"\xff" * (hi - addr))

ih.write_hex_file(f"{b}/combined.hex")
PY

"$PY" "$WS/zephyr/scripts/build/uf2conv.py" -c -f 0xADA52840 \
      -o "$B/combined.uf2" "$B/combined.hex"
echo "wrote $B/combined.uf2 ($(du -h "$B/combined.uf2" | cut -f1))"
