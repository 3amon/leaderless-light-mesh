#!/usr/bin/env python3
"""
flash_timing.py — fast LED flash-timing measurement via the Pi camera.

Ground-truth instrument for the TIME-SYNC gossip protocol: point the camera
at one or more LED rings running thread-ring's flash mode (`flash <ms>` on
UDP :4242), and this measures each ring's flash onsets from camera sensor
timestamps — no reliance on what the nodes claim about their own clocks.

Pipeline (all numpy, no per-image files):
  1. discover: a few seconds of per-pixel max/min → flashing blobs via
     connected components; nearby fragments merged (one ring can split into
     2 blobs); with --expect N, the N top-most blobs win (specular
     reflections sit lower in the frame and lag 10-50 ms).
  2. measure: per frame, mean Y-plane brightness per blob → normalized
     series → hysteresis edge detector with sub-frame interpolation on the
     0.5 crossing. Edges stream to --jsonl as they happen (crash-safe for
     long runs).
  3. report: per-blob period + jitter, pairwise onset offsets (median ±
     MAD) AND the full offset-vs-time series — heal curves and drift plots
     come from that. JSON summary at the end (or on SIGINT/SIGTERM when
     running with --duration 0 = endless).

Timestamps are libcamera SensorTimestamp (CLOCK_BOOTTIME ns) — immune to
NTP steps, and directly comparable to time.clock_gettime(CLOCK_BOOTTIME)
stamps taken by an orchestrator on the same Pi (sync_test.py uses this to
mark injection times on the same axis). Rolling-shutter caveat: blobs at
different image rows carry a constant per-row readout offset (~µs/row); for
sub-ms absolute work, put rings at the same height or calibrate the
constant out. Relative drift, jitter, and heal dynamics are unaffected.

Usage:
  python3 flash_timing.py --duration 20
  python3 flash_timing.py --duration 0 --expect 2 \
      --jsonl /tmp/run/edges.jsonl --json /tmp/run/summary.json
"""

import argparse
import json
import signal
import sys
import time

import cv2
import numpy as np
from picamera2 import Picamera2


def log(*a):
    print(*a, flush=True)


def median_mad(vals):
    if not vals:
        return None, None
    v = np.asarray(vals, dtype=np.float64)
    med = float(np.median(v))
    mad = float(np.median(np.abs(v - med)))
    return med, mad


class Blob:
    def __init__(self, idx, bbox, mask, lo, hi):
        self.idx = idx
        x, y, w, h = bbox
        self.bbox = bbox
        self.sl = (slice(y, y + h), slice(x, x + w))
        self.mask = mask  # bool mask within bbox
        self.lo = lo  # brightness floor (off)
        self.hi = hi  # brightness peak (flash)
        self.cx = x + w / 2
        self.cy = y + h / 2
        self.state = "low"
        self.prev = None  # (t, norm)
        self.edges = []  # rising-edge times (s)
        self.gaps = []  # bracketing frame gap per edge (s)

    def mean(self, Y):
        return float(Y[self.sl][self.mask].mean())

    def feed(self, t, m):
        span = max(self.hi - self.lo, 1e-6)
        n = (m - self.lo) / span
        edge = None
        if self.prev is not None:
            t0, n0 = self.prev
            if self.state == "low" and n > 0.6:
                if n0 < 0.5 <= n and n != n0:
                    edge = t0 + (0.5 - n0) / (n - n0) * (t - t0)
                else:
                    edge = t
                self.state = "high"
                self.edges.append(edge)
                # bracketing frame gap: a dropped frame here means the
                # interpolation spans 2+ frame periods and the edge time
                # can be off by 1-2 frames (reads as a ±17/±33 ms
                # "excursion" that the clocks never had)
                self.gaps.append(t - t0)
            elif self.state == "high" and n < 0.4:
                self.state = "low"
        self.prev = (t, n)
        return edge


