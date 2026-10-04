#!/usr/bin/env python3
"""
sync_test.py — repeatable sync-protocol test scenarios, camera ground-truthed.

Runs ON the Pi (ring-gateway), next to flash_timing.py + ringctl.py. Puts the
rings in flash mode, runs the camera instrument for the whole scenario,
injects a disturbance at a known time, and writes one self-contained run
directory with everything needed to plot/compare later:

    ~/sync-tests/<stamp>-<scenario>/
        log.txt          flash_timing stdout (EDGE/STATUS lines)
        edges.jsonl      streamed edge events (crash-safe)
        summary.json     flash_timing summary incl. offset-vs-time series
        events.jsonl     what the harness did, when (CLOCK_BOOTTIME — the
                         same clock as the camera's SensorTimestamp)
        telemetry.jsonl  periodic `status` replies from each ring
        result.json      computed metrics (baseline / heal time / drift)

Scenarios:
  steady    — no disturbance; long hold. Metrics: median, MAD, max |dev|,
              drift slope.
      sync_test.py steady --duration 1800
  scramble  — phase-step one ring's clock, watch the heal.
      sync_test.py scramble --ms 500 --at 30 --duration 150
  skew      — bias one ring's clock rate (bad-crystal sim), watch the
              frequency loop compensate. Optional --off-at for recovery.
      sync_test.py skew --ppm 300 --at 30 --duration 300
  reboot    — cold-boot one ring, measure time back to flashing + locked.
              (Firmware re-enters pattern mode on boot; the harness
              re-sends `flash` once the ring answers again.)
      sync_test.py reboot --at 30 --duration 240

Notes:
  * blob ids are left-to-right in the camera frame; the ring↔blob mapping
    is recorded in events.jsonl only as "targets", so note the physical
    layout if signs matter.
  * needs the dashboard on :8080 for name→address resolution (ringctl).
"""

import argparse
import json
import math
import os
import pathlib
import queue
import signal
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import control_client
import thread_socket

import ringctl
from status_telemetry import parse_status

PORT = 4242
LOCK_S = 0.010  # camera-side "locked" threshold (firmware gate is 6 ms)
LOCK_RUN = 3  # consecutive locked pairs to call it healed
SUPPORTED_CAMERA_FPS = 30.0
CAMERA_DISCOVERY_TIMEOUT_S = 60.0


def boottime():
    return time.clock_gettime(time.CLOCK_BOOTTIME)


class Run:
    def __init__(self, outdir):
        self.dir = outdir
        os.makedirs(outdir, exist_ok=True)
        self.events = open(os.path.join(outdir, "events.jsonl"), "w", buffering=1)
        self.telemetry = open(os.path.join(outdir, "telemetry.jsonl"), "w", buffering=1)

    def event(self, kind, **kw):
        rec = {"t": boottime(), "event": kind, **kw}
        self.events.write(json.dumps(rec) + "\n")
        print(f"[{rec['t']:.3f}] {kind} {kw}", flush=True)
        return rec

    def close(self):
        self.events.close()
        self.telemetry.close()


def cmd(
    addr,
    text,
    timeout=5,
    *,
    device_name=None,
    control_key_dir=None,
    allow_legacy_control=False,
):
    if control_client.command_is_mutating(text) and not allow_legacy_control:
        if not device_name:
            raise RuntimeError("authenticated mutation requires an inventory device name")
        return control_client.authenticated_udp_command(
            addr,
            device_name,
            text,
            timeout,
            key_path=control_client.key_path_for(device_name, control_key_dir),
        )
    s = thread_socket.udp_socket(addr, timeout)
    try:
        s.sendto(text.encode(), (addr, PORT))
        return s.recv(256).decode()
    except (OSError, socket.timeout):
        return None
    finally:
        s.close()


def poll_status(run, rings):
    for name, addr in rings.items():
        reply = cmd(addr, "status", timeout=3)
        rec = {"t": boottime(), "ring": name}
        rec.update(parse_status(reply))
        run.telemetry.write(json.dumps(rec) + "\n")


def stop_camera(camera):
    if camera is None:
        return
    proc, reader = camera
    if proc.poll() is None:
        try:
            proc.send_signal(signal.SIGINT)
        except OSError:
            pass
        try:
            proc.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            try:
                proc.kill()
                proc.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
    reader.join(timeout=5)
    if proc.stdout is not None:
        try:
            proc.stdout.close()
        except OSError:
            pass


