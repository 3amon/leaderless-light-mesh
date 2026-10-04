#!/usr/bin/env bash
# Flash the ring plugged into THIS machine to v0.1.83, over USB only.
#
# Companion to check-ring.sh. Safe and idempotent: the UF2 writes
# 0x26000..0x90000 (MCUboot + slot 0 + slot 1's header), while settings live at
# 0xEC000 — so the control key, Thread identity and layout all survive. Proven
# on dusty-lotus-shine, glowing-koi-gleam and feral-dune-haze on 2026-08-30/31.
#
# A complete write takes ~13-14 s. A 2-3 s "success" means the write was
# TRUNCATED (the board dropped off mid-copy) and will not boot — that is what
# put gentle-spark-bloom into a DFU loop. This script fails loudly on that.

set -uo pipefail

REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
UF2=${UF2:-$REPO/dist/v0.1.83/thread-ring.combined.uf2}
EXPECT_SHA=${EXPECT_SHA:-7a6a6b26a0d70fd888a6431976079ea0918942b41035c834a4da0ed59ef535d2}

die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[ -f "$UF2" ] || die "artifact missing: $UF2"
sha=$(sha256sum "$UF2" | cut -d' ' -f1)
[ "$sha" = "$EXPECT_SHA" ] || die "artifact sha mismatch: $sha"
echo "  artifact verified: $(basename "$UF2")"

# --- get it into the bootloader if it is in app mode ---------------------
PORT=$(ls /dev/serial/by-id/*thread-ring* 2>/dev/null | head -1)
if [ -n "$PORT" ]; then
  echo "  app mode on $PORT — 1200-baud touch to enter bootloader"
  stty -F "$PORT" 1200 2>/dev/null || true
fi

DEV=""
for i in $(seq 1 30); do
  DEV=$(lsblk -no LABEL,PATH 2>/dev/null | awk '/^NICENANO/{print $2}' | head -1)
  [ -n "$DEV" ] && { echo "  bootloader volume at $DEV (${i}s)"; break; }
  sleep 1
done
[ -n "$DEV" ] || die "no NICENANO volume appeared — try a double-tap reset"

MNT=$(lsblk -no MOUNTPOINT "$DEV" | head -1)
OWNED=0
if [ -z "$MNT" ]; then
  MNT=/mnt/uf2-ring
  sudo mkdir -p "$MNT"
  sudo mount -t vfat -o uid=$(id -u),gid=$(id -g),flush "$DEV" "$MNT" || die "mount failed"
  OWNED=1
fi

grep -qE 'Board-ID.*nRF52840-nicenano' "$MNT/INFO_UF2.TXT" 2>/dev/null \
  || { [ "$OWNED" = 1 ] && sudo umount "$MNT"; die "not a nice!nano board — refusing to flash"; }

echo "  writing…"
S=$(date +%s)
cp "$UF2" "$MNT/" 2>/dev/null || true   # board resets mid-write; errors are normal
sync 2>/dev/null || true
[ "$OWNED" = 1 ] && sudo umount "$MNT" 2>/dev/null || true
EL=$(( $(date +%s) - S ))
echo "  write took ${EL}s"

if [ "$EL" -lt 8 ]; then
  die "write completed in ${EL}s — TOO FAST, almost certainly truncated. Re-seat the cable and retry; do NOT leave the board in this state."
fi
echo "  OK — complete write. Board should re-enumerate in ~1-2 s."
