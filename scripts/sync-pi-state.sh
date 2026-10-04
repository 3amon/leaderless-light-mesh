#!/usr/bin/env bash
# Mirror ring-gateway's irreplaceable state off the Pi.
#
# The Pi is where control keys and inventory are WRITTEN FIRST, and its root is
# a read-write ext4 on SD. Until 2026-08-23 the control keys existed in exactly
# one place: /etc/thread-led-node/control-keys on that card. Losing it means no
# node in the fleet can be commanded or deployed to again, ever.
#
# Run this after every onboarding session.
#
# Refuses to overwrite the local inventory if the Pi's copy would DROP a device
# or change an existing one -- that is the shape of a corrupted or rolled-back
# file, and silently syncing it would destroy the good local copy.

set -euo pipefail

PI=${PI:-pi@example.net}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
PI_SRC=${PI_SRC:-/home/pi/deployments/20260815-v0169-usb-quiet-group/source}
KEYDIR=${KEYDIR:-$HOME/.config/thread-led-node/control-keys}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

say() { printf '%s\n' "$*"; }
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

say "== inventory =="
scp -q "$PI:$PI_SRC/config/rings.json" "$TMP/pi-rings.json" || die "cannot fetch rings.json"

python3 - "$TMP/pi-rings.json" "$REPO/config/rings.json" <<'PY' || die "inventory regression; NOT synced"
import json, sys
pi = {d["name"]: d for d in json.load(open(sys.argv[1]))["devices"]}
try:
    lo = {d["name"]: d for d in json.load(open(sys.argv[2]))["devices"]}
except FileNotFoundError:
    lo = {}
lost = sorted(set(lo) - set(pi))
conflict = sorted(n for n in set(pi) & set(lo) if pi[n] != lo[n])
new = sorted(set(pi) - set(lo))
print(f"  pi={len(pi)} local={len(lo)} new={new or 'none'}")
if lost:
    print(f"  REFUSING: pi copy is missing {lost}")
    raise SystemExit(1)
if conflict:
    print(f"  REFUSING: fields changed for {conflict}")
    raise SystemExit(1)
PY

cp "$TMP/pi-rings.json" "$REPO/config/rings.json"
say "  inventory synced into $REPO/config/rings.json"

say "== control keys =="
mkdir -p "$KEYDIR"; chmod 700 "$(dirname "$KEYDIR")" "$KEYDIR"
ssh -o BatchMode=yes "$PI" 'sudo -n tar cf - -C /etc/thread-led-node control-keys' \
  | tar xf - -C "$(dirname "$KEYDIR")" || die "key mirror failed"
chmod 700 "$KEYDIR"; chmod 600 "$KEYDIR"/*.key

ssh -o BatchMode=yes "$PI" 'sudo -n sh -c "cd /etc/thread-led-node/control-keys && sha256sum *.key"' \
  | sort > "$TMP/pi.sha"
(cd "$KEYDIR" && sha256sum -- *.key) | sort > "$TMP/local.sha"
if ! diff -q "$TMP/pi.sha" "$TMP/local.sha" >/dev/null; then
  diff "$TMP/pi.sha" "$TMP/local.sha" | head
  die "key hashes differ after mirror"
fi
say "  $(wc -l < "$TMP/pi.sha") keys mirrored, all hashes verified"

say "== cross-check =="
python3 - "$REPO/config/rings.json" "$KEYDIR" <<'PY'
import json, pathlib, sys
names = {d["name"] for d in json.load(open(sys.argv[1]))["devices"]}
keys = {p.stem for p in pathlib.Path(sys.argv[2]).glob("*.key")}
missing, orphan = sorted(names - keys), sorted(keys - names)
print(f"  devices={len(names)} keys={len(keys)}")
if missing: print(f"  inventoried without a key: {missing}")
if orphan:  print(f"  key without inventory (do NOT delete): {orphan}")
if not missing and not orphan: print("  one-to-one")
PY

say "== git =="
cd "$REPO"
if git diff --quiet -- config/rings.json; then
  say "  no inventory change to commit"
else
  git --no-pager diff --stat -- config/rings.json
  say "  review, then: git add config/rings.json && git commit && git push"
fi
