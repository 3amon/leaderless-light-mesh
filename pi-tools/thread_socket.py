#!/usr/bin/env python3
"""IPv6 UDP sockets with stable Thread border-router source selection.

OpenThread may retain partition-specific RLOC/ALOC addresses on ``wpan0``.
After a partition change, Linux can select one of those transient locators as
the source for host-originated traffic even though the rings cannot route a
reply to it. Prefer the border router's ordinary OMR address in the
destination /64. Fall back to normal kernel selection off the Pi or when no
safe matching address exists.
"""

from __future__ import annotations

import ipaddress
import pathlib
import socket

IF_INET6 = pathlib.Path("/proc/net/if_inet6")
THREAD_INTERFACE = "wpan0"
THREAD_LOCATOR_PREFIX = bytes.fromhex("000000fffe00")


def stable_source_address(
    remote: str,
    *,
    interface: str = THREAD_INTERFACE,
    address_table: pathlib.Path = IF_INET6,
) -> str | None:
    try:
        destination = ipaddress.IPv6Address(remote)
    except ipaddress.AddressValueError:
        return None
    destination_prefix = destination.packed[:8]
    candidates: list[ipaddress.IPv6Address] = []

    try:
        lines = address_table.read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        fields = line.split()
        if len(fields) != 6 or fields[5] != interface:
            continue
        try:
            candidate = ipaddress.IPv6Address(int(fields[0], 16))
        except (ValueError, ipaddress.AddressValueError):
            continue
        if candidate.packed[:8] != destination_prefix:
            continue
        if candidate.packed[8:14] == THREAD_LOCATOR_PREFIX:
            continue
        candidates.append(candidate)
    return str(sorted(candidates)[0]) if candidates else None


def udp_socket(remote: str, timeout: float) -> socket.socket:
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    source = stable_source_address(remote)

    if source is not None:
        sock.bind((source, 0))
    sock.settimeout(timeout)
    return sock
