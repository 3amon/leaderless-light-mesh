#!/usr/bin/env bash
# Check a thread-led-node ring plugged into THIS machine, over USB only.
#
# Written 2026-08-31 during the straggler hunt: any ring running <= v0.1.63
# transmits legacy beacons, and ONE of them drags the whole fleet back to the
# chatty COMPAT sync cadence (450-550 ms instead of 1800-2600 ms) for 120 s
# after every beacon. See SYNC_COMPAT_QUIET_MS in sync_schedule.h.
#
# Works with no Thread radio and no mesh, so it is usable from the laptop while
# the mesh is congested — which is the point.
#
# Reading the running version over USB only works if the board still has the
# shell (the bootstrap/provisioning image, CONFIG_SHELL=y). The fleet image
# builds with CONFIG_SHELL=n, so for those the version is simply unreadable
# here and the honest answer is "unknown — flash to be certain".

set -uo pipefail

REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
UF2=${UF2:-$REPO/dist/v0.1.83/thread-ring.combined.uf2}
INV=$REPO/config/rings.json
IDENT=$REPO/pi-tools/identify_target.py

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- identify
JSON=$(python3 "$IDENT" --json --inventory "$INV" 2>/dev/null)
if [ -z "$JSON" ]; then say "no ring attached to this machine"; exit 1; fi

python3 - "$JSON" "$INV" <<'PY'
import json, sys, subprocess, re, glob, os

targets = json.loads(sys.argv[1]).get("targets", [])
inv = {d["usb_serial"]: d for d in json.load(open(sys.argv[2]))["devices"] if d.get("usb_serial")}
if not targets:
    print("no ring attached"); raise SystemExit(1)

def read_version(port):
    """Only works if the board still has the shell (bootstrap image)."""
    try:
        subprocess.run(["stty", "-F", port, "115200", "raw", "-echo"],
                       capture_output=True, timeout=5)
        fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    except Exception:
        return None
    try:
        import time, select
        os.write(fd, b"\r\nring status\r\n")
        buf = b""
        end = time.time() + 4
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.3)
            if r:
                try: buf += os.read(fd, 4096)
                except BlockingIOError: pass
            m = re.search(rb"build=(v[0-9][^\s]*)", buf)
            if m: return m.group(1).decode()
        return None
    finally:
        os.close(fd)

for t in targets:
    serial, mode, port = t["usb_serial"], t["mode"], t["port"]
    rec = inv.get(serial)
    name = t.get("name") or (rec or {}).get("name") or "NOT INVENTORIED"
    print(f"  ring:      {name}")
    print(f"  serial:    {serial}")
    print(f"  usb mode:  {mode}")
    if rec:
        print(f"  inventory: order {rec['rollout_order']}, canary={rec.get('canary', False)}")
    else:
        print(f"  inventory: NOT PRESENT — this is a blank/unknown board")

    if mode == "dfu":
        print("  version:   (in bootloader — cannot read; ready to flash)")
        print("  VERDICT:   FLASH")
        continue

    v = read_version(port)
    if v is None:
        # The fleet image has no shell, so fall back to the last version this
        # ring REPORTED to the border router. Not authoritative (it may have
        # been reflashed since) but far better than defaulting to "flash".
        print("  version:   unreadable over USB (fleet image has CONFIG_SHELL=n)")
        last = None
        src = ""
        # LIVE query first. The dashboard's cached img is stale for minutes
        # after a flash — trusting it re-flashes a board that is already
        # current. ringctl asks the ring itself, so it reflects reality.
        try:
            out = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                 "pi@example.net",
                 "cd /home/pi/deployments/20260815-v0169-usb-quiet-group/source/pi-tools && "
                 f"sudo -n timeout 20 python3 ringctl.py {name} status"],
                capture_output=True, text=True, timeout=40).stdout
            m = re.search(r"build=(v[0-9][^\s]*)", out)
            if m:
                last, src = m.group(1), "live, asked the ring"
        except Exception:
            pass
        if last is None:
            try:
                out = subprocess.run(
                    ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                     "pi@example.net",
                     "curl -s --max-time 8 http://localhost:8080/api/rings"],
                    capture_output=True, text=True, timeout=25).stdout
                for r in (json.loads(out).get("rings") or {}).values():
                    if r.get("name") == name:
                        last, src = r.get("img"), "CACHED — may be stale after a flash"
                        break
            except Exception:
                pass
        if last:
            print(f"  version reported: {last}  ({src})")
            try:
                nums = tuple(int(x) for x in re.findall(r"\d+", str(last).split("+")[0])[:3])
                if nums <= (0, 1, 63):
                    print("  VERDICT:   FLASH — forces the WHOLE fleet to COMPAT sync")
                elif nums < (0, 1, 83):
                    print("  VERDICT:   FLASH — behind v0.1.83 (mode arbitration)")
                else:
                    print("  VERDICT:   OK — already v0.1.83")
            except Exception:
                print("  VERDICT:   FLASH TO BE SURE (could not parse)")
        else:
            print("  no record on the border router either")
            print("  VERDICT:   FLASH TO BE SURE  (idempotent; keys+identity survive)")
    else:
        print(f"  version:   {v}")
        try:
            nums = tuple(int(x) for x in re.findall(r"\d+", v)[:3])
            if nums <= (0, 1, 63):
                print("  VERDICT:   FLASH — forces the WHOLE fleet to COMPAT sync")
            elif nums < (0, 1, 83):
                print("  VERDICT:   FLASH — behind v0.1.83 (mode arbitration)")
            else:
                print("  VERDICT:   OK — already v0.1.83")
        except Exception:
            print("  VERDICT:   FLASH TO BE SURE (could not parse version)")
PY