def start_camera(run, n_rings, fps, args_extra=(), discovery_timeout=None):
    log = open(os.path.join(run.dir, "log.txt"), "w", buffering=1)
    try:
        proc = subprocess.Popen(
            [
                sys.executable,
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "flash_timing.py"),
                "--duration",
                "0",
                "--expect",
                str(n_rings),
                "--fps",
                str(fps),
                "--jsonl",
                os.path.join(run.dir, "edges.jsonl"),
                "--json",
                os.path.join(run.dir, "summary.json"),
                *args_extra,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except Exception:
        log.close()
        raise
    output = queue.Queue()

    # Read in a worker so a quiet/stuck camera process cannot make readline()
    # defeat the discovery deadline. The same worker keeps draining after
    # discovery, preventing the child from filling its stdout pipe.
    def drain():
        try:
            for line in proc.stdout:
                log.write(line)
                output.put(line)
        finally:
            output.put(None)
            log.close()

    reader = threading.Thread(target=drain, name="camera-output", daemon=True)
    reader.start()
    camera = (proc, reader)

    blobs = 0
    timeout = CAMERA_DISCOVERY_TIMEOUT_S if discovery_timeout is None else discovery_timeout
    deadline = time.monotonic() + timeout
    measuring = False
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            line = output.get(timeout=remaining)
        except queue.Empty:
            break
        if line is None:
            break
        if line.startswith("# blob "):
            blobs += 1
        if line.startswith("# measuring"):
            measuring = True
            break
    if not measuring:
        stop_camera(camera)
        raise RuntimeError(
            f"sync_test: camera discovery never finished — rings flashing? (see {run.dir}/log.txt)"
        )
    if blobs != n_rings:
        run.event("blob_count_mismatch", found=blobs, expected=n_rings)
        stop_camera(camera)
        raise RuntimeError(
            f"sync_test: found {blobs} blobs, expected {n_rings} — check log.txt bboxes"
        )

    return camera


def analyze(run, scenario, t_inj):
    with open(os.path.join(run.dir, "summary.json")) as f:
        summary = json.load(f)
    result = {"scenario": scenario, "t_inject": t_inj, "pairs": []}
    for pair in summary.get("offsets_vs_blob0", []):
        series = pair.get("series", [])
        r = {
            "blob": pair["blob"],
            "n": len(series),
            "offset_s": pair["offset_s"],
            "jitter_s": pair["jitter_s"],
            "_offset_samples_s": [point[1] for point in series],
        }
        if series:
            ts = [p[0] for p in series]
            offs = [p[1] for p in series]
            if t_inj:
                pre = [o for t, o in series if t < t_inj]
                post = [(t, o) for t, o in series if t >= t_inj]
                r["baseline_med_s"] = median(pre)
                settle = median([o for _, o in post[-10:]])
                if settle is not None:
                    r["post_max_dev_s"] = max(abs(o - settle) for _, o in post)
                    streak = 0
                    heal = None
                    for t, o in post:
                        streak = streak + 1 if abs(o - settle) <= LOCK_S else 0
                        if streak >= LOCK_RUN:
                            heal = t - t_inj
                            break
                    r["heal_s"] = heal
                    r["settle_med_s"] = settle
            else:
                # steady run: drift slope via least squares
                n = len(series)
                mt, mo = sum(ts) / n, sum(offs) / n
                sxx = sum((t - mt) ** 2 for t in ts)
                sxy = sum((t - mt) * (o - mo) for t, o in series)
                r["drift_s_per_min"] = (sxy / sxx * 60) if sxx else None
                r["max_dev_s"] = max(abs(o - mo) for o in offs)
        result["pairs"].append(r)
    return result


def median(vals):
    if not vals:
        return None
    v = sorted(vals)
    return v[len(v) // 2]


def qualify_steady(
    result,
    duration_s,
    period_ms,
    min_pair_fraction,
    max_abs_offset_ms,
    min_offset_inlier_fraction,
    max_jitter_ms,
    max_abs_drift_ms_per_min,
):
    expected_pairs = duration_s / (period_ms / 1000.0)
    minimum_pairs = max(1, math.ceil(expected_pairs * min_pair_fraction))
    thresholds = {
        "minimum_pairs": minimum_pairs,
        "minimum_pair_fraction": min_pair_fraction,
        "max_abs_offset_ms": max_abs_offset_ms,
        "minimum_offset_inlier_fraction": min_offset_inlier_fraction,
        "max_jitter_ms": max_jitter_ms,
        "max_abs_drift_ms_per_min": max_abs_drift_ms_per_min,
    }
    qualified = []
    for pair in result.get("pairs", []):
        reasons = []
        if pair.get("n", 0) < minimum_pairs:
            reasons.append(f"paired flashes {pair.get('n', 0)} < {minimum_pairs}")
        checks = (
            ("offset_s", max_abs_offset_ms, "absolute offset"),
            ("jitter_s", max_jitter_ms, "jitter"),
            ("drift_s_per_min", max_abs_drift_ms_per_min, "absolute drift per minute"),
        )
        for field, limit_ms, label in checks:
            value = pair.get(field)
            if value is None:
                reasons.append(f"{label} is unavailable")
            elif abs(value * 1000.0) > limit_ms:
                reasons.append(f"{label} {abs(value * 1000.0):.3f}ms > {limit_ms:.3f}ms")
        offset_samples = pair.pop("_offset_samples_s", [])
        if offset_samples:
            inliers = sum(abs(value * 1000.0) <= max_abs_offset_ms for value in offset_samples)
            pair["offset_inlier_fraction"] = inliers / len(offset_samples)
        else:
            pair["offset_inlier_fraction"] = None
        inlier_fraction = pair["offset_inlier_fraction"]
        if inlier_fraction is None:
            reasons.append("offset inlier fraction is unavailable")
        elif inlier_fraction < min_offset_inlier_fraction:
            reasons.append(
                f"offset inlier fraction {inlier_fraction:.3f} < {min_offset_inlier_fraction:.3f}"
            )
        pair["pass"] = not reasons
        pair["reasons"] = reasons
        qualified.append(pair["pass"])
    result["thresholds"] = thresholds
    result["pass"] = bool(qualified) and all(qualified)
    if not qualified:
        result["reasons"] = ["no ring pairs were measured"]
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("scenario", choices=["steady", "scramble", "skew", "reboot"])
    ap.add_argument(
        "--rings",
        default="opal-reef-tide,feral-tide-light",
        help="comma-separated ring names (dashboard resolves)",
    )
    ap.add_argument(
        "--target", default=None, help="ring that receives the disturbance (default: last)"
    )
    ap.add_argument(
        "--period", type=int, default=1000, help="flash period ms (finer = more samples/s)"
    )
    ap.add_argument(
        "--fps",
        type=float,
        default=SUPPORTED_CAMERA_FPS,
        help="camera frame rate (default: 30; current OV64A40 setup is not reliable above 30)",
    )
    ap.add_argument("--duration", type=float, default=150, help="total measure seconds")
    ap.add_argument("--at", type=float, default=30, help="inject after this many seconds")
    ap.add_argument("--ms", type=int, default=500, help="scramble magnitude ms")
    ap.add_argument("--ppm", type=int, default=300, help="skew magnitude ppm")
    ap.add_argument(
        "--off-at",
        type=float,
        default=0,
        help="skew only: also send `skew off` at this second (0 = never)",
    )
    ap.add_argument("--poll", type=float, default=3, help="telemetry poll seconds")
    ap.add_argument("--min-pair-fraction", type=float, default=0.9)
    ap.add_argument("--max-abs-offset-ms", type=float, default=10.0)
    ap.add_argument("--min-offset-inlier-fraction", type=float, default=0.65)
    ap.add_argument("--max-jitter-ms", type=float, default=5.0)
    ap.add_argument("--max-abs-drift-ms-per-min", type=float, default=2.0)
    ap.add_argument(
        "--keep-flash", action="store_true", help="leave rings in flash mode afterwards"
    )
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--control-key-dir", type=pathlib.Path)
    ap.add_argument("--allow-legacy-control", action="store_true")
    args = ap.parse_args()
    if args.fps <= 0 or args.fps > SUPPORTED_CAMERA_FPS:
        ap.error(f"--fps must be greater than 0 and no more than {SUPPORTED_CAMERA_FPS:g}")
    if not 0 < args.min_pair_fraction <= 1:
        ap.error("--min-pair-fraction must be greater than 0 and no more than 1")
    if not 0 < args.min_offset_inlier_fraction <= 1:
        ap.error("--min-offset-inlier-fraction must be greater than 0 and no more than 1")
    if min(args.max_abs_offset_ms, args.max_jitter_ms, args.max_abs_drift_ms_per_min) <= 0:
        ap.error("steady-state timing thresholds must be greater than 0")

    names = [n.strip() for n in args.rings.split(",") if n.strip()]
    target = args.target or names[-1]
    if target not in names:
        sys.exit(f"sync_test: --target {target!r} not in --rings")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = args.outdir or os.path.expanduser(f"~/sync-tests/{stamp}-{args.scenario}")
    run = Run(outdir)
    print(f"run dir: {outdir}", flush=True)

    rings = {}
    cam = None
    injected = None
    skew_off_done = args.scenario != "skew" or args.off_at <= 0
    reboot_flash_restored = args.scenario != "reboot"
    try:
        for n in names:
            addr = ringctl.resolve(n)
            st = cmd(addr, "status")
            if st is None:
                raise RuntimeError(f"sync_test: {n} ({addr}) not answering")
            rings[n] = addr
            run.event("ring", name=n, addr=addr, status=st)

        run.event(
            "config",
            scenario=args.scenario,
            target=target,
            period=args.period,
            fps=args.fps,
            duration=args.duration,
            at=args.at,
            ms=args.ms,
            ppm=args.ppm,
            off_at=args.off_at,
        )

        # Make sure no disturbance is left over, then enter flash mode only
        # after the cleanup guard is active.
        for n, addr in rings.items():
            cmd(
                addr,
                "skew off",
                device_name=n,
                control_key_dir=args.control_key_dir,
                allow_legacy_control=args.allow_legacy_control,
            )
        for n, addr in rings.items():
            run.event(
                "flash_on",
                ring=n,
                reply=cmd(
                    addr,
                    f"flash {args.period}",
                    device_name=n,
                    control_key_dir=args.control_key_dir,
                    allow_legacy_control=args.allow_legacy_control,
                ),
            )

        cam = start_camera(run, len(rings), args.fps)
        run.event("camera_measuring")
        t0 = boottime()
        next_poll = 0.0
        while boottime() - t0 < args.duration:
            now = boottime() - t0
            if now >= next_poll:
                next_poll = now + args.poll
                poll_status(run, rings)
            if args.scenario != "steady" and injected is None and now >= args.at:
                addr = rings[target]
                if args.scenario == "scramble":
                    command = f"scramble {args.ms}"
                elif args.scenario == "skew":
                    command = f"skew {args.ppm}"
                else:
                    command = "reboot"
                r = cmd(
                    addr,
                    command,
                    device_name=target,
                    control_key_dir=args.control_key_dir,
                    allow_legacy_control=args.allow_legacy_control,
                )
                injected = boottime()
                run.event("inject", scenario=args.scenario, ring=target, reply=r)
            if not skew_off_done and now >= args.off_at:
                skew_off_done = True
                run.event(
                    "skew_off",
                    ring=target,
                    reply=cmd(
                        rings[target],
                        "skew off",
                        device_name=target,
                        control_key_dir=args.control_key_dir,
                        allow_legacy_control=args.allow_legacy_control,
                    ),
                )
            if injected and not reboot_flash_restored and boottime() - injected > 3:
                # ring is back once status answers again; re-arm flash mode
                r = cmd(rings[target], "status", timeout=2)
                if r:
                    reboot_flash_restored = True
                    run.event("reboot_back", ring=target, status=r)
                    run.event(
                        "flash_on",
                        ring=target,
                        reply=cmd(
                            rings[target],
                            f"flash {args.period}",
                            device_name=target,
                            control_key_dir=args.control_key_dir,
                            allow_legacy_control=args.allow_legacy_control,
                        ),
                    )
            time.sleep(0.5)
    finally:
        run.event("measure_end")
        stop_camera(cam)
        for n, addr in rings.items():
            cmd(
                addr,
                "skew off",
                device_name=n,
                control_key_dir=args.control_key_dir,
                allow_legacy_control=args.allow_legacy_control,
            )
            if not args.keep_flash:
                cmd(
                    addr,
                    "flash off",
                    device_name=n,
                    control_key_dir=args.control_key_dir,
                    allow_legacy_control=args.allow_legacy_control,
                )
        run.event("cleanup_done", flash_kept=args.keep_flash)
        run.close()

    result = analyze(run, args.scenario, injected)
    if args.scenario == "steady":
        result = qualify_steady(
            result,
            args.duration,
            args.period,
            args.min_pair_fraction,
            args.max_abs_offset_ms,
            args.min_offset_inlier_fraction,
            args.max_jitter_ms,
            args.max_abs_drift_ms_per_min,
        )
    else:
        for pair in result.get("pairs", []):
            pair.pop("_offset_samples_s", None)
    with open(os.path.join(run.dir, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result, indent=1))
    print(f"\nrun dir: {outdir}", flush=True)
    return 0 if result.get("pass", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
