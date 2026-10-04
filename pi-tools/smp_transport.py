#!/usr/bin/env python3
"""Machine-readable SMP transport used by deploy.py.

Run this with the Python interpreter that owns smpclient (normally the
interpreter in smpmgr's shebang).  Stdout is exactly one JSON document; logs
and tracebacks go to stderr.  Keeping this small isolates the optional SMP
dependency from the deployment state machine and its host-only tests.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import pathlib
import sys
from collections.abc import AsyncIterator

from smp_retry import retry_timeout
from smpclient import SMPClient
from smpclient.generics import error, success
from smpclient.requests.image_management import (
    ImageErase,
    ImageStatesRead,
    ImageUploadWrite,
)
from smpclient.transport.udp import Addr, SMPUDPTransport
from thread_socket import stable_source_address

from safe_diagnostics import summarize

DEFAULT_SMP_MTU = 192


class StableSourceSMPUDPTransport(SMPUDPTransport):
    """Bind SMP to the same routable OMR source as the UDP control tools."""

    async def connect(self, address: str, timeout_s: float, port: int = 1337) -> None:
        source = stable_source_address(address)

        if source is None:
            await super().connect(address, timeout_s, port)
            return
        await asyncio.wait_for(
            self._client.connect(
                Addr(host=address, port=port),
                Addr(host=source, port=0),
            ),
            timeout_s,
        )
        self._is_ipv6 = True


def image_dict(image) -> dict[str, object]:
    return {
        "slot": int(image.slot),
        "version": str(image.version),
        "hash": bytes(image.hash).hex().upper(),
        "bootable": bool(image.bootable),
        "pending": bool(image.pending),
        "confirmed": bool(image.confirmed),
        "active": bool(image.active),
        "permanent": bool(image.permanent),
    }


def response_error(response) -> str:
    return json.dumps(summarize(response), sort_keys=True)


async def request_with_retries(
    client: SMPClient,
    request,
    *,
    timeout_s: float,
    attempts: int,
    offset: int,
):
    """Retry the exact same idempotent upload request after UDP loss."""

    async def operation():
        return await client.request(request, timeout_s=timeout_s)

    def report_retry(attempt: int) -> None:
        print(
            json.dumps(
                {
                    "event": "chunk_retry",
                    "offset": offset,
                    "attempt": attempt,
                },
                sort_keys=True,
            ),
            file=sys.stderr,
            flush=True,
        )

    return await retry_timeout(operation, attempts=attempts, on_retry=report_retry)


def upload_offset(response, image_size: int) -> int:
    if error(response):
        raise RuntimeError(f"upload request failed: {response_error(response)}")
    if not success(response):
        raise RuntimeError("upload request returned an unknown response")
    if response.off is None:
        raise RuntimeError(f"upload response has no offset: {response_error(response)}")
    offset = int(response.off)
    if not 0 <= offset <= image_size:
        raise RuntimeError(f"upload response offset {offset} exceeds image size {image_size}")
    return offset


async def resilient_upload(
    client: SMPClient,
    image: bytes,
    *,
    first_timeout_s: float,
    subsequent_timeout_s: float,
    chunk_attempts: int,
) -> AsyncIterator[int]:
    """Upload while retrying a lost UDP request without resetting its sequence."""
    request = client._maximize_image_upload_write_packet(
        ImageUploadWrite(
            off=0,
            data=b"",
            image=0,
            len=len(image),
            sha=hashlib.sha256(image).digest(),
            upgrade=False,
        ),
        image,
    )
    response = await request_with_retries(
        client,
        request,
        timeout_s=first_timeout_s,
        attempts=chunk_attempts,
        offset=0,
    )
    offset = upload_offset(response, len(image))
    yield offset

    while offset != len(image):
        request = client._maximize_image_upload_write_packet(
            ImageUploadWrite(
                off=offset,
                data=b"",
                len=len(image) if offset == 0 else None,
                image=0 if offset == 0 else None,
                upgrade=False if offset == 0 else None,
            ),
            image,
        )
        response = await request_with_retries(
            client,
            request,
            timeout_s=subsequent_timeout_s,
            attempts=chunk_attempts,
            offset=offset,
        )
        offset = upload_offset(response, len(image))
        yield offset

    match = getattr(response, "match", None)
    if match is not None and match is not True:
        raise RuntimeError(f"upload hash mismatch: {response_error(response)}")


async def run(args: argparse.Namespace) -> dict[str, object]:
    client = SMPClient(
        StableSourceSMPUDPTransport(mtu=args.mtu),
        args.address,
        timeout_s=args.timeout,
    )
    await client.connect(args.timeout)
    try:
        if args.command == "state":
            response = await client.request(ImageStatesRead(), timeout_s=args.timeout)
            if error(response):
                raise RuntimeError(f"state request failed: {response_error(response)}")
            if not success(response):
                raise RuntimeError("state request returned an unknown response")
            return {"ok": True, "images": [image_dict(image) for image in response.images]}

        if args.command == "erase":
            response = await client.request(ImageErase(slot=args.slot), timeout_s=args.timeout)
            if error(response):
                raise RuntimeError(f"erase request failed: {response_error(response)}")
            if not success(response):
                raise RuntimeError("erase request returned an unknown response")
            return {"ok": True, "slot": args.slot}

        if args.command == "upload":
            image = args.image.read_bytes()
            final_offset = 0
            async for offset in resilient_upload(
                client,
                image,
                first_timeout_s=args.first_timeout,
                subsequent_timeout_s=args.timeout,
                chunk_attempts=args.chunk_attempts,
            ):
                final_offset = offset
                if args.progress:
                    print(
                        json.dumps(
                            {"event": "progress", "offset": offset, "total": len(image)},
                            sort_keys=True,
                        ),
                        file=sys.stderr,
                        flush=True,
                    )
            if final_offset != len(image):
                raise RuntimeError(f"upload ended at offset {final_offset}, expected {len(image)}")
            return {"ok": True, "offset": final_offset, "total": len(image)}

        raise RuntimeError(f"unknown command {args.command!r}")
    finally:
        await client.disconnect()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument(
        "--mtu",
        type=int,
        default=DEFAULT_SMP_MTU,
        help="UDP MTU (192 limits 802.15.4 fragmentation on Thread)",
    )
    parser.add_argument("--timeout", type=float, default=15.0)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("state")
    erase = sub.add_parser("erase")
    erase.add_argument("slot", type=int)
    upload = sub.add_parser("upload")
    upload.add_argument("image", type=pathlib.Path)
    upload.add_argument("--first-timeout", type=float, default=45.0)
    upload.add_argument(
        "--chunk-attempts",
        type=int,
        default=3,
        help="attempts for the same upload request before reconnecting",
    )
    upload.add_argument("--progress", action="store_true")
    args = parser.parse_args()
    if not 1 <= getattr(args, "chunk_attempts", 1) <= 8:
        parser.error("--chunk-attempts must be between 1 and 8")
    return args


def main() -> int:
    try:
        result = asyncio.run(run(parse_args()))
    except Exception as exc:
        print(f"smp_transport: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
