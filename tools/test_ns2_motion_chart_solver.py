#!/usr/bin/env python3
"""Host tests for the evidence-gated Pro2 carrier-chart solver."""

from __future__ import annotations

import math
from pathlib import Path

from ns2_motion_chart_solver import (
    CarrierSample,
    CarrierTransition,
    analyze_captures,
    analyze_edge_evidence,
    solve_chart_maps,
    transition_window_is_contiguous,
    transitions_from_samples,
)


ROOT = Path(__file__).resolve().parents[1]


def _sample(sequence: int, state: int, retained: tuple[float, float, float]):
    return CarrierSample(
        source="fixture",
        sequence=sequence,
        time_seconds=sequence * 0.01,
        state=state,
        retained=retained,
    )


def test_transition_extraction_does_not_bridge_sources() -> None:
    samples = [
        _sample(0, 0, (0.1, 0.2, 0.3)),
        _sample(1, 3, (0.3, 0.1, 0.2)),
        CarrierSample("other", 0, 1.0, 1, (0.4, 0.5, 0.6)),
    ]
    transitions = transitions_from_samples(samples)
    assert len(transitions) == 1
    assert (transitions[0].before_state, transitions[0].after_state) == (0, 3)


def test_solver_recovers_state3_cyclic_map() -> None:
    transitions = [
        CarrierTransition(
            "a", 0, 1, 0, 3,
            (0.10, 0.20, 0.30),
            (0.301, 0.099, 0.201),
        ),
        CarrierTransition(
            "b", 0, 1, 3, 0,
            (-0.40, 0.25, 0.15),
            (0.251, 0.151, -0.399),
        ),
    ]
    solution = solve_chart_maps(transitions)
    assert solution["connected_states"] == [0, 3]
    assert solution["best"]["maps"][0] == (0, 1, 2)
    assert solution["best"]["maps"][3] == (1, 2, 0)
    assert solution["best"]["rms_error"] < 0.003
    assert solution["squared_error_margin"] > 0.01
    edge = analyze_edge_evidence(transitions)["0<->3"]
    assert edge["status"] == "consistent_observed"
    assert edge["observed_permutations"] == [(1, 2, 0)]


def test_real_reciprocal_fixture_and_unresolved_states() -> None:
    captures = [
        ROOT / "dumps/BLE CAPTURE/pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
        ROOT / "dumps/BLE CAPTURE/pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
        ROOT / "dumps/experiments/20260729-122701-pro2-mag-ceramic-confirmation-immediate-baseline/motion.raw.jsonl",
    ]
    result = analyze_captures(captures)
    assert result["solution"]["best"]["maps"][3] == (1, 2, 0)
    assert result["transition_counts"] == {"0->3": 1, "3->0": 1}
    assert result["state_status"][0] == "anchor"
    assert result["state_status"][1] == "observed_without_anchor_transition"
    assert result["state_status"][2] == "unseen"
    assert result["state_status"][3] == "anchor_edge_consistent_observed"
    assert result["global_unsigned_model"]["status"] == "composable_candidate"
    assert result["captures"][0]["transition_evidence"] == "accepted_contiguous"
    assert math.isclose(
        result["solution"]["best"]["rms_error"],
        0.0019809859237529927,
        rel_tol=0.0,
        abs_tol=1e-12,
    )


def test_rolling_untriggered_window_is_not_transition_evidence() -> None:
    capture = (
        ROOT
        / "dumps/BLE CAPTURE/pro2-chart-face-forward-no-transition-2026-07-29.jsonl"
    )
    assert not transition_window_is_contiguous(capture, 3881)


def test_state1_transition_supplies_one_anchor_edge_observation() -> None:
    root = ROOT / "dumps/BLE CAPTURE"
    result = analyze_captures(
        [
            root / "pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
            root / "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
            root / "pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl",
        ]
    )
    assert result["transition_counts"] == {
        "0->1": 1,
        "0->3": 1,
        "3->0": 1,
    }
    assert result["solution"]["best"]["maps"][1] == (2, 0, 1)
    assert result["state_status"][1] == "anchor_edge_single_observation"
    assert result["edge_evidence"]["0<->1"]["status"] == "single_observation"
    assert result["global_unsigned_model"]["status"] == "composable_candidate"
    assert result["state_status"][2] == "unseen"


