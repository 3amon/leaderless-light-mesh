"""Parsing and threshold evaluation for bounded ring status telemetry."""

import re

FIELDS = {
    "uptime_s": r"up=(\d+)s",
    "state": r"state=([a-z]+)",
    "theta_ms": r"theta=(-?\d+)ms",
    "ppm": r"ppm=(-?\d+)",
    "peer_age_ms": r"age=(-?\d+)ms",
    "freshness_lapses": r"lapse=(\d+)",
    "sync_profile": r"sched=([a-z]+)",
    "ota_quiet_s": r"quiet=(\d+)s",
    "network_mode": r"mode=(normal|party|ultra)/\d+",
    "mode_priority": r"mode=(?:normal|party|ultra)/(\d+)",
    "infrastructure": r"infra=(\d+)",
    "usb_present": r"usb=([01])",
    "usb_quiet": r"usbquiet=([01])",
    "pattern": r"pat=(-?\d+)",
    "boot_sequence": r"boot=(\d+)/",
}


def parse_status(reply):
    """Return known fields from old or current `status` replies."""
    parsed = {"reply": reply}
    if not reply:
        return parsed
    parsed["name"] = reply.split(maxsplit=1)[0]
    for key, pattern in FIELDS.items():
        match = re.search(pattern, reply)
        if match:
            value = match.group(1)
            parsed[key] = value if key in ("state", "sync_profile", "network_mode") else int(value)

    match = re.search(r"nbr=(\d+)/(\d+)(?:/(\d+))?", reply)
    if match:
        parsed["locked_peers"] = int(match.group(1))
        parsed["fresh_peers"] = int(match.group(2))
        if match.group(3) is not None:
            parsed["tracked_peers"] = int(match.group(3))
    return parsed


def evaluate_freshness(
    records,
    ring_names,
    expected_fresh,
    max_stale_samples=0,
    max_missing_samples=0,
    max_lapse_delta=0,
):
    """Summarize a soak and apply explicit per-ring freshness thresholds."""
    result = {"pass": True, "thresholds": {}, "rings": {}}
    result["thresholds"] = {
        "expected_fresh": expected_fresh,
        "max_stale_samples": max_stale_samples,
        "max_missing_samples": max_missing_samples,
        "max_lapse_delta": max_lapse_delta,
    }
    for name in ring_names:
        samples = [record for record in records if record.get("ring") == name]
        valid = [record for record in samples if record.get("reply")]
        lapses = [record["freshness_lapses"] for record in valid if "freshness_lapses" in record]
        ages = [record["peer_age_ms"] for record in valid if "peer_age_ms" in record]
        boots = [record["boot_sequence"] for record in valid if "boot_sequence" in record]
        stale = sum(record.get("fresh_peers", -1) < expected_fresh for record in valid)
        missing = len(samples) - len(valid)
        lapse_delta = max(lapses) - min(lapses) if lapses else None
        reasons = []
        if not valid:
            reasons.append("no valid status samples")
        if missing > max_missing_samples:
            reasons.append(f"missing samples {missing} > {max_missing_samples}")
        if stale > max_stale_samples:
            reasons.append(f"stale samples {stale} > {max_stale_samples}")
        if lapse_delta is None:
            reasons.append("firmware did not report a freshness-lapse counter")
        elif lapse_delta > max_lapse_delta:
            reasons.append(f"freshness lapses {lapse_delta} > {max_lapse_delta}")
        if boots and min(boots) != max(boots):
            reasons.append(f"boot sequence changed {boots[0]} -> {boots[-1]}")
        summary = {
            "samples": len(samples),
            "valid_samples": len(valid),
            "missing_samples": missing,
            "stale_samples": stale,
            "lapse_start": lapses[0] if lapses else None,
            "lapse_end": lapses[-1] if lapses else None,
            "lapse_delta": lapse_delta,
            "boot_start": boots[0] if boots else None,
            "boot_end": boots[-1] if boots else None,
            "max_peer_age_ms": max(ages) if ages else None,
            "pass": not reasons,
            "reasons": reasons,
        }
        result["rings"][name] = summary
        result["pass"] = result["pass"] and summary["pass"]
    return result
