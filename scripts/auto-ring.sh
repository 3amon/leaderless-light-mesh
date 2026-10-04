#!/usr/bin/env bash
# Bring every ring that reaches this laptop to v0.1.83, automatically.
#
# Goal (2026-08-31): get the whole fleet coherent. Anything not on v0.1.83 is
# flashed as it is plugged in; v0.1.83 boards are left alone.
#
# Why flash everything below 0.1.83, not just <= v0.1.63:
#   - <= v0.1.63 forces the WHOLE fleet onto the chatty COMPAT sync cadence
#     (SYNC_COMPAT_QUIET_MS, sync_schedule.h) — the sync bug.
#   - v0.1.64..v0.1.82 do not force COMPAT but still sit outside mode
#     arbitration in practice (observed on humming-mirage-cloud and
#     glowing-wisp-wave, both v0.1.67, stranded on their own agreement hash).
#
# SAFETY
#   - A board is flashed at most MAX_FLASH times per run. A board that keeps
#     returning to DFU after a good write is QUARANTINED, not re-flashed:
#     that loop is what wrecked gentle-spark-bloom on 2026-08-23.
#   - flash-ring.sh refuses a write that completes in under 8s (truncated).
#   - A board already on v0.1.83 is never touched.
set -uo pipefail

BIN="$(cd "$(dirname "$0")" && pwd)"
MAX_FLASH=2
declare -A TRIED=() DONE=()

ts() { date -u +%H:%M:%S; }
serial_of() { ls -1 /dev/serial/by-id/ 2>/dev/null | grep -E 'thread-ring|nice_nano' | head -1 \
              | grep -oE '[0-9A-F]{16}' | head -1; }

LAST=""
while true; do
  S=$(serial_of)
  if [ -z "$S" ]; then LAST=""; sleep 3; continue; fi

  # Announce a board already handled this run, but only on a NEW attachment —
  # otherwise the 3s poll would repeat it forever. Silence here is what made a
  # re-plugged board look like a new one.
  if [ -n "${DONE[$S]:-}" ]; then
    if [ "$S" != "$LAST" ]; then
      NAME=$("$BIN/check-ring.sh" 2>/dev/null | awk '/^  ring:/{print $2}')
      case "${DONE[$S]}" in
        ok)          W="was already current" ;;
        flashed)     W="FLASHED by this run" ;;
        quarantined) W="QUARANTINED — needs hands-on" ;;
        no-return)   W="flashed but did not re-enumerate" ;;
        *)           W="handled" ;;
      esac
      echo "$(ts) REPEAT ${NAME:-$S} ($S) — already handled this session ($W). Nothing to do; grab a different one."
      LAST=$S
    fi
    sleep 3; continue
  fi

  OUT=$("$BIN/check-ring.sh" 2>&1)
  echo "$(ts) --- $S"
  echo "$OUT"

  if echo "$OUT" | grep -q "VERDICT:   OK"; then
    echo "$(ts) RESULT $S already current — leaving alone"
    DONE[$S]=ok; LAST=$S
    sleep 3; continue
  fi

  n=${TRIED[$S]:-0}
  if [ "$n" -ge "$MAX_FLASH" ]; then
    echo "$(ts) QUARANTINED $S — $n flashes already, still not current. Needs hands-on."
    DONE[$S]=quarantined; LAST=$S
    sleep 3; continue
  fi
  TRIED[$S]=$((n+1))

  echo "$(ts) FLASHING $S (attempt $((n+1))/$MAX_FLASH)"
  if "$BIN/flash-ring.sh" 2>&1; then
    # Do NOT re-check via the border router here. Its record still holds the
    # PRE-flash version until the ring rejoins the mesh and heartbeats, which
    # can take minutes — re-checking would report the old version and trigger a
    # pointless second flash on every single board.
    #
    # The trustworthy success signal is flash-ring.sh itself: it verifies the
    # artifact hash, confirms the Board-ID, and fails if the write completes in
    # under 8s (a truncated write). A clean ~14s write is the confirmation.
    echo "$(ts) FLASHED $S — waiting for it to re-enumerate"
    back=no
    for i in $(seq 1 20); do
      sleep 2
      [ "$(serial_of)" = "$S" ] && { back=yes; break; }
    done
    if [ "$back" = yes ]; then
      echo "$(ts) RESULT $S flashed to v0.1.83 and re-enumerated — done"
      DONE[$S]=flashed; LAST=$S
    else
      echo "$(ts) WARNING $S did not re-enumerate within 40s — check it by hand"
      DONE[$S]=no-return
    fi
  else
    echo "$(ts) FLASH FAILED $S — see message above"
  fi
  sleep 3
done
