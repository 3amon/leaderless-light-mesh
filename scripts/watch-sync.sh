#!/usr/bin/env bash
# Watch group-sync convergence across the ring fleet.
#
# Reads ONLY the dashboard's heartbeat data (multicast, already flowing) — it
# issues no unicast queries, so observing does not load the mesh it measures.
# That matters: on 2026-08-31 the layout watcher's own ringctl polling became a
# meaningful share of traffic on a congested mesh.
#
# Convergence is judged by the group agreement hash (`agr`) and group pattern
# (`gpat`). Many clusters = rings running independently; one cluster = converged.
# Raw theta/ofs is NOT comparable across nodes (each is an offset to that
# board's own free-running counter) so it is deliberately not used here.
#
# Emits a line only when the picture changes, plus a periodic heartbeat so
# silence is distinguishable from a dead watcher.
set -uo pipefail

PI=${PI:-pi@example.net}
POLL=${POLL:-45}
HEARTBEAT_EVERY=${HEARTBEAT_EVERY:-8}   # samples between forced prints

last=""
n=0
while true; do
  J=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI" \
        'curl -s --max-time 10 http://localhost:8080/api/rings' 2>/dev/null)
  if [ -n "$J" ]; then
    LINE=$(printf '%s' "$J" | python3 -c '
import json,sys,datetime as dt
from collections import Counter
try: rings=json.load(sys.stdin)["rings"]
except Exception: raise SystemExit
now=dt.datetime.now(dt.timezone.utc)
def age(r):
    try: return (now-dt.datetime.fromisoformat(r["last_seen"].replace("Z","+00:00"))).total_seconds()
    except Exception: return 10**9
live=[r for r in rings.values() if age(r)<300]
if not live: print("live=0"); raise SystemExit
clusters=Counter(str(r.get("agr")) for r in live)
pats =Counter(str(r.get("gpat")) for r in live)
top=clusters.most_common(1)[0]
big=pats.most_common(1)[0]
state = "CONVERGED" if len(clusters)==1 else ("converging" if top[1]>=max(2,len(live)*2//3) else "SPLIT")
print(f"{state} live={len(live)} clusters={len(clusters)} biggest={top[1]}/{len(live)} "
      f"gpat={big[0]}x{big[1]} pats={len(pats)}")
' 2>/dev/null)
    if [ -n "$LINE" ]; then
      n=$((n+1))
      if [ "$LINE" != "$last" ] || [ $((n % HEARTBEAT_EVERY)) -eq 0 ]; then
        echo "$(date -u +%H:%M:%S) $LINE"
        last="$LINE"
      fi
    fi
  fi
  sleep "$POLL"
done