def merge_fragments(stats, gap):
    """Union bboxes whose gap-expanded rectangles overlap (a ring split by
    the diffuser reads as 2+ fragments). stats = [(x, y, w, h, area), ...]"""
    boxes = [list(s) for s in stats]
    merged = True
    while merged:
        merged = False
        out = []
        while boxes:
            x, y, w, h, a = boxes.pop(0)
            i = 0
            while i < len(boxes):
                x2, y2, w2, h2, a2 = boxes[i]
                if (
                    x - gap < x2 + w2
                    and x2 - gap < x + w
                    and y - gap < y2 + h2
                    and y2 - gap < y + h
                ):
                    nx, ny = min(x, x2), min(y, y2)
                    w = max(x + w, x2 + w2) - nx
                    h = max(y + h, y2 + h2) - ny
                    x, y, a = nx, ny, a + a2
                    boxes.pop(i)
                    merged = True
                else:
                    i += 1
            out.append([x, y, w, h, a])
        boxes = out
    return [tuple(b) for b in boxes]


def clean_edges(blob, max_gap):
    """Edges whose bracketing frame interval was normal — a dropped frame
    around the onset corrupts the interpolated time by 1-2 frame periods."""
    return [e for e, g in zip(blob.edges, blob.gaps) if g <= max_gap]


