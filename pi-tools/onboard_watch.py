#!/usr/bin/env python3
"""Watch USB for thread-led-node boards and onboard them as they appear.

Emits one JSON line per event on stdout. Designed to be tailed.

Full provisioning pipeline per BLANK board, matching the canonical
2026-08-11 onboarding evidence layout:

  bootstrap (reviewed recovery UF2)
    -> join -> inventory -> control key
    -> mcuboot_update.py  (recovery -> erase -> forward -> erase)
    -> deploy.py to the fleet image
    -> layout ring16 -> final verification

!! DO NOT bootstrap from thread-ring.combined.uf2 of the current release. !!
On 2026-08-23 that mistake was made on three boards: it installs MCUboot
built from that release instead of the REVIEWED MCUboot, and skips the
mcuboot_update.py qualification entirely. One of the three (feral-dune-haze)
then failed OTA with a reproducible `classification: rollback` -- exactly the
failure that qualification step exists to catch.

The bootstrap artifact is therefore read from the reviewed profile's
`recovery` entry, so it is the qualified artifact by construction and cannot
drift from the profile mcuboot_update.py is about to enforce.
"""
from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import subprocess
import sys
import time

SRC = pathlib.Path("/home/pi/deployments/20260815-v0169-usb-quiet-group/source")
TOOLS = SRC / "pi-tools"
INVENTORY = SRC / "config/rings.json"
SIGNED = pathlib.Path("/home/pi/thread-ring-0.1.83.signed.bin")
EVIDENCE_ROOT = pathlib.Path("/home/pi/onboard-evidence")

# dist/v0.1.83/provenance.json records `recovery_uf2.image.version = 0.1.83`:
# thread-ring.combined.uf2 IS the recovery UF2 for this release, and that
# provenance pins mcuboot 2d61c318 (v2.2.0) + mcuboot-watchdog-trace.patch --
# the same commit and patch set as config/toolchain.lock.json, i.e. the same
# bootloader the fleet's reviewed baseline uses.
UF2 = pathlib.Path("/home/pi/thread-ring-0.1.83.combined.uf2")
UF2_SHA = "7a6a6b26a0d70fd888a6431976079ea0918942b41035c834a4da0ed59ef535d2"
SMPMGR = "/home/pi/.local/bin/smpmgr"
DASH = "http://localhost:8080/api/rings"
# Geometry for the NEXT board onboarded. Read per board rather than baked in,
# so a run of jewel7s can be slipped in between ring16s without restarting:
#   echo jewel7 > /home/pi/onboard-watch/layout
# Pixel counts and default brightness come from the firmware's led_layout.c --
# ring16 16px@45%, jewel7 7px@100%, rope100 100px@100%.
LAYOUT_FILE = pathlib.Path("/home/pi/onboard-watch/layout")
VALID_LAYOUTS = ("ring16", "jewel7", "rope100")
DEFAULT_LAYOUT = "ring16"
POLL = 3.0
JOIN_TIMEOUT = 240.0
BOARD_ID = "nRF52840-nicenano"
# Flashes of the same board before declaring a boot loop rather than retrying.
MAX_FLASH_ATTEMPTS = 2


def log(event: str, **kw) -> None:
    rec = {"ts": dt.datetime.now(dt.timezone.utc).isoformat(), "event": event}
    rec.update(kw)
    print(json.dumps(rec), flush=True)


def run(cmd: list[str], timeout: float = 600.0) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def inventory() -> dict:
    return json.loads(INVENTORY.read_text())


def inv_serials() -> set[str]:
    return {d["usb_serial"] for d in inventory()["devices"] if d.get("usb_serial")}


def inv_addrs() -> set[str]:
    return {d["address"] for d in inventory()["devices"]}


def live_rings() -> dict:
    out = run(["curl", "-s", "--max-time", "10", DASH], timeout=20)
    if out.returncode != 0 or not out.stdout.strip():
        return {}
    try:
        return json.loads(out.stdout)["rings"]
    except Exception:
        return {}


def app_targets() -> list[dict]:
    """Boards enumerated in application mode, via the fleet's own identifier."""
    out = run([sys.executable, str(TOOLS / "identify_target.py"), "--json",
               "--inventory", str(INVENTORY)], timeout=30)
    # Exit status is a classification, not success/failure: 0 = one known target,
    # 1 = none attached, 2 = unknown or ambiguous, 3 = error. rc 2 is exactly the
    # case this watcher exists for, so trust stdout whenever it parses.
    if not out.stdout.strip():
        return []
    try:
        return [t for t in json.loads(out.stdout)["targets"] if t["mode"] == "app"]
    except Exception:
        log("identify_unparsed", rc=out.returncode,
            detail=(out.stdout + out.stderr).strip()[:200])
        return []


