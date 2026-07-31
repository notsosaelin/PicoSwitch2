#!/usr/bin/env python3
"""Analyze PicoSwitch2 audio-lab snapshots without guessing from one maximum."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


class AudioLabError(ValueError):
    pass


COUNTERS = (
    "sends",
    "hci_events",
    "hci_packets",
    "pcm_packets",
    "pcm_nonzero",
    "pcm_short",
    "pcm_dropped",
    "pcm_over_2ms",
    "opus_frames",
    "opus_errors",
    "opus_over_20ms",
    "pipeline_resets",
    "codec_calls",
    "codec_blocks",
    "codec_no_encoder",
    "codec_no_pcm",
    "codec_disconnected",
    "codec_usb_inactive",
    "codec_over_10ms",
    "core1_over_10ms",
    "usb_active_us",
)

MAXIMA = (
    "send_max_us",
    "hci_max_us",
    "pcm_max_gap_us",
    "pcm_queue_max",
    "opus_encode_max_us",
    "opus_gap_max_us",
    "codec_gap_max_us",
    "core1_gap_max_us",
)


def _load(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AudioLabError(f"{path}: {exc}") from exc
    if document.get("schema") != "picoswitch2-audio-samples/v1":
        raise AudioLabError(f"{path}: unsupported or missing schema")
    samples = document.get("samples")
    if not isinstance(samples, list) or len(samples) < 2:
        raise AudioLabError(f"{path}: at least two audio samples are required")
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict) or not isinstance(sample.get("audio"), dict):
            raise AudioLabError(f"{path}: sample {index} lacks an audio object")
        if sample["audio"].get("audio") is not True:
            raise AudioLabError(f"{path}: sample {index} is not an audio status reply")
    return document


def _delta(first: dict[str, Any], last: dict[str, Any], name: str) -> int | None:
    if name not in first or name not in last:
        return None
    before = int(first[name])
    after = int(last[name])
    if after < before:
        return None
    return after - before


def analyze(document: dict[str, Any]) -> dict[str, Any]:
    samples = document["samples"]
    first = samples[0]["audio"]
    last = samples[-1]["audio"]
    deltas = {
        name: _delta(first, last, name)
        for name in COUNTERS
        if name in first and name in last
    }
    maxima = {
        name: max(int(sample["audio"].get(name, 0)) for sample in samples)
        for name in MAXIMA
    }
    reset_fields = [name for name, value in deltas.items() if value is None]
    warnings: list[str] = []
    failures: list[str] = []

    if deltas.get("pcm_dropped", 0):
        failures.append(f"USB PCM dropped {deltas['pcm_dropped']} packet(s)")
    if deltas.get("opus_errors", 0):
        failures.append(f"Opus reported {deltas['opus_errors']} encode error(s)")
    if deltas.get("pcm_short", 0):
        warnings.append(f"USB delivered {deltas['pcm_short']} short packet(s)")
    if deltas.get("pipeline_resets", 0):
        warnings.append(f"audio pipeline reset {deltas['pipeline_resets']} time(s)")
    if reset_fields:
        warnings.append(
            "counter reset/wrap prevents deltas for " + ", ".join(reset_fields)
        )

    stack_values = [
        int(sample["audio"].get("core1_stack_free", 0)) for sample in samples
    ]
    minimum_stack = min(stack_values)
    if minimum_stack and minimum_stack < 8192:
        warnings.append(f"core-1 stack headroom fell to {minimum_stack} bytes")

    active_samples = sum(bool(sample["audio"].get("usb_active")) for sample in samples)
    if active_samples == 0:
        warnings.append("USB speaker stream was inactive in every snapshot")

    if failures:
        verdict = "FAIL"
    elif warnings:
        verdict = "OBSERVE"
    else:
        verdict = "PASS"
    return {
        "schema": "picoswitch2-audio-analysis/v1",
        "verdict": verdict,
        "failures": failures,
        "warnings": warnings,
        "sample_count": len(samples),
        "active_samples": active_samples,
        "duration_s": (
            int(samples[-1]["elapsed_ms"]) - int(samples[0]["elapsed_ms"])
        )
        / 1000.0,
        "deltas": deltas,
        "observed_maxima": maxima,
        "minimum_core1_stack_free": minimum_stack,
        "boundary": (
            "Timing maxima identify where to investigate; they do not by themselves "
            "prove audible failure. PCM drops and Opus errors are hard failures."
        ),
    }


def compare(current: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    current_analysis = analyze(current)
    baseline_analysis = analyze(baseline)
    fields = (
        "pcm_dropped",
        "opus_errors",
        "pipeline_resets",
        "sends",
        "opus_frames",
    )
    delta_change = {
        name: (
            current_analysis["deltas"].get(name),
            baseline_analysis["deltas"].get(name),
        )
        for name in fields
    }
    return {
        "schema": "picoswitch2-audio-comparison/v1",
        "current": current_analysis,
        "baseline": baseline_analysis,
        "delta_current_baseline": delta_change,
    }


def _render(analysis: dict[str, Any]) -> str:
    lines = [
        f"Verdict: {analysis['verdict']}",
        f"Samples: {analysis['sample_count']} over {analysis['duration_s']:.1f} s",
        f"USB-active samples: {analysis['active_samples']}",
        f"Core-1 stack free minimum: {analysis['minimum_core1_stack_free']} bytes",
        "",
        "Counter deltas:",
    ]
    lines.extend(
        f"  {name}: {value}" for name, value in analysis["deltas"].items()
    )
    lines.append("")
    lines.append("Observed maxima:")
    lines.extend(
        f"  {name}: {value}" for name, value in analysis["observed_maxima"].items()
    )
    if analysis["failures"]:
        lines.extend(["", "Failures:", *[f"  - {item}" for item in analysis["failures"]]])
    if analysis["warnings"]:
        lines.extend(["", "Warnings:", *[f"  - {item}" for item in analysis["warnings"]]])
    lines.extend(["", analysis["boundary"]])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples")
    parser.add_argument("--baseline")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        current = _load(Path(args.samples))
        if args.baseline:
            result = compare(current, _load(Path(args.baseline)))
        else:
            result = analyze(current)
    except AudioLabError as exc:
        print(f"audio_lab_analyze: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    elif args.baseline:
        print("Current")
        print(_render(result["current"]))
        print("\nBaseline")
        print(_render(result["baseline"]))
    else:
        print(_render(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
