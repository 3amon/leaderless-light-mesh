#!/usr/bin/env python3
"""Optically verify that a rejoin restart is seamless.

Telemetry can prove the ring rebooted and that the backoff escalated. It cannot
prove the thing the change was actually for: that the restart is invisible. That
has only ever been checkable by a human watching the ring, and two regressions
got shipped because of it -- an early white boot marker, and the attach
crossfade decaying to amber a couple of seconds after the ring came back.

So the ring's own light is the assertion. Frames are captured across an injected
heal, and the sequence must show:

  * a fade DOWN to darkness over roughly the fade-out period
  * a dark window while the node reboots
  * a fade UP afterwards
  * no white frame and no red frame at any point -- the boot marker, the white
    boot chase, and the fault double-pulse are all bright and low-saturation or
    bright and red, so any of them reappearing fails this test

The analysis is a pure function over (timestamp, mean R, mean G, mean B)
samples, so it is unit-tested against synthetic sequences rather than requiring
a camera and a darkened room to change.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

# Fractions of the brightest observed frame.
#
# "Dark" is deliberately not near-zero. Per-pixel background subtraction leaves
# a small residual -- different pixels bottom out on different frames, so the
# floor of a complete fade measures around 14 percent rather than 0. A ring that
# is off and a ring that is rendering are separated by roughly 6x, so this sits
# comfortably between them.
DARK_FRACTION = 0.25          # at or below this counts as "dark"
LIT_FRACTION = 0.55           # at or above this counts as "lit"

# A forbidden indicator must persist. The white marker lasts the whole boot, the
# white chase 2.4 s and the fault pulse 800 ms, so anything real spans many
# frames; a lone frame is noise or a transition sample.
MIN_INDICATOR_FRAMES = 3

# A white frame: bright, and all three channels close together.
WHITE_MIN_LEVEL = 0.45        # fraction of peak
WHITE_MAX_SPREAD = 0.16       # (max channel - min channel) / max channel

# Region-of-interest discovery. The ring is whatever changed during the fade.
DECIMATE = 8                  # 640x480 -> 80x60 per stored frame
MIN_ROI_SPREAD = 8.0          # levels; below this nothing meaningfully moved

# A red frame: bright, red dominant, and green+blue both well below it.
RED_MIN_LEVEL = 0.30
RED_DOMINANCE = 1.8           # red must exceed this multiple of max(g, b)


class Sample(tuple):
    """(t_seconds, r, g, b) with 0..255 channel means."""

    __slots__ = ()

    def __new__(cls, t, r, g, b):
        return super().__new__(cls, (float(t), float(r), float(g), float(b)))

    @property
    def t(self):
        return self[0]

    @property
    def level(self):
        return max(self[1], self[2], self[3])


def classify_frame(sample: Sample, peak: float) -> str | None:
    """Return 'white'/'red' if this frame is a forbidden indicator, else None."""
    if peak <= 0:
        return None
    r, g, b = sample[1], sample[2], sample[3]
    level = sample.level / peak
    channel_max = max(r, g, b)
    if channel_max <= 0:
        return None
    spread = (channel_max - min(r, g, b)) / channel_max

    if level >= WHITE_MIN_LEVEL and spread <= WHITE_MAX_SPREAD:
        return "white"
    if level >= RED_MIN_LEVEL and r > RED_DOMINANCE * max(g, b):
        return "red"
    return None


def analyse_fade(samples: list[Sample]) -> dict:
    """Verdict on a captured restart. Pure: no camera, no clock, no I/O."""
    result = {
        "passed": False,
        "failures": [],
        "peak": 0.0,
        "fade_out_s": None,
        "dark_s": None,
        "fade_in_s": None,
        "forbidden": [],
    }
    if len(samples) < 8:
        result["failures"].append(f"too few samples ({len(samples)})")
        return result

    peak = max(s.level for s in samples)
    result["peak"] = round(peak, 2)
    if peak <= 0:
        result["failures"].append("no light captured at all")
        return result

    dark_level = peak * DARK_FRACTION
    lit_level = peak * LIT_FRACTION

    # Forbidden indicators anywhere in the window, as sustained runs.
    run_kind: str | None = None
    run: list[Sample] = []

    def flush_run():
        if run_kind and len(run) >= MIN_INDICATOR_FRAMES:
            result["forbidden"].append({
                "kind": run_kind,
                "from_t": round(run[0].t, 3),
                "to_t": round(run[-1].t, 3),
                "frames": len(run),
                "rgb": [round(run[0][1]), round(run[0][2]), round(run[0][3])],
            })

    for s in samples:
        kind = classify_frame(s, peak)
        if kind == run_kind:
            run.append(s)
            continue
        flush_run()
        run_kind, run = kind, ([s] if kind else [])
    flush_run()

    if result["forbidden"]:
        kinds = sorted({f["kind"] for f in result["forbidden"]})
        result["failures"].append(
            f"forbidden indicator(s) during restart: {', '.join(kinds)} "
            f"({len(result['forbidden'])} frame(s))"
        )

    # Structure: lit -> dark -> lit.
    lit_before = next((i for i, s in enumerate(samples) if s.level >= lit_level), None)
    if lit_before is None:
        result["failures"].append("ring was never lit before the restart")
        return result

    dark_start = next(
        (i for i in range(lit_before, len(samples)) if samples[i].level <= dark_level),
        None,
    )
    if dark_start is None:
        result["failures"].append("ring never went dark: no restart observed")
        return result

    dark_end = next(
        (i for i in range(dark_start, len(samples)) if samples[i].level > dark_level),
        None,
    )
    if dark_end is None:
        result["failures"].append("ring never came back up")
        return result

    lit_after = next(
        (i for i in range(dark_end, len(samples)) if samples[i].level >= lit_level),
        None,
    )
    if lit_after is None:
        result["failures"].append("ring came back but never reached full brightness")
        return result

    # Time the fade from the LAST fully-lit frame, not the first one seen, or
    # the measurement swallows however long the ring sat lit beforehand.
    #
    # These spans are threshold crossings (lit -> dark), so they are SHORTER
    # than the true ramp: the head above `lit_level` and the tail below
    # `dark_level` are both clipped. They are for confirming a gradual,
    # correctly-ordered transition, not for measuring the ramp constants --
    # compare them to each other rather than to the firmware's timings.
    last_lit = max(
        i for i in range(lit_before, dark_start) if samples[i].level >= lit_level
    )

    result["fade_out_s"] = round(samples[dark_start].t - samples[last_lit].t, 3)
    result["dark_s"] = round(samples[dark_end].t - samples[dark_start].t, 3)
    result["fade_in_s"] = round(samples[lit_after].t - samples[dark_end].t, 3)

    # A fade, not a cut: the transition must span several frames. A hard cut to
    # black would show one sample between lit and dark.
    if dark_start - last_lit < 3:
        result["failures"].append(
            f"cut rather than fade on the way out ({dark_start - last_lit} frames)"
        )
    if lit_after - dark_end < 3:
        result["failures"].append(
            f"cut rather than fade on the way back ({lit_after - dark_end} frames)"
        )

    result["passed"] = not result["failures"]
    return result


# ---------------------------------------------------------------- capture ---

def capture(address: str, device: str, seconds: float, fps: int,
            trigger, exposure_us: int, gain: float, out_dir: pathlib.Path | None):
    """Capture mean RGB per frame while `trigger` fires the restart."""
    from picamera2 import Picamera2  # imported lazily: only present on the Pi
    import numpy as np

    # The dashboard snapshots every six seconds, so the camera may be taken at
    # any point in the open/configure/start sequence -- not just at open, which
    # is why retrying only the constructor left a half-initialised camera that
    # then failed at start(). Retry the whole sequence and release in between.
    frame_us = int(1e6 / fps)
    picam2 = None
    for attempt in range(15):
        try:
            picam2 = Picamera2()
            picam2.configure(picam2.create_video_configuration(
                main={"size": (640, 480), "format": "RGB888"},
                controls={
                    "FrameDurationLimits": (frame_us, frame_us),
                    "AeEnable": False,   # fixed exposure, or the camera
                    "AwbEnable": False,  # "corrects" the fade we are measuring
                    "ExposureTime": exposure_us,
                    "AnalogueGain": gain,
                },
            ))
            picam2.start()
            break
        except RuntimeError as exc:
            if picam2 is not None:
                try:
                    picam2.close()
                except Exception:  # noqa: BLE001 - already failing; keep going
                    pass
                picam2 = None
            if attempt == 14:
                raise
            print(f"# camera busy ({exc}); retrying", flush=True)
            time.sleep(2)
    time.sleep(0.4)

    # Keep a decimated copy of every frame. Averaging the WHOLE frame does not
    # work: the ring occupies a small part of it, so ambient light swamps the
    # fade entirely -- a first run measured a 7% dip against a 100% fade. The
    # region of interest is found afterwards, from the footage itself.
    stack: list = []
    times: list[float] = []
    started = time.monotonic()
    fired = False
    try:
        while time.monotonic() - started < seconds:
            request = picam2.capture_request()
            try:
                frame = request.make_array("main")
                small = frame[::DECIMATE, ::DECIMATE].astype(np.float32)
            finally:
                request.release()
            times.append(time.monotonic() - started)
            stack.append(small)

            if not fired and times[-1] >= 1.0:
                fired = True
                trigger()
    finally:
        picam2.stop()

    frames = np.stack(stack)                       # (n, h, w, 3), BGR
    luma = frames.max(axis=3)                      # per-pixel level over time
    spread = luma.max(axis=0) - luma.min(axis=0)   # what actually changed
    peak_spread = float(spread.max())

    # Subtract the per-pixel darkest value seen. Room light does not switch off
    # with the ring, so without this the floor of a full fade sits at ambient --
    # measured at 41 percent of peak, which no "is it dark" threshold can call
    # dark. What remains is the ring's own contribution, which really does reach
    # zero, and is free of the ambient colour cast that would otherwise skew the
    # white/red classification.
    frames = np.clip(frames - frames.min(axis=0), 0, None)

    if peak_spread < MIN_ROI_SPREAD:
        # Nothing in view changed materially. Say so rather than analysing
        # ambient light and reporting a confident, meaningless verdict.
        mask = np.ones_like(spread, dtype=bool)
        roi_note = (f"no region changed by more than {peak_spread:.1f} levels; "
                    "is the ring in frame and lit?")
    else:
        mask = spread >= max(MIN_ROI_SPREAD, 0.5 * peak_spread)
        roi_note = None

    pixels = int(mask.sum())
    selected = frames[:, mask, :]                  # (n, pixels, 3)
    means = selected.mean(axis=1)                  # (n, 3) BGR
    samples = [Sample(t, m[2], m[1], m[0]) for t, m in zip(times, means)]

    diagnostics = {
        "roi_pixels": pixels,
        "roi_fraction": round(pixels / spread.size, 4),
        "peak_spread": round(peak_spread, 1),
        "note": roi_note,
    }

    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "samples.jsonl").write_text(
            "".join(json.dumps({"t": s.t, "r": s[1], "g": s[2], "b": s[3]}) + "\n"
                    for s in samples)
        )
        (out_dir / "roi.json").write_text(json.dumps(diagnostics, indent=2))
    print("# roi: " + json.dumps(diagnostics), flush=True)
    return samples


def device_boot(address: str) -> int | None:
    """Current boot ordinal, or None if the ring did not answer."""
    import socket as _socket

    sock = _socket.socket(_socket.AF_INET6, _socket.SOCK_DGRAM)
    sock.settimeout(5)
    try:
        sock.sendto(b"status", (address, 4242))
        raw = sock.recvfrom(512)[0].decode(errors="replace")
    except OSError:
        return None
    finally:
        sock.close()
    for field in raw.split():
        if field.startswith("boot="):
            head = field[5:].split("/")[0]
            return int(head) if head.isdigit() else None
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--seconds", type=float, default=45.0)
    parser.add_argument("--fps", type=int, default=30)
    # Low by default: a lit ring clips at these ranges, and a clipped fade has
    # a flat top that hides its own shape.
    parser.add_argument("--exposure-us", type=int, default=3000)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--samples", type=pathlib.Path,
                        help="analyse a previously captured samples.jsonl instead")
    args = parser.parse_args(argv)

    if args.samples:
        samples = [
            Sample(d["t"], d["r"], d["g"], d["b"])
            for d in (json.loads(line) for line in
                      args.samples.read_text().splitlines() if line.strip())
        ]
    else:
        import control_client

        def trigger():
            for text in ("heal alone 1", "heal fire"):
                control_client.authenticated_udp_command(
                    args.address, args.target, text, args.timeout)

        before = device_boot(args.address)
        samples = capture(args.address, args.target, args.seconds, args.fps,
                          trigger, args.exposure_us, args.gain,
                          args.out.parent if args.out else None)
        after = device_boot(args.address)
        try:
            import control_client as cc
            cc.authenticated_udp_command(args.address, args.target,
                                         "heal clear", args.timeout)
        except Exception as exc:  # noqa: BLE001 - cleanup must not mask results
            print(f"# warning: could not disarm injection: {exc}", flush=True)

    verdict = analyse_fade(samples)
    verdict["samples"] = len(samples)
    if not args.samples:
        verdict["boot_before"], verdict["boot_after"] = before, after
        if before is not None and after is not None and after <= before:
            # Distinguish "the fade looked wrong" from "nothing ever restarted".
            verdict["passed"] = False
            verdict["failures"].append(
                f"no reboot occurred during capture (boot stayed {before})")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict, indent=2))
    return 0 if verdict["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
