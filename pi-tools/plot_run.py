#!/usr/bin/env python3
"""
plot_run.py — plot a sync_test.py run directory (offset vs time + events).

Run on the workstation after copying a run dir back from the Pi:
    scp -r pi@example.net:sync-tests/<run> /tmp/ && \
    uv run --with matplotlib python plot_run.py /tmp/<run>

Writes <run>/plot.png: pairwise camera offset over time (ms), with harness
events (inject / skew_off / reboot_back) as vertical markers — both are on
the Pi's CLOCK_BOOTTIME axis, so they line up exactly.
"""

import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1].rstrip("/")
    with open(os.path.join(d, "summary.json")) as f:
        summary = json.load(f)
    events = []
    try:
        with open(os.path.join(d, "events.jsonl")) as f:
            events = [json.loads(ln) for ln in f if ln.strip()]
    except FileNotFoundError:
        pass

    fig, ax = plt.subplots(figsize=(11, 5))
    t0 = summary.get("t_start", 0)
    for pair in summary.get("offsets_vs_blob0", []):
        series = pair.get("series", [])
        if not series:
            continue
        ts = [p[0] - t0 for p in series]
        offs = [p[1] * 1000 for p in series]
        ax.plot(ts, offs, "o-", ms=3, lw=1, label=f"blob{pair['blob']} − blob0")

    marker_kinds = {
        "inject": "red",
        "skew_off": "darkorange",
        "reboot_back": "green",
        "flash_on": "gray",
    }
    seen = set()
    for ev in events:
        kind = ev.get("event")
        if kind not in marker_kinds or ev["t"] < t0:
            continue
        label = kind if kind not in seen else None
        seen.add(kind)
        ax.axvline(ev["t"] - t0, color=marker_kinds[kind], ls="--", lw=1, label=label)

    ax.axhspan(-10, 10, color="green", alpha=0.08, label="±10 ms")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("flash-onset offset (ms)")
    name = os.path.basename(d)
    ax.set_title(name)
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    out = os.path.join(d, "plot.png")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    print(out)


if __name__ == "__main__":
    main()