def pair_offsets(ref_edges, edges, max_dt=0.5):
    """Match each edge to the nearest ref edge → [(t, offset_s), ...]."""
    out = []
    if not ref_edges:
        return out
    ref = np.asarray(ref_edges)
    for e in edges:
        i = int(np.searchsorted(ref, e))
        cands = ref[max(0, i - 1) : i + 1]
        if not len(cands):
            continue
        d = e - float(cands[np.argmin(np.abs(cands - e))])
        if abs(d) < max_dt:  # same flash event
            out.append((e, d))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument(
        "--duration",
        type=float,
        default=20,
        help="measure seconds; 0 = endless (stop with SIGINT/SIGTERM)",
    )
    ap.add_argument("--discover", type=float, default=5, help="blob-discovery seconds")
    ap.add_argument(
        "--fps",
        type=float,
        default=30,
        help="camera frame rate (default: 30; current OV64A40 setup is not reliable above 30)",
    )
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument(
        "--exposure-us",
        type=int,
        default=0,
        help="0 = auto: 90%% of the frame period. Near-full-frame "
        "exposure is deliberate — a short exposure leaves the "
        "sensor blind most of each frame, and flash onsets "
        "landing in the blind zone quantize to whole frames "
        "(reads as ±17ms sync excursions that aren't real)",
    )
    ap.add_argument("--gain", type=float, default=2.5)
    ap.add_argument("--min-blob", type=int, default=25, help="min blob area px")
    ap.add_argument("--max-blobs", type=int, default=4)
    ap.add_argument(
        "--expect",
        type=int,
        default=0,
        help="number of real rings; extras (reflections, lower in "
        "frame) are dropped after fragment merge",
    )
    ap.add_argument(
        "--merge-px", type=int, default=30, help="merge blob fragments closer than this"
    )
    ap.add_argument(
        "--status-every", type=float, default=10, help="seconds between # STATUS lines (0 = off)"
    )
    ap.add_argument(
        "--jsonl", default=None, help="stream edges here as JSON lines (crash-safe log)"
    )
    ap.add_argument(
        "--json", default="/tmp/ring-timing.json", help="summary JSON path (dashboard reads this)"
    )
    args = ap.parse_args()
    if args.fps <= 0 or args.fps > 30:
        ap.error("--fps must be greater than 0 and no more than 30")

    stop = {"flag": False}

    def on_sig(signum, frame):
        stop["flag"] = True
        log(f"# signal {signum} → finishing up")

    signal.signal(signal.SIGTERM, on_sig)
    signal.signal(signal.SIGINT, on_sig)

    jout = open(args.jsonl, "w", buffering=1) if args.jsonl else None

    def jline(obj):
        if jout:
            jout.write(json.dumps(obj) + "\n")

    # the dashboard's snapshot worker holds the camera ~1.5s of every 6 —
    # retry acquisition instead of dying if we start inside that window
    for attempt in range(15):
        try:
            picam2 = Picamera2()
            break
        except RuntimeError as e:
            if attempt == 14:
                raise
            log(f"# camera busy ({e}); retrying in 2s")
            time.sleep(2)
    us = int(1e6 / args.fps)
    if args.exposure_us <= 0:
        args.exposure_us = int(us * 0.9)
    cfg = picam2.create_video_configuration(
        main={"size": (args.width, args.height), "format": "YUV420"},
        controls={
            "FrameDurationLimits": (us, us),
            "AeEnable": False,
            "AwbEnable": False,
            "ExposureTime": args.exposure_us,
            "AnalogueGain": args.gain,
        },
    )
    picam2.configure(cfg)
    picam2.start()
    time.sleep(0.3)
    H = args.height

    def grab():
        req = picam2.capture_request()
        try:
            arr = req.make_array("main")
            ts = req.get_metadata().get("SensorTimestamp", 0) / 1e9
        finally:
            req.release()
        return ts, arr[:H, :]  # Y plane

    # ---- discovery: per-pixel extremes catch the flashing regions ----
    log(f"# discovering blobs for {args.discover}s (rings must be in flash mode)")
    t0, Y = grab()
    mx = Y.astype(np.float32).copy()
    mn = Y.astype(np.float32).copy()
    frames = 1
    while True:
        t, Y = grab()
        np.maximum(mx, Y, out=mx)
        np.minimum(mn, Y, out=mn)
        frames += 1
        if t - t0 > args.discover or stop["flag"]:
            break
    diff = mx - mn
    peak = float(diff.max())
    log(f"# discovery: {frames} frames, peak flash delta {peak:.0f}")
    if peak < 25:
        log(
            "# WARNING: no strong flashing found — is flash mode on? "
            "Falling back to whole-frame ROI."
        )
        mask_full = np.ones_like(diff, dtype=np.uint8)
        stats = [(0, 0, diff.shape[1], diff.shape[0], diff.size)]
        labels = mask_full
    else:
        mask_full = (diff > max(20.0, 0.4 * peak)).astype(np.uint8)
        n, labels, st, _ = cv2.connectedComponentsWithStats(mask_full)
        stats = [tuple(st[i][:5]) for i in range(1, n)]
        stats = [s for s in stats if s[4] >= args.min_blob]
        pre = len(stats)
        stats = merge_fragments(stats, args.merge_px)
        if len(stats) != pre:
            log(f"# merged {pre} fragments → {len(stats)} blobs")
        stats.sort(key=lambda s: -s[4])
        stats = stats[: args.max_blobs]
        if args.expect and len(stats) > args.expect:
            stats.sort(key=lambda s: s[1] + s[3] / 2)  # centroid row, top first
            for s in stats[args.expect :]:
                log(
                    f"# dropping low blob at ({s[0]},{s[1]},{s[2]}x{s[3]}) — "
                    f"reflection? (--expect {args.expect})"
                )
            stats = stats[: args.expect]
        stats.sort(key=lambda s: s[0])  # left-to-right = stable ids
    if not stats:
        log("# ERROR: no blobs found")
        sys.exit(1)

    blobs = []
    for i, (x, y, w, h, area) in enumerate(stats):
        if peak < 25:
            m = np.ones((h, w), dtype=bool)
        else:
            m = mask_full[y : y + h, x : x + w] > 0
        lo = float(mn[y : y + h, x : x + w][m].mean())
        hi = float(mx[y : y + h, x : x + w][m].mean())
        b = Blob(i, (int(x), int(y), int(w), int(h)), m, lo, hi)
        blobs.append(b)
        log(
            f"# blob {i}: bbox=({x},{y},{w}x{h}) area={area} "
            f"lo={lo:.0f} hi={hi:.0f} centroid=({b.cx:.0f},{b.cy:.0f})"
        )
        jline(
            {
                "type": "blob",
                "blob": i,
                "bbox": [int(x), int(y), int(w), int(h)],
                "centroid": [b.cx, b.cy],
                "lo": lo,
                "hi": hi,
            }
        )

    # ---- measurement ----
    log(
        f"# measuring {'endlessly' if args.duration <= 0 else f'for {args.duration}s'}"
        f" at target {args.fps} fps"
    )
    t_start = None
    next_status = None
    nframes = 0
    while not stop["flag"]:
        t, Y = grab()
        if t_start is None:
            t_start = t
            next_status = t + args.status_every
        nframes += 1
        for b in blobs:
            e = b.feed(t, b.mean(Y))
            if e is not None:
                g = b.gaps[-1]
                log(f"EDGE blob={b.idx} t={e:.6f} gap={g * 1000:.1f}ms")
                jline({"type": "edge", "blob": b.idx, "t": e, "gap": round(g, 6)})
        if args.status_every > 0 and t >= next_status:
            next_status = t + args.status_every
            counts = " ".join(f"b{b.idx}={len(b.edges)}" for b in blobs)
            log(f"# STATUS t={t - t_start:.0f}s frames={nframes} edges: {counts}")
        if args.duration > 0 and t - t_start > args.duration:
            break
    fps_real = nframes / max(t - t_start, 1e-9)
    log(f"# {nframes} frames in {t - t_start:.1f}s = {fps_real:.1f} fps")

    # ---- analysis ----
    summary = {"fps": fps_real, "t_start": t_start, "t_end": t, "blobs": []}
    for b in blobs:
        periods = list(np.diff(b.edges)) if len(b.edges) > 1 else []
        pmed, pmad = median_mad(periods)
        entry = {
            "blob": b.idx,
            "edges": len(b.edges),
            "period_s": pmed,
            "period_jitter_s": pmad,
            "centroid": [b.cx, b.cy],
        }
        summary["blobs"].append(entry)
        log(
            f"BLOB {b.idx}: {len(b.edges)} flashes, "
            f"period={pmed and f'{pmed * 1000:.2f}ms'} "
            f"jitter(MAD)={pmad and f'{pmad * 1000:.2f}ms'}"
        )

    # pairwise onset offsets vs blob 0 — THE sync-quality number, plus the
    # full time series (heal curves / drift live there). Edges bracketed by
    # a dropped frame are excluded from matching (their interpolated time is
    # off by 1-2 frame periods) and accounted for per blob.
    max_gap = 1.5 / args.fps
    for b in blobs:
        dropped = len(b.edges) - len(clean_edges(b, max_gap))
        summary["blobs"][b.idx]["edges_frame_gap_excluded"] = dropped
        if dropped:
            log(
                f"# blob {b.idx}: {dropped} edges excluded "
                f"(bracketing frame gap > {max_gap * 1000:.1f}ms — dropped frames)"
            )
    ref = blobs[0]
    summary["offsets_vs_blob0"] = []
    for b in blobs[1:]:
        series = pair_offsets(clean_edges(ref, max_gap), clean_edges(b, max_gap))
        deltas = [d for _, d in series]
        omed, omad = median_mad(deltas)
        summary["offsets_vs_blob0"].append(
            {
                "blob": b.idx,
                "n": len(deltas),
                "offset_s": omed,
                "jitter_s": omad,
                "series": [[round(t_, 6), round(d, 6)] for t_, d in series],
            }
        )
        if omed is not None:
            log(
                f"OFFSET blob{b.idx}-blob0: {omed * 1000:+.2f}ms "
                f"± {omad * 1000:.2f}ms (n={len(deltas)})"
            )

    if args.json:
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=1)
        log(f"# summary written to {args.json}")
    if jout:
        jline({"type": "summary", **{k: summary[k] for k in ("fps", "t_start", "t_end")}})
        jout.close()
    picam2.stop()


if __name__ == "__main__":
    main()