def current_layout() -> str:
    """Layout for the board being onboarded right now.

    An unreadable or unrecognised value falls back to ring16 and says so, rather
    than passing junk to ringctl or silently giving a jewel7 the wrong geometry.
    """
    try:
        want = LAYOUT_FILE.read_text().strip()
    except OSError:
        return DEFAULT_LAYOUT
    if want in VALID_LAYOUTS:
        return want
    if want:
        log("layout_invalid", requested=want[:40], using=DEFAULT_LAYOUT,
            valid=list(VALID_LAYOUTS))
    return DEFAULT_LAYOUT


def dfu_serial() -> str | None:
    """USB serial of a board sitting in the UF2 bootloader, when it exposes one.

    identify_target.py recognises `nice_nano_<serial>` by-id names as mode=dfu,
    which lets a flash loop be attributed to a specific board rather than to
    whichever device happens to land on /dev/sda.
    """
    out = run([sys.executable, str(TOOLS / "identify_target.py"), "--json",
               "--inventory", str(INVENTORY)], timeout=30)
    if not out.stdout.strip():
        return None
    try:
        for t in json.loads(out.stdout)["targets"]:
            if t.get("mode") == "dfu":
                return t.get("usb_serial")
    except Exception:
        return None
    return None


def bootloader_drives() -> list[dict]:
    """UF2 bootloader mass-storage volumes (these boards expose no serial in DFU)."""
    out = run(["lsblk", "-J", "-o", "NAME,LABEL,PATH,TRAN,MOUNTPOINT,SIZE"], timeout=20)
    if out.returncode != 0:
        return []
    found = []

    def walk(nodes, tran=None):
        for n in nodes:
            t = n.get("tran") or tran
            if n.get("label") == "NICENANO" and t == "usb":
                found.append(n)
            walk(n.get("children") or [], t)

    try:
        walk(json.loads(out.stdout)["blockdevices"])
    except Exception:
        return []
    return found


MOUNT_ROOT = pathlib.Path("/mnt/uf2-onboard")


def mount(dev: str) -> str | None:
    """Mount with sudo, not udisksctl: a detached watcher has no tty for polkit."""
    ls = run(["lsblk", "-no", "MOUNTPOINT", dev], timeout=20)
    existing = ls.stdout.strip().splitlines()
    if existing and existing[0].strip():
        return existing[0].strip()
    run(["sudo", "-n", "mkdir", "-p", str(MOUNT_ROOT)], timeout=30)
    out = run(["sudo", "-n", "mount", "-t", "vfat",
               "-o", f"uid={os.getuid()},gid={os.getgid()},flush",
               dev, str(MOUNT_ROOT)], timeout=60)
    if out.returncode == 0:
        return str(MOUNT_ROOT)
    log("mount_failed", device=dev,
        detail=(out.stdout + out.stderr).strip()[:300])
    return None


def unmount() -> None:
    run(["sync"], timeout=60)
    run(["sudo", "-n", "umount", str(MOUNT_ROOT)], timeout=60)


def flash(drive: dict) -> bool:
    dev = drive["path"]
    mp = drive.get("mountpoint") or mount(dev)
    if not mp:
        return False
    info = pathlib.Path(mp) / "INFO_UF2.TXT"
    try:
        text = info.read_text(errors="replace")
    except OSError as exc:
        log("flash_skipped", device=dev, reason=f"no INFO_UF2.TXT: {exc}")
        unmount()
        return False
    if BOARD_ID not in text:
        log("flash_skipped", device=dev, reason="board-id mismatch",
            detail=text.strip()[:200])
        unmount()
        return True  # deliberate: do not retry a foreign board
    log("flashing", device=dev, mountpoint=mp)
    cp = run(["cp", str(UF2), f"{mp}/"], timeout=180)
    # The board resets itself mid-write as UF2 completes; I/O errors here are normal.
    unmount()
    log("flashed", device=dev, rc=cp.returncode,
        detail=(cp.stderr.strip()[:200] or None))
    return True


