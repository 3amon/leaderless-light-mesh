#!/usr/bin/env python3
"""
ringctl.py — command thread-led-node boards over the Thread mesh.

  ringctl.py <name|ipv6> <command...>

Commands (see firmware poll_commands): status | layout [ring16|jewel7|rope100] |
power [carrier|module-vcc] |
mode [normal|party|ultra [priority] [blink_ms] [duty] [hard|fade]]
(priority defaults to current+1 — the wrap-safe serial scheme never
runs out. The effect applies to ANY mode: blink=0 none (default),
else period in ms 130-2550 steps of 10. duty 20-80 or 100=solid: hard
wave = on-fraction, fade wave = brightness floor of the breathe.
`mode party blink=1000 duty=30 wave=fade` = party breathing 30-100%) |
flash <ms> | flash off | scramble <ms> | pattern <n|auto> | bright <pct> |
quiet [30..14400|off] | sleep | hb | solo [on|off] |
always-on [on|off] (permanently USB-powered: never the quiet charging display)

Names are resolved via the local gateway dashboard (nodes table, populated by
heartbeats), so `ringctl.py led-ring status` works from any border router.
"""

import argparse
import json
import pathlib
import re
import socket
import sys
import urllib.request

import control_client
import thread_socket

PORT = 4242
DASH = "http://localhost:8080/api/state"


def resolve(name):
    if ":" in name:  # already an IPv6 address
        return name
    with urllib.request.urlopen(DASH, timeout=5) as r:
        d = json.load(r)
    for key, node in (d.get("nodes") or {}).items():
        if node.get("name") == name:
            for cand in (node.get("thread_addr"), key):
                if cand and ":" in str(cand):
                    return str(cand)
    sys.exit(f"ringctl: no node named {name!r} on the dashboard")


def probe_mode(addr):
    probe = thread_socket.udp_socket(addr, 5)
    probe.sendto(b"mode", (addr, PORT))
    try:
        return probe.recv(512).decode()
    except socket.timeout:
        sys.exit(f"ringctl: no reply from {addr} for mode query")


def rewrite_mode(command, reply):
    """Auto-priority: bid current+1 (serial comparison makes +1 win
    forever) and carry the currently configured effect forward unless the
    caller overrides it — a bare `mode party` must not silently wipe a
    configured blink/duty/wave (the register owns the effect since v3) or
    a configured accent= (since v4)."""
    match = re.search(r"priority=(\d+)", reply)
    if not match:
        sys.exit(f"ringctl: unrecognized mode reply: {reply}")
    bumped = (int(match.group(1)) + 1) % 2**32
    options = dict(o.split("=", 1) for o in command[2:])
    rewritten = ["mode", command[1], str(bumped)]
    blink = options.get("blink")
    if blink is None:
        current = re.search(r"blink=(\d+)ms", reply)
        blink = current.group(1) if current else "0"
    accent = options.get("accent")
    if accent is None:
        current = re.search(r"accent=(\d+)", reply)
        # A node too old to report an accent renders the full swell, so
        # that is what "unchanged" means against those replies.
        accent = current.group(1) if current else "100"

    effect_wanted = int(blink) != 0 or "duty" in options or "wave" in options
    # accent is the SIXTH positional argument, so a non-default accent has
    # to carry blink/duty/wave along with it even when no effect is set.
    if not effect_wanted and int(accent) == 100:
        return rewritten  # no effect or accent configured or requested
    rewritten.append(blink)
    duty = options.get("duty")
    if duty is None:
        current = re.search(r"duty=(\d+)", reply)
        duty = current.group(1) if current else "50"
    rewritten.append(duty)
    wave = options.get("wave")
    if wave is None:
        current = re.search(r"wave=(hard|fade)", reply)
        wave = current.group(1) if current else "hard"
    if wave not in ("hard", "fade"):
        sys.exit("ringctl: wave= must be hard or fade")
    rewritten.append(wave)
    rewritten.append(accent)
    return rewritten


def send_command(args, parser, addr, cmd):
    if control_client.command_is_mutating(cmd):
        device_name = args.device_name or (args.target if ":" not in args.target else None)
        if not device_name:
            parser.error("--device-name is required for an authenticated raw-IPv6 mutation")
        try:
            return control_client.authenticated_udp_command(
                addr,
                device_name,
                cmd,
                5,
                key_path=control_client.key_path_for(device_name, args.control_key_dir),
            )
        except control_client.ControlAuthError as exc:
            sys.exit(f"ringctl: {exc}")
    s = thread_socket.udp_socket(addr, 5)
    s.sendto(cmd.encode(), (addr, PORT))
    try:
        return s.recv(512).decode()
    except socket.timeout:
        sys.exit(f"ringctl: no reply from {addr} (asleep? off-mesh?)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="dashboard node name or IPv6 address")
    parser.add_argument("command", nargs="+")
    parser.add_argument("--device-name", help="inventory name required with a raw IPv6 mutation")
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    args = parser.parse_args()
    addr = resolve(args.target)
    auto_mode = (
        2 <= len(args.command) <= 6
        and args.command[0] == "mode"
        and args.command[1] in ("normal", "party", "ultra")
        and all(
            o.startswith(("blink=", "duty=", "wave=", "accent="))
            for o in args.command[2:]
        )
    )
    requested = list(args.command)
    # The probe->bid window is a read-modify-write race between operators:
    # the loser's command comes back result=rejected. One re-probe/re-bid
    # resolves it; a second loss is reported as the failure it is.
    for attempt in (1, 2):
        command = rewrite_mode(requested, probe_mode(addr)) if auto_mode else requested
        reply = send_command(args, parser, addr, " ".join(command))
        print(reply)
        if auto_mode and "result=rejected" in reply and attempt == 1:
            print("ringctl: lost a priority race — re-bidding", file=sys.stderr)
            continue
        break
    if "error=" in reply or "result=rejected" in reply:
        sys.exit(1)


if __name__ == "__main__":
    main()
