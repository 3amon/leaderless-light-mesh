#!/usr/bin/env bash
# Sync the border-router Pi's /opt/thread-led-node tooling to this repo's
# committed main. Run from the workstation as part of EVERY release whose
# commands or replies change shape — a stale Pi client fails as confusing
# device-side errors (out_of_range, auth_required) on commands that pass
# host tests, which has now bitten three times.
#
# Only tooling and inventory are synced; the Pi's evidence directories and
# firmware artifacts are never touched.
set -euo pipefail

PI=${PI:-pi@example.net}
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"

if [ -n "$(git status --porcelain -- pi-tools config/rings.json)" ]; then
    echo "sync_pi_tools: commit pi-tools/ and config/rings.json first" >&2
    exit 1
fi

FILES=$(git ls-tree --name-only main -- pi-tools/ | grep -E '\.(py|sh)$')
FILES="$FILES config/rings.json"

failures=0
for f in $FILES; do
    local_hash=$(git show "main:$f" | sha256sum | cut -d' ' -f1)
    git show "main:$f" | ssh -o BatchMode=yes "$PI" \
        "sudo -n tee /opt/thread-led-node/$f > /dev/null && sudo -n chmod 755 /opt/thread-led-node/$f"
    remote_hash=$(ssh -o BatchMode=yes "$PI" \
        "sha256sum /opt/thread-led-node/$f" | cut -d' ' -f1)
    if [ "$local_hash" = "$remote_hash" ]; then
        echo "synced  $f"
    else
        echo "MISMATCH $f" >&2
        failures=$((failures + 1))
    fi
done

ssh -o BatchMode=yes "$PI" \
    'python3 -m py_compile /opt/thread-led-node/pi-tools/*.py && echo "pi-tools compile OK"'

[ "$failures" -eq 0 ] || { echo "sync_pi_tools: $failures mismatches" >&2; exit 1; }
echo "Pi tooling matches main."