def test_rapid_state1_capture_rejects_global_unsigned_composition() -> None:
    root = ROOT / "dumps/BLE CAPTURE"
    result = analyze_captures(
        [
            root / "pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
            root / "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
            root / "pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl",
            root / "pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl",
        ]
    )
    assert result["transition_counts"] == {
        "0->1": 1,
        "0->3": 1,
        "1->0": 1,
        "3->0": 1,
        "3->1": 1,
    }
    edge01 = result["edge_evidence"]["0<->1"]
    assert edge01["status"] == "inconsistent_observed"
    assert edge01["observed_permutations"] == [(2, 0, 1), (2, 1, 0)]
    assert math.isclose(
        edge01["minimum_residual"],
        0.01702490718064754,
        rel_tol=0.0,
        abs_tol=1e-12,
    )
    assert edge01["maximum_residual"] > 1.18
    reverse_observation = next(
        observation
        for observation in edge01["observations"]
        if observation["direction"] == "1->0"
    )
    assert reverse_observation["diagnostic_signed_residual"] < 0.025
    assert reverse_observation["diagnostic_signed_signs"] != (1, 1, 1)
    assert reverse_observation["cyclic_topology_permutation"] == (2, 0, 1)
    assert reverse_observation["cyclic_boundary_lane"] == 0
    assert reverse_observation["cyclic_branch"] == "opposite_omitted_sign"
    assert reverse_observation["cyclic_branch_signs"] == (1, -1, -1)
    assert reverse_observation["cyclic_branch_residual"] < 0.025
    assert reverse_observation["cyclic_branch_margin"] > 1.35
    assert result["cyclic_omission_branch_model"] == {
        "status": "supported_on_observed_states_0_1_3",
        "observation_count": 5,
        "rms_residual": result["cyclic_omission_branch_model"]["rms_residual"],
        "maximum_residual": result["cyclic_omission_branch_model"][
            "maximum_residual"
        ],
        "minimum_branch_margin": result["cyclic_omission_branch_model"][
            "minimum_branch_margin"
        ],
        "branches": {
            "same_omitted_sign": 4,
            "opposite_omitted_sign": 1,
        },
        "note": (
            "stateful cyclic topology with paired non-boundary sign flips; "
            "state 2 remains prediction-only until captured"
        ),
    }
    assert result["cyclic_omission_branch_model"]["maximum_residual"] < 0.048
    assert result["cyclic_omission_branch_model"]["minimum_branch_margin"] > 0.32
    assert result["edge_evidence"]["1<->3"]["status"] == "single_observation"
    assert (
        result["global_unsigned_model"]["status"]
        == "inconsistent_with_local_edge_fits"
    )
    assert result["global_unsigned_model"]["maximum_composition_excess"] > 1.2
    assert result["state_status"][1] == "anchor_edge_inconsistent_observed"
    assert result["state_status"][2] == "unseen"


def test_state2_transition_closes_all_chart_states() -> None:
    root = ROOT / "dumps/BLE CAPTURE"
    result = analyze_captures(
        [
            root / "pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
            root / "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
            root / "pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl",
            root / "pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl",
            root / "pro2-chart-transition-3-to-2-2026-07-29.jsonl",
        ]
    )
    assert result["transition_counts"] == {
        "0->1": 2,
        "0->3": 1,
        "1->0": 1,
        "1->3": 1,
        "2->3": 1,
        "3->0": 1,
        "3->1": 1,
        "3->2": 1,
    }
    assert result["state_counts"][2] == 26
    assert result["state_status"][2] == "indirect_transition_only"

    edge23 = result["edge_evidence"]["2<->3"]
    assert edge23["status"] == "consistent_observed"
    assert edge23["cyclic_status"] == "consistent_observed"
    assert edge23["observed_cyclic_branches"] == [
        "opposite_omitted_sign"
    ]
    assert edge23["observation_count"] == 2
    assert edge23["maximum_cyclic_residual"] < 0.037
    for observation in edge23["observations"]:
        assert observation["cyclic_topology_permutation"] == (2, 0, 1)
        assert observation["cyclic_branch"] == "opposite_omitted_sign"
        assert observation["cyclic_branch_signs"] == (1, -1, -1)
        assert observation["cyclic_branch_residual"] < 0.037
        assert observation["cyclic_branch_margin"] > 0.98

    cyclic = result["cyclic_omission_branch_model"]
    assert cyclic["status"] == "supported_on_all_chart_states"
    assert cyclic["observation_count"] == 9
    assert cyclic["branches"] == {
        "same_omitted_sign": 6,
        "opposite_omitted_sign": 3,
    }
    assert cyclic["maximum_residual"] < 0.048
    assert cyclic["minimum_branch_margin"] > 0.32
    assert cyclic["note"] == (
        "stateful cyclic topology with paired non-boundary sign flips; "
        "all four chart states have adjacent hardware evidence"
    )
    assert result["captures"][-1]["dropped_records"] == 0
    assert result["captures"][-1]["transition_evidence"] == "accepted_contiguous"


def main() -> int:
    test_transition_extraction_does_not_bridge_sources()
    test_solver_recovers_state3_cyclic_map()
    test_real_reciprocal_fixture_and_unresolved_states()
    test_rolling_untriggered_window_is_not_transition_evidence()
    test_state1_transition_supplies_one_anchor_edge_observation()
    test_rapid_state1_capture_rejects_global_unsigned_composition()
    test_state2_transition_closes_all_chart_states()
    print("ns2 motion chart solver tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
