#!/usr/bin/env bash
# Emit one line per ring plugged into THIS laptop, then run check-ring.sh.
# Used during the 2026-08-31 straggler hunt: rings come to the laptop one at a
# time to be checked for <= v0.1.63 firmware (which forces the whole fleet onto
# the chatty COMPAT sync cadence).
set -uo pipefail
SEEN=""
while true; do
  CUR=$(ls -1 /dev/serial/by-id/ 2>/dev/null | grep -E 'thread-ring|nice_nano' | sort | tr '\n' ' ')
  if [ "$CUR" != "$SEEN" ]; then
    if [ -n "$CUR" ] && [ "$CUR" != " " ]; then
      echo "=== ring attached: $CUR"
      "$(dirname "$0")/check-ring.sh" 2>&1
    else
      echo "=== ring removed (nothing attached)"
    fi
    SEEN="$CUR"
  fi
  sleep 3
done
