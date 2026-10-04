#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
SNAPSHOT=${1:-}
PI_HOST=${2:-pi@example.net}

if [[ ! "$SNAPSHOT" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
    echo "usage: $0 YYYY-MM-DD [PI_HOST]" >&2
    exit 2
fi

DESTINATION="$REPO_ROOT/evidence/hardware/$SNAPSHOT"
if [ -e "$DESTINATION" ]; then
    echo "refusing to overwrite existing snapshot: $DESTINATION" >&2
    exit 1
fi

mkdir -p "$DESTINATION"

rsync -a --checksum \
    --exclude=dashboard-before.json \
    --exclude=dashboard-after.json \
    "$PI_HOST:/home/pi/boot-tests/" "$DESTINATION/boot-tests/"
rsync -a --checksum \
    "$PI_HOST:/home/pi/deployments/" "$DESTINATION/deployments/"
rsync -a --checksum \
    "$PI_HOST:/home/pi/sync-tests/" "$DESTINATION/sync-tests/"

echo "archived Pi hardware evidence under $DESTINATION"