def resolve_identity(baseline: set[str]) -> tuple[str, str] | None:
    """Wait for exactly one genuinely-new node on the mesh."""
    deadline = time.monotonic() + JOIN_TIMEOUT
    stable: tuple[str, str] | None = None
    while time.monotonic() < deadline:
        known = inv_addrs() | baseline
        cand = [(a, r.get("name")) for a, r in live_rings().items()
                if a not in known and r.get("name")]
        if len(cand) == 1:
            if stable == cand[0]:
                return cand[0]
            stable = cand[0]
        elif len(cand) > 1:
            log("identity_ambiguous", candidates=[c[1] for c in cand])
            stable = None
        else:
            stable = None
        time.sleep(POLL)
    return None


def add_to_inventory(name: str, address: str, serial: str) -> None:
    ts = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run(["cp", "-a", str(INVENTORY), f"{INVENTORY}.bak-{ts}"], timeout=30)
    doc = inventory()
    devs = doc["devices"]
    if any(d["name"] == name or d["usb_serial"] == serial for d in devs):
        log("inventory_skipped", name=name, reason="already present")
        return
    devs.append({
        "name": name, "address": address, "usb_serial": serial,
        "canary": False,
        "rollout_order": max(d["rollout_order"] for d in devs) + 1,
    })
    INVENTORY.write_text(json.dumps(doc, indent=2) + "\n")
    log("inventoried", name=name, address=address, usb_serial=serial,
        rollout_order=devs[-1]["rollout_order"], count=len(devs))


def pick_sentinel(target_name: str) -> str | None:
    """A healthy inventoried ring, not the target: the control that proves the
    mesh is fine when the target goes quiet mid-update."""
    now = dt.datetime.now(dt.timezone.utc)
    inv = {d["name"] for d in inventory()["devices"]}
    best = None
    preferred_ok = False
    for ring in live_rings().values():
        name = ring.get("name")
        if name == target_name or name not in inv or ring.get("hw") != "ok":
            continue
        seen = ring.get("last_seen")
        if not seen:
            continue
        try:
            age = (now - dt.datetime.fromisoformat(
                str(seen).replace("Z", "+00:00"))).total_seconds()
        except ValueError:
            continue
        if age < 180:
            if name == PREFERRED_SENTINEL and name != target_name:
                preferred_ok = True
            if best is None or age < best[0]:
                best = (age, name)
    if preferred_ok:
        return PREFERRED_SENTINEL
    return best[1] if best else None


def step(label: str, cmd: list[str], name: str, timeout: float = 600.0) -> bool:
    out = run(cmd, timeout=timeout)
    ok = out.returncode == 0
    log(label, name=name, ok=ok,
        detail=((out.stdout + out.stderr).strip()[-300:] if not ok else None))
    return ok


def onboard(serial: str, baseline: set[str]) -> None:
    log("onboarding", usb_serial=serial, layout=current_layout())
    ident = resolve_identity(baseline)
    if not ident:
        log("onboard_failed", usb_serial=serial, reason="no unambiguous new mesh node")
        return
    address, name = ident
    log("identified", usb_serial=serial, name=name, address=address)
    add_to_inventory(name, address, serial)

    if not step("control_key", ["sudo", "-n", sys.executable,
                str(TOOLS / "provision_control_key.py"), "--target", name,
                "--timeout", "60"], name, 300):
        return
    # mcuboot_update.py is deliberately NOT run here. The only reviewed profile
    # on the Pi (mcuboot-v0145) has a v0.1.44 recovery artifact, and that image
    # does not boot on this hardware batch: on 2026-08-23 it took a clean,
    # complete write on feral-dune-haze and never enumerated again. Recovery was
    # double-tap DFU plus rewriting v0.1.83.
    #
    # Skipping it is safe for the bootloader itself -- dist/v0.1.83's provenance
    # pins the same mcuboot commit and patch set as config/toolchain.lock.json,
    # so this is the same MCUboot VERSION, just without the qualification
    # TRANSACTION (verified install, slot-1 erase, transactional forward deploy).
    #
    # To re-enable: build a profile whose `recovery` artifact is known to boot on
    # this batch, with `forward` at a higher version -- which needs a build above
    # 0.1.83, since 0.1.83 is currently the top of the tree.

    # A failed OTA gate is recorded but does not abort. The board already runs
    # the correct image from bootstrap, and the known watchdog-interrupted swap
    # (reset=0x12 = RESET_WATCHDOG|RESET_SOFTWARE, reboot reason "none") would
    # otherwise leave the node inventoried but without geometry.
    ota_ok = step("deployed", ["sudo", "-n", sys.executable, str(TOOLS / "deploy.py"),
                  "deploy", name, str(SIGNED), "--allow-same-version",
                  "--smpmgr", SMPMGR], name, 900)
    if not ota_ok:
        log("ota_gate_failed", name=name,
            note="runs correct image from bootstrap; OTA path unproven on this board")
    layout = current_layout()
    if not step("layout_set", ["sudo", "-n", sys.executable, str(TOOLS / "ringctl.py"),
                name, "layout", layout], name, 180):
        return
    log("onboard_complete", usb_serial=serial, name=name, address=address,
        layout=layout, ota_gate="passed" if ota_ok else "FAILED-watchdog-swap")


