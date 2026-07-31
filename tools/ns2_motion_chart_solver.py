#!/usr/bin/env python3
"""Audit genuine Pro Controller 2 carrier-chart lane maps at transitions.

The length-0x1E motion carrier exposes three normalized wire lanes and a
two-bit chart state.  A chart transition is the only place to test how the new
wire lanes relate to the old chart.  State-stable captures provide coverage,
but they do not prove a cross-chart lane map.

This tool deliberately tests the smallest useful hypothesis first: unsigned
lane permutations.  It reports each observed chart edge independently, then
audits whether those local fits compose into one global state-0 frame.  A
global best fit is only a candidate; it is explicitly rejected when it cannot
reproduce the independently best edge fits.  This distinction matters because
the genuine rapid ``3 -> 1 -> 0`` capture disproves the former assumption that
one unsigned permutation per state is universally composable.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

from ns2_motion_reference import (
    MotionReferenceError,
    decode_motion30_orientation,
    read_motionpair_jsonl,
)


PERMUTATIONS = tuple(itertools.permutations(range(3)))
SIGN_PATTERNS = tuple(itertools.product((-1, 1), repeat=3))
IDENTITY = (0, 1, 2)


@dataclass(frozen=True)
class CarrierSample:
    source: str
    sequence: int
    time_seconds: float
    state: int
    retained: tuple[float, float, float]


@dataclass(frozen=True)
class CarrierTransition:
    source: str
    before_sequence: int
    after_sequence: int
    before_state: int
    after_state: int
    before: tuple[float, float, float]
    after: tuple[float, float, float]


def apply_permutation(
    values: Sequence[float], permutation: Sequence[int]
) -> tuple[float, float, float]:
    return tuple(values[index] for index in permutation)  # type: ignore[return-value]


def transition_error(
    transition: CarrierTransition,
    maps: dict[int, tuple[int, int, int]],
) -> float:
    before = apply_permutation(
        transition.before, maps[transition.before_state]
    )
    after = apply_permutation(
        transition.after, maps[transition.after_state]
    )
    return math.sqrt(sum((left - right) ** 2 for left, right in zip(before, after)))


def _edge_fit(transition: CarrierTransition) -> dict[str, object]:
    """Fit the higher-numbered chart's lanes into the lower chart's wire order."""

    low_state = min(transition.before_state, transition.after_state)
    high_state = max(transition.before_state, transition.after_state)
    if transition.before_state == low_state:
        low = transition.before
        high = transition.after
    else:
        low = transition.after
        high = transition.before

    candidates: list[dict[str, object]] = []
    for permutation in PERMUTATIONS:
        projected = apply_permutation(high, permutation)
        error = math.sqrt(
            sum((left - right) ** 2 for left, right in zip(low, projected))
        )
        candidates.append(
            {
                "permutation": permutation,
                "error": error,
            }
        )
    candidates.sort(key=lambda item: float(item["error"]))
    best = candidates[0]
    runner_up = candidates[1]

    # This broader fit is diagnostic only. It identifies whether an unsigned
    # failure has the shape of a sign/branch transition, but one adjacent pair
    # cannot establish a reusable signed chart transform.
    signed_candidates: list[dict[str, object]] = []
    for permutation in PERMUTATIONS:
        permuted = apply_permutation(high, permutation)
        for signs in SIGN_PATTERNS:
            projected = tuple(
                value * sign for value, sign in zip(permuted, signs)
            )
            error = math.sqrt(
                sum((left - right) ** 2 for left, right in zip(low, projected))
            )
            signed_candidates.append(
                {
                    "permutation": permutation,
                    "signs": signs,
                    "error": error,
                }
            )
    signed_candidates.sort(key=lambda item: float(item["error"]))
    signed_best = signed_candidates[0]

    # Nintendo's cyclic omitted-component topology supplies a much narrower
    # stateful hypothesis than arbitrary signed permutations. Each state s
    # carries components s+1,s+2,s+3 (mod 4). At an a<->b boundary, the
    # component omitted by the source chart is represented by the component
    # omitted by the destination chart. Canonicalizing the omitted component's
    # sign yields exactly two branches: same-sign (no lane flips) or
    # opposite-sign (flip the two non-boundary lanes).
    low_components = tuple((low_state + index) & 3 for index in (1, 2, 3))
    high_components = tuple((high_state + index) & 3 for index in (1, 2, 3))
    topology_permutation: list[int] = []
    for component in low_components:
        source_component = low_state if component == high_state else component
        topology_permutation.append(high_components.index(source_component))
    boundary_lane = low_components.index(high_state)
    same_signs = (1, 1, 1)
    opposite_signs = tuple(
        1 if lane == boundary_lane else -1 for lane in range(3)
    )
    topology_values = apply_permutation(high, topology_permutation)
    branch_candidates: list[dict[str, object]] = []
    for branch, signs in (
        ("same_omitted_sign", same_signs),
        ("opposite_omitted_sign", opposite_signs),
    ):
        projected = tuple(
            value * sign for value, sign in zip(topology_values, signs)
        )
        error = math.sqrt(
            sum((left - right) ** 2 for left, right in zip(low, projected))
        )
        branch_candidates.append(
            {
                "branch": branch,
                "signs": signs,
                "residual": error,
            }
        )
    branch_candidates.sort(key=lambda item: float(item["residual"]))
    branch_best = branch_candidates[0]
    branch_runner_up = branch_candidates[1]
    return {
        "source": transition.source,
        "before_sequence": transition.before_sequence,
        "after_sequence": transition.after_sequence,
        "direction": f"{transition.before_state}->{transition.after_state}",
        "low_state": low_state,
        "high_state": high_state,
        "higher_to_lower_permutation": best["permutation"],
        "residual": best["error"],
        "runner_up_permutation": runner_up["permutation"],
        "runner_up_residual": runner_up["error"],
        "runner_up_margin": float(runner_up["error"]) - float(best["error"]),
        "diagnostic_signed_permutation": signed_best["permutation"],
        "diagnostic_signed_signs": signed_best["signs"],
        "diagnostic_signed_residual": signed_best["error"],
        "cyclic_topology_permutation": tuple(topology_permutation),
        "cyclic_boundary_lane": boundary_lane,
        "cyclic_branch": branch_best["branch"],
        "cyclic_branch_signs": branch_best["signs"],
        "cyclic_branch_residual": branch_best["residual"],
        "cyclic_other_branch_residual": branch_runner_up["residual"],
        "cyclic_branch_margin": (
            float(branch_runner_up["residual"])
            - float(branch_best["residual"])
        ),
    }


def analyze_edge_evidence(
    transitions: Sequence[CarrierTransition],
) -> dict[str, dict[str, object]]:
    """Report local chart-edge fits without assuming that they compose."""

    grouped: dict[tuple[int, int], list[dict[str, object]]] = {}
    for transition in transitions:
        fit = _edge_fit(transition)
        edge = (int(fit["low_state"]), int(fit["high_state"]))
        grouped.setdefault(edge, []).append(fit)

    result: dict[str, dict[str, object]] = {}
    for (low_state, high_state), observations in sorted(grouped.items()):
        permutations = [
            tuple(observation["higher_to_lower_permutation"])
            for observation in observations
        ]
        unique = sorted(set(permutations))
        cyclic_branches = [
            str(observation["cyclic_branch"])
            for observation in observations
        ]
        unique_cyclic_branches = sorted(set(cyclic_branches))
        if len(observations) == 1:
            status = "single_observation"
        elif len(unique) == 1:
            status = "consistent_observed"
        else:
            status = "inconsistent_observed"
        if len(observations) == 1:
            cyclic_status = "single_observation"
        elif len(unique_cyclic_branches) == 1:
            cyclic_status = "consistent_observed"
        else:
            cyclic_status = "inconsistent_observed"
        result[f"{low_state}<->{high_state}"] = {
            "low_state": low_state,
            "high_state": high_state,
            "status": status,
            "observation_count": len(observations),
            "observed_permutations": unique,
            "minimum_residual": min(
                float(observation["residual"])
                for observation in observations
            ),
            "maximum_residual": max(
                float(observation["residual"])
                for observation in observations
            ),
            "cyclic_status": cyclic_status,
            "observed_cyclic_branches": unique_cyclic_branches,
            "minimum_cyclic_residual": min(
                float(observation["cyclic_branch_residual"])
                for observation in observations
            ),
            "maximum_cyclic_residual": max(
                float(observation["cyclic_branch_residual"])
                for observation in observations
            ),
            "observations": observations,
        }
    return result


def read_capture(path: Path) -> tuple[list[CarrierSample], int]:
    notifications, dropped = read_motionpair_jsonl(path)
    samples: list[CarrierSample] = []
    for sequence, notification in enumerate(notifications):
        length = notification.value[0x0E]
        if length != 0x1E:
            continue
        pdu = notification.value[0x0F:0x0F + length]
        orientation = decode_motion30_orientation(pdu)
        samples.append(
            CarrierSample(
                source=str(path),
                sequence=sequence,
                time_seconds=notification.time_seconds,
                state=orientation.state,
                retained=orientation.retained,
            )
        )
    return samples, dropped


def transition_window_is_contiguous(path: Path, dropped: int) -> bool:
    """Accept zero-drop captures or a completed trigger-frozen window."""

    if dropped == 0:
        return True
    end: dict[str, object] | None = None
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(item, dict) and item.get("motionpair") == "end":
                end = item
    if end is None or not isinstance(end.get("chart"), dict):
        return False
    chart = end["chart"]
    return bool(chart.get("triggered")) and bool(chart.get("complete"))


def transitions_from_samples(
    samples: Sequence[CarrierSample],
) -> list[CarrierTransition]:
    transitions: list[CarrierTransition] = []
    for before, after in zip(samples, samples[1:]):
        if before.source != after.source or before.state == after.state:
            continue
        transitions.append(
            CarrierTransition(
                source=before.source,
                before_sequence=before.sequence,
                after_sequence=after.sequence,
                before_state=before.state,
                after_state=after.state,
                before=before.retained,
                after=after.retained,
            )
        )
    return transitions


def _connected_states(
    transitions: Sequence[CarrierTransition], anchor: int
) -> set[int]:
    connected = {anchor}
    changed = True
    while changed:
        changed = False
        for transition in transitions:
            edge = {transition.before_state, transition.after_state}
            if connected & edge and not edge <= connected:
                connected.update(edge)
                changed = True
    return connected


def solve_chart_maps(
    transitions: Sequence[CarrierTransition],
    anchor: int = 0,
) -> dict[str, object]:
    connected = _connected_states(transitions, anchor)
    usable = [
        transition
        for transition in transitions
        if transition.before_state in connected
        and transition.after_state in connected
    ]
    variables = sorted(connected - {anchor})
    candidates: list[dict[str, object]] = []
    for selections in itertools.product(PERMUTATIONS, repeat=len(variables)):
        maps = {anchor: IDENTITY}
        maps.update(zip(variables, selections))
        errors = [transition_error(transition, maps) for transition in usable]
        squared = sum(error * error for error in errors)
        candidates.append(
            {
                "maps": maps,
                "transition_errors": errors,
                "sum_squared_error": squared,
                "rms_error": (
                    math.sqrt(squared / len(errors)) if errors else None
                ),
                "max_error": max(errors) if errors else None,
            }
        )
    candidates.sort(key=lambda item: float(item["sum_squared_error"]))
    best = candidates[0]
    runner_up = candidates[1] if len(candidates) > 1 else None
    return {
        "anchor_state": anchor,
        "connected_states": sorted(connected),
        "transition_count": len(usable),
        "best": best,
        "runner_up": runner_up,
        "squared_error_margin": (
            float(runner_up["sum_squared_error"])
            - float(best["sum_squared_error"])
            if runner_up is not None
            else None
        ),
    }


def analyze_captures(paths: Iterable[Path]) -> dict[str, object]:
    all_samples: list[CarrierSample] = []
    all_transitions: list[CarrierTransition] = []
    captures: list[dict[str, object]] = []
    for path in paths:
        samples, dropped = read_capture(path)
        transition_safe = transition_window_is_contiguous(path, dropped)
        transitions = (
            transitions_from_samples(samples) if transition_safe else []
        )
        all_samples.extend(samples)
        all_transitions.extend(transitions)
        captures.append(
            {
                "path": str(path),
                "samples": len(samples),
                "dropped_records": dropped,
                "transition_evidence": (
                    "accepted_contiguous"
                    if transition_safe
                    else "rejected_noncontiguous"
                ),
                "state_counts": dict(Counter(sample.state for sample in samples)),
                "transitions": len(transitions),
            }
        )

    state_counts = Counter(sample.state for sample in all_samples)
    observed_states = sorted(
        state for state in range(4) if state_counts.get(state, 0)
    )
    edge_counts = Counter(
        (transition.before_state, transition.after_state)
        for transition in all_transitions
    )
    solution = solve_chart_maps(all_transitions)
    edge_evidence = analyze_edge_evidence(all_transitions)
    cyclic_observations = [_edge_fit(transition) for transition in all_transitions]
    cyclic_squared = sum(
        float(observation["cyclic_branch_residual"]) ** 2
        for observation in cyclic_observations
    )
    local_errors = [
        float(_edge_fit(transition)["residual"])
        for transition in all_transitions
    ]
    global_errors = [
        float(error)
        for error in solution["best"]["transition_errors"]
    ]
    maximum_composition_excess = max(
        (
            global_error - local_error
            for global_error, local_error in zip(global_errors, local_errors)
        ),
        default=0.0,
    )
    global_model_status = (
        "inconsistent_with_local_edge_fits"
        if maximum_composition_excess > 1e-9
        else "composable_candidate"
    )
    if not all_transitions:
        cyclic_model_status = "no_transition_evidence"
        cyclic_model_note = (
            "stateful cyclic topology with paired non-boundary sign flips; "
            "no adjacent chart transition was captured"
        )
    elif observed_states == [0, 1, 2, 3]:
        cyclic_model_status = "supported_on_all_chart_states"
        cyclic_model_note = (
            "stateful cyclic topology with paired non-boundary sign flips; "
            "all four chart states have adjacent hardware evidence"
        )
    else:
        cyclic_model_status = (
            "supported_on_observed_states_"
            + "_".join(str(state) for state in observed_states)
        )
        missing_states = [
            state for state in range(4) if state not in observed_states
        ]
        missing_label = (
            f"state {missing_states[0]}"
            if len(missing_states) == 1
            else "states " + ",".join(str(state) for state in missing_states)
        )
        missing_verb = "remains" if len(missing_states) == 1 else "remain"
        cyclic_model_note = (
            "stateful cyclic topology with paired non-boundary sign flips; "
            f"{missing_label} {missing_verb} prediction-only until captured"
        )
    connected = set(solution["connected_states"])

    def state_status(state: int) -> str:
        if state == solution["anchor_state"]:
            return "anchor"
        if not state_counts.get(state, 0):
            return "unseen"
        edge = edge_evidence.get(
            f"{min(state, solution['anchor_state'])}"
            f"<->{max(state, solution['anchor_state'])}"
        )
        if edge is not None:
            return f"anchor_edge_{edge['status']}"
        if state in connected:
            return "indirect_transition_only"
        return "observed_without_anchor_transition"

    return {
        "captures": captures,
        "state_counts": {state: state_counts.get(state, 0) for state in range(4)},
        "transition_counts": {
            f"{before}->{after}": count
            for (before, after), count in sorted(edge_counts.items())
        },
        "solution": solution,
        "edge_evidence": edge_evidence,
        "global_unsigned_model": {
            "status": global_model_status,
            "maximum_composition_excess": maximum_composition_excess,
            "note": (
                "candidate_only; a state-wide unsigned permutation is not "
                "protocol truth unless all local chart edges compose"
            ),
        },
        "cyclic_omission_branch_model": {
            "status": cyclic_model_status,
            "observation_count": len(cyclic_observations),
            "rms_residual": (
                math.sqrt(cyclic_squared / len(cyclic_observations))
                if cyclic_observations
                else None
            ),
            "maximum_residual": max(
                (
                    float(observation["cyclic_branch_residual"])
                    for observation in cyclic_observations
                ),
                default=None,
            ),
            "minimum_branch_margin": min(
                (
                    float(observation["cyclic_branch_margin"])
                    for observation in cyclic_observations
                ),
                default=None,
            ),
            "branches": dict(
                Counter(
                    str(observation["cyclic_branch"])
                    for observation in cyclic_observations
                )
            ),
            "note": cyclic_model_note,
        },
        "state_status": {
            state: state_status(state)
            for state in range(4)
        },
        "transitions": [asdict(transition) for transition in all_transitions],
    }


def _format_map(permutation: Sequence[int]) -> str:
    return "(" + ",".join(f"G{index}" for index in permutation) + ")"


def format_summary(result: dict[str, object]) -> str:
    lines = [
        "Pro Controller 2 carrier-chart corpus",
        f"  states: {result['state_counts']}",
        f"  transitions: {result['transition_counts']}",
    ]
    solution = result["solution"]
    best = solution["best"]
    maps = best["maps"]
    lines.append(
        "  global unsigned candidate: "
        + ", ".join(
            f"state {state} -> {_format_map(permutation)}"
            for state, permutation in sorted(maps.items())
        )
    )
    lines.append(
        "  global model status: "
        f"{result['global_unsigned_model']['status']} "
        "(max composition excess "
        f"{result['global_unsigned_model']['maximum_composition_excess']:.6f})"
    )
    cyclic = result["cyclic_omission_branch_model"]
    if cyclic["observation_count"]:
        lines.append(
            "  cyclic omission/sign-branch model: "
            f"{cyclic['status']}, RMS/max "
            f"{cyclic['rms_residual']:.6f}/{cyclic['maximum_residual']:.6f}, "
            f"min branch margin {cyclic['minimum_branch_margin']:.6f}, "
            f"branches={cyclic['branches']}"
        )
    lines.append(
        f"  transition RMS/max: {best['rms_error']:.6f}/"
        f"{best['max_error']:.6f}"
        if best["rms_error"] is not None
        else "  transition RMS/max: unavailable"
    )
    lines.append(
        "  status: "
        + ", ".join(
            f"{state}={status}"
            for state, status in result["state_status"].items()
        )
    )
    if solution["runner_up"] is not None:
        lines.append(
            "  runner-up squared-error margin: "
            f"{solution['squared_error_margin']:.9f}"
        )
    for edge, evidence in result["edge_evidence"].items():
        lines.append(
            f"  edge {edge}: unsigned={evidence['status']}, "
            f"observations={evidence['observation_count']}, "
            f"permutations={evidence['observed_permutations']}, "
            f"residual={evidence['minimum_residual']:.6f}.."
            f"{evidence['maximum_residual']:.6f}; "
            f"cyclic={evidence['cyclic_status']}, "
            f"branches={evidence['observed_cyclic_branches']}, "
            f"residual={evidence['minimum_cyclic_residual']:.6f}.."
            f"{evidence['maximum_cyclic_residual']:.6f}"
        )
    for capture in result["captures"]:
        lines.append(
            f"  {capture['path']}: {capture['samples']} carrier samples, "
            f"states={capture['state_counts']}, transitions={capture['transitions']}, "
            f"dropped={capture['dropped_records']}, "
            f"evidence={capture['transition_evidence']}"
        )
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "capture",
        type=Path,
        nargs="+",
        help="UART motionpair JSONL capture (repeatable)",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args(argv)
    try:
        result = analyze_captures(args.capture)
    except MotionReferenceError as exc:
        parser.error(str(exc))
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(format_summary(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