BASELINE_STALE_SECONDS = 300.0


def stale_uninventoried() -> set[str]:
    """Uninventoried mesh records that predate this run.

    Presence alone is the wrong test: if the watcher restarts after a freshly
    flashed board has already joined, that board looks "pre-existing" and would
    be excluded from its own identification. Only records that have not been
    heard from recently are treated as background.
    """
    now = dt.datetime.now(dt.timezone.utc)
    known = inv_addrs()
    stale = set()
    for addr, ring in live_rings().items():
        if addr in known:
            continue
        seen = ring.get("last_seen")
        if not seen:
            stale.add(addr)
            continue
        try:
            ts = dt.datetime.fromisoformat(str(seen).replace("Z", "+00:00"))
        except ValueError:
            stale.add(addr)
            continue
        if (now - ts).total_seconds() > BASELINE_STALE_SECONDS:
            stale.add(addr)
    return stale


def main() -> int:
    if not UF2.is_file():
        log("fatal", reason=f"missing {UF2}")
        return 2
    sha = run(["sha256sum", str(UF2)], timeout=60).stdout.split()[0]
    if sha != UF2_SHA:
        log("fatal", reason="uf2 sha256 mismatch", found=sha)
        return 2

    baseline = stale_uninventoried()
    log("watcher_ready", uf2=str(UF2), uf2_sha256=sha, layout=current_layout(),
        layout_file=str(LAYOUT_FILE),
        inventory=str(INVENTORY), inventoried=len(inv_addrs()),
        excluded_preexisting=sorted(baseline))

    done: set[str] = set()
    flashed: set[str] = set()
    attempts: dict[str, int] = {}
    quarantined: set[str] = set()
    while True:
        try:
            for drive in bootloader_drives():
                key = drive["path"] + (drive.get("label") or "")
                if key in flashed:
                    continue
                # A board whose freshly written image will not boot escapes back
                # into the UF2 bootloader (the boot-recovery ladder), where an
                # unguarded watcher flashes it again -- forever, wearing flash
                # and never converging. Seen on gentle-spark-bloom 2026-08-23.
                # Identity comes from the DFU serial when the bootloader exposes
                # one, so a genuinely different board is never blamed.
                ident = dfu_serial() or key
                if ident in quarantined:
                    continue
                attempts[ident] = attempts.get(ident, 0) + 1
                if attempts[ident] > MAX_FLASH_ATTEMPTS:
                    quarantined.add(ident)
                    log("flash_loop_quarantined", identity=ident,
                        attempts=attempts[ident] - 1,
                        note="board keeps returning to DFU after a good write; "
                             "needs hands-on diagnosis, not another flash")
                    continue
                if flash(drive):
                    flashed.add(key)

            known = inv_serials()
            for t in app_targets():
                serial = t["usb_serial"]
                if serial in done:
                    continue
                done.add(serial)
                if serial in known:
                    log("already_inventoried", usb_serial=serial, name=t.get("name"))
                    continue
                onboard(serial, baseline)

            # let a re-plugged board be reconsidered once it is gone
            present = {t["usb_serial"] for t in app_targets()}
            done &= present | {s for s in done if s in inv_serials()}
            live_paths = {d["path"] + (d.get("label") or "") for d in bootloader_drives()}
            flashed &= live_paths
        except Exception as exc:  # never let one bad poll kill the watcher
            log("poll_error", detail=repr(exc)[:300])
        time.sleep(POLL)


if __name__ == "__main__":
    sys.exit(main())
