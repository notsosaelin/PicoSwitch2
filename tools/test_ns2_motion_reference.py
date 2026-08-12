#!/usr/bin/env python3
"""Host tests for ns2_motion_reference.py pure decoders."""

import json
import tempfile
from pathlib import Path

from ns2_motion_reference import (
    Notification,
    analyze_native,
    decode_motion30_orientation,
    decode_motion30_quaternion,
    decode_motion40,
    decode_report05_raw_imu,
    decode_temperature_tail16_value,
    decode_tail16_fields,
    icm_fifo_header_audit,
    legacy_g678_source_ranges,
    read_blecap_jsonl,
    read_motionpair_jsonl,
)


def test_raw_report() -> None:
    report = bytes.fromhex(
        "a30e0000000000000000af47842f3885000000000000000000000000000000"
        "2f0d000000000000000001b7ab02000a00a7ff4d023510eeff03001400000000"
    )
    sample = decode_report05_raw_imu(report)
    assert sample.timestamp_us == 0x0002ABB7
    assert sample.temperature_raw == 10
    assert abs(sample.temperature_c - 25.078125) < 1e-9
    assert sample.accel == (-89, 589, 4149)
    assert sample.gyro == (-18, 3, 20)


def test_native_catchup_report() -> None:
    pdu = bytes.fromhex(
        "d908010f63067ff171a650c6fa04ecf49810ff3fffff00000ae8f44cc8fffffe"
        "df00c009e8e93321"
    )
    sample = decode_motion40(pdu, previous_tick=2249)
    assert sample.tick == 2265
    assert sample.tick_delta == 16
    assert sample.elapsed_ticks == 16
    assert sample.sensor_status == 0x0F
    assert sample.layout == "catchup"
    assert sample.accel == (
        (79, -709, 4248),
        (40, -355, 2124),
        (78, -707, 4249),
    )
    assert sample.gyro == ((-4, -4, 3), (-2, -9, 6))
    assert sample.packing_mode == 3
    assert sample.prefix_lane2_low2 == 1
    assert sample.prefix_carrier == (2081176, 422385, -2739579)
    assert sample.prefix_widths == (22, 21, 23)
    assert sample.prefix_precision_shift == 0
    assert sample.tail_value == 0
    assert sample.tail_width == 1


def test_native_high_rate_report() -> None:
    pdu = bytes.fromhex(
        "2f72000d43de74b70a7b3be73af23e01043a3d002714e4ffd7fbff400180aa13"
        "e0bad35b65421b01"
    )
    sample = decode_motion40(pdu, previous_tick=0x228)
    assert sample.tick == 0x22F
    assert sample.tick_delta == 7
    assert sample.layout == "high_rate"
    assert sample.accel == (
        (20412, -181756, 1088512),
        (20138, -181330, 1087830),
    )
    assert sample.gyro == ((-447, -267, 320),)
    assert sample.packing_mode == 3
    assert sample.prefix_lane2_low2 == 1
    assert sample.prefix_carrier == (-2279536, -2178387, -14847075)
    assert sample.prefix_widths == (24, 23, 25)
    assert sample.prefix_precision_shift == 2
    assert sample.tail_value == 0x011B
    assert sample.tail_width == 16
    tail = decode_tail16_fields(sample)
    assert tail.temperature_integer_raw == 4
    assert tail.temperature_a_fraction3 == 3
    assert tail.temperature_b_fraction3 == 3
    assert tail.temperature_a_q3 == 35
    assert tail.temperature_b_q3 == 35
    assert tail.temperature_a_raw == 4.375
    assert tail.temperature_b_raw == 4.375
    assert abs(tail.temperature_a_c - 25.0341796875) < 1e-12
    assert abs(tail.temperature_b_c - 25.0341796875) < 1e-12
    assert tail.fractions_equal is True

    negative = decode_temperature_tail16_value((0x3FE << 6) | (5 << 3) | 4)
    assert negative.temperature_integer_raw == -2
    assert negative.temperature_a_q3 == -12
    assert negative.temperature_b_q3 == -11
    assert negative.temperature_a_raw == -1.5
    assert negative.temperature_b_raw == -1.375


def test_native_normal_report() -> None:
    pdu = bytes.fromhex(
        "62b7000ec3c3ee31093d955d0805f0f49bd0ffffff00200578fa260400f4ff01"
        "8013f0d363420001"
    )
    sample = decode_motion40(pdu, previous_tick=1879)
    assert sample.tick == 1890
    assert sample.tick_delta == 11
    assert sample.layout == "normal"
    assert sample.accel == ((80, -708, 4251), (41, -354, 2125), (78, -705, 4248))
    assert sample.gyro == ((-1, -1, 0), (0, -3, 1))
    assert sample.packing_mode == 3
    assert sample.prefix_lane2_low2 == 1
    assert sample.prefix_carrier == (-282384, -194255, -4002647)
    assert sample.prefix_widths == (22, 21, 23)
    assert sample.prefix_precision_shift == 0
    assert sample.tail_value == 0x0100
    assert sample.tail_width == 16


def test_length30_quaternion_decoder() -> None:
    pdu = bytes.fromhex(
        "8750000C0014E61502FAEBB0005C17A8DEE32F005E9292016006C6100002"
    )
    quaternion = decode_motion30_quaternion(pdu)
    assert abs(sum(value * value for value in quaternion) - 1.0) < 1e-9
    assert quaternion[0] > 0.0
    orientation = decode_motion30_orientation(pdu)
    assert orientation.state == 0
    assert orientation.quaternion_wxyz == quaternion
    assert orientation.carrier_raw == (
        34989588,
        11594746,
        11016028,
    )


def test_cadence_layout_boundaries() -> None:
    def sample(elapsed: int, previous_tick: int | None = None):
        pdu = bytearray(40)
        pdu[0] = 100
        pdu[1] = (elapsed & 0x0F) << 4
        pdu[2] = elapsed >> 4
        # The cadence layouts are sub-layouts of packing mode 3. A zeroed
        # payload is the observed mode-0 marker and must not be decoded as IMU.
        pdu[4] = 0x03
        return decode_motion40(bytes(pdu), previous_tick=previous_tick)

    assert sample(10).layout == "high_rate"
    assert sample(11).layout == "normal"
    assert sample(14).layout == "normal"
    assert sample(15).layout == "catchup"
    assert sample(16).layout == "catchup"
    # Classification is self-contained even when the captured predecessor is
    # absent or wrong.
    assert sample(7, previous_tick=0).layout == "high_rate"


def test_exact_fifo_header_rules() -> None:
    pdu = bytearray(40)
    pdu[0] = 0x70
    pdu[20] = 0x7C
    audit = icm_fifo_header_audit(bytes(pdu))
    assert audit["two_packet4_at_0_20"] is True
    assert audit["two_packet3_offsets"] == ()

    pdu[0] = 0x60
    pdu[16] = 0x6C
    audit = icm_fifo_header_audit(bytes(pdu))
    assert audit["two_packet4_at_0_20"] is False
    assert 0 in audit["two_packet3_offsets"]


def test_interleaved_native_layout_and_integrity_delta() -> None:
    def report(length: int, tick: int, elapsed: int) -> bytes:
        pdu = bytearray(length)
        pdu[0] = tick & 0xFF
        pdu[1] = (elapsed << 4) | ((tick >> 8) & 0x0F)
        if length == 0x28:
            pdu[4] = 0x03
        return bytes(0x0E) + bytes([length]) + bytes(pdu)

    notifications = [
        Notification(0.0000, report(0x1E, 100, 5)),
        Notification(0.0075, report(0x28, 107, 7)),
        Notification(0.0150, report(0x1E, 112, 5)),
        Notification(0.0225, report(0x28, 119, 7)),
    ]
    summary = analyze_native(notifications)
    assert summary["pdu40_count"] == 2
    assert summary["high_rate"]["count"] == 2
    assert summary["normal"] == {}
    assert summary["catchup"] == {}
    assert summary["tick_delta"]["min"] == 7
    assert summary["tick_delta"]["max"] == 7
    assert summary["elapsed_ticks_match_preceding_delta"] == 2


def test_normal_layout_keeps_joycon_cadence_boundary() -> None:
    def report(tick: int) -> bytes:
        pdu = bytearray(0x28)
        pdu[0] = tick & 0xFF
        pdu[1] = (12 << 4) | ((tick >> 8) & 0x0F)
        pdu[4] = 0x03
        return bytes(0x0E) + bytes([0x28]) + bytes(pdu)

    summary = analyze_native(
        [Notification(0.000, report(100)), Notification(0.015, report(112))]
    )
    assert summary["normal"]["count"] == 2
    assert summary["high_rate"] == {}


def test_dropped_capture_keeps_self_contained_layout() -> None:
    def report(tick: int) -> bytes:
        pdu = bytearray(0x28)
        pdu[0] = tick & 0xFF
        pdu[1] = (7 << 4) | ((tick >> 8) & 0x0F)
        pdu[2] = 0
        pdu[3] = 0x0D
        pdu[4] = 0x03
        return bytes(0x0E) + bytes([0x28]) + bytes(pdu)

    summary = analyze_native(
        [
            Notification(0.000, report(100)),
            # The observed 100-tick jump represents omitted capture records,
            # while this retained packet still says its own layout elapsed 7.
            Notification(0.010, report(200)),
        ],
        dropped_records=5,
    )
    assert summary["high_rate"]["count"] == 2
    assert summary["normal"] == {}
    assert summary["catchup"] == {}
    assert summary["elapsed_ticks_match_preceding_delta"] == 0
    assert summary["elapsed_ticks_mismatches"] == {
        "encoded_7_observed_100": 1
    }
    assert summary["sensor_status_profiles"]["0x0D"]["layouts"] == {
        "high_rate": 2
    }
    assert summary["prefix"]["carrier_epoch_fit"] == {
        "status": "skipped_retained_sequence_has_tick_gaps",
        "dropped_records": 5,
        "local_elapsed_mismatches": 1,
    }

    contiguous = analyze_native(
        [
            Notification(0.000, report(100)),
            Notification(0.010, report(107)),
        ],
        dropped_records=5,
    )
    assert contiguous["capture_integrity"]["preceding_carrier_sequence"] == (
        "retained_window_contiguous_global_drops_elsewhere"
    )
    assert contiguous["prefix"]["carrier_epoch_fit"] == {
        "status": "insufficient_state_aligned_records",
        "pdu40_records": 2,
        "length30_records": 0,
        "capture_integrity": (
            "global_drops_but_retained_window_is_locally_contiguous"
        ),
        "dropped_records": 5,
    }


def test_legacy_alias_overlap() -> None:
    assert legacy_g678_source_ranges() == (
        (204, 225),
        (228, 249),
        (252, 271),
    )
    # Catch-up gyro2 occupies payload bits 197..244 and accel3 245..286.
    assert legacy_g678_source_ranges()[0][0] <= 244
    assert legacy_g678_source_ranges()[1][1] >= 245
    assert legacy_g678_source_ranges()[2][0] >= 245


def test_blecap_reader() -> None:
    report = (
        "a30e0000000000000000af47842f3885000000000000000000000000000000"
        "2f0d000000000000000001b7ab02000a00a7ff4d023510eeff03001400000000"
    )
    records = [
        {
            "blecap": "record",
            "t_us": 0xFFFFFFF0,
            "kind": "input",
            "handle": "0x000A",
            "length": 63,
            "captured": 63,
            "payload": report,
        },
        {
            "blecap": "record",
            "t_us": 0x00000020,
            "kind": "input",
            "handle": "0x000A",
            "length": 63,
            "captured": 63,
            "payload": report,
        },
        {"blecap": "end", "records": 2, "dropped": 0},
    ]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "capture.jsonl"
        path.write_text(
            "".join(json.dumps(record) + "\n" for record in records),
            encoding="utf-8",
        )
        streams, dropped = read_blecap_jsonl(path)
    assert dropped == 0
    assert len(streams[0x000A]) == 2
    assert abs(streams[0x000A][1].time_seconds - 48e-6) < 1e-12


def test_motionpair_reader() -> None:
    pdu30 = "8750000C0014E61502FAEBB0005C17A8DEE32F005E9292016006C6100002"
    pdu40 = (
        "9470000DC3DD59A8FFC3728CA0E2BB005199814833C4810A10ECFFEF00C0E8"
        "0BA0E0190837430002"
    )
    records = [
        {
            "motionpair": "record",
            "t_us": 0xFFFFFFF0,
            "native_len": 30,
            "native": pdu30,
        },
        {
            "motionpair": "record",
            "t_us": 0x00000020,
            "native_len": 40,
            "native": pdu40,
        },
        {"motionpair": "end", "records": 2, "dropped": 3},
    ]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "capture.jsonl"
        path.write_text(
            "".join(json.dumps(record) + "\n" for record in records),
            encoding="utf-8",
        )
        notifications, dropped = read_motionpair_jsonl(path)
    assert dropped == 3
    assert len(notifications) == 2
    assert notifications[0].value[0x0E] == 0x1E
    assert notifications[1].value[0x0E] == 0x28
    assert abs(notifications[1].time_seconds - 48e-6) < 1e-12


def test_prefix_truncated_carrier_model_real_capture() -> None:
    capture = (
        Path(__file__).resolve().parents[1]
        / "dumps"
        / "BLE CAPTURE"
        / "sw2_native_passthrough_live_2026-07-21.jsonl"
    )
    streams, dropped = read_blecap_jsonl(capture)
    summary = analyze_native(streams[0x000E], dropped)
    fit = summary["prefix"]["carrier_epoch_fit"]
    assert fit["status"] == "diagnostic_predecessor_plus_four_truncated_carrier_fit"
    assert fit["predecessor_offset_ticks"] == 4.0
    assert fit["best_constant_offset_ticks"] == -3.0
    assert fit["mean_abs_correlation"] > 0.9999
    assert fit["mean_fixed_scale_normalized_rmse"] < 0.003
    assert {
        group["length30_state"]
        for group in fit["groups"]
    } == {0, 3}
    for group in fit["groups"]:
        for axis in group["fixed_scale_axes"]:
            assert abs(axis["intercept_quantization_error"]) < 0.001
    history = summary["prefix"]["history_orientation_decode"]
    assert history["state_mismatches"] == 0
    assert history["decoded_records"] == 67
    assert history["angular_error_degrees"]["median"] < 0.005
    assert history["angular_error_degrees"]["max"] < 0.061


def test_predecessor_plus_four_epoch_real_capture() -> None:
    capture = (
        Path(__file__).resolve().parents[1]
        / "dumps"
        / "motion"
        / "2026-07-24"
        / "ds5-pro2-paired-pitch-2026-07-22.jsonl"
    )
    notifications, dropped = read_motionpair_jsonl(capture)
    summary = analyze_native(notifications, dropped)
    fit = summary["prefix"]["carrier_epoch_fit"]
    assert fit["predecessor_offset_ticks"] == 4.0
    assert fit["mean_fixed_scale_normalized_rmse"] < 0.003
    assert (
        fit["best_constant_mean_fixed_scale_normalized_rmse"]
        / fit["mean_fixed_scale_normalized_rmse"]
    ) > 3.0
    history = summary["prefix"]["history_orientation_decode"]
    assert history["state_mismatches"] == 0
    assert history["decoded_records"] == 25
    assert history["angular_error_degrees"]["median"] < 0.001
    assert history["angular_error_degrees"]["max"] < 0.011


def test_reciprocal_chart_transition_captures() -> None:
    root = Path(__file__).resolve().parents[1] / "dumps" / "BLE CAPTURE"
    captures = (
        (
            root / "pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
            3,
            0,
            0,
            (0, 0, 2),
            0,
            0.003,
        ),
        (
            root / "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
            0,
            3,
            0,
            (-2, -1, 2),
            16,
            0.002,
        ),
    )
    for (
        capture,
        before_state,
        after_state,
        prefix_state,
        prefix_windows,
        unit_violations,
        maximum_transition_delta,
    ) in captures:
        notifications, dropped = read_motionpair_jsonl(capture)
        assert dropped == 0
        summary = analyze_native(notifications, dropped)
        audit = summary["carrier_chart"]
        assert len(audit["transitions"]) == 1
        transition = audit["transitions"][0]
        assert transition["before_state"] == before_state
        assert transition["after_state"] == after_state
        assert transition["canonical_frame"] == "state0_boundary_projection"
        assert transition["canonical_delta_norm"] < maximum_transition_delta
        constraint = audit["strict_unit_constraint"]
        assert constraint["violations"] == unit_violations
        seam = audit["prefix_transition_epochs"]
        assert len(seam) == 1
        assert seam[0]["before_state"] == before_state
        assert seam[0]["after_state"] == after_state
        assert seam[0]["selected_state"] == prefix_state
        assert seam[0]["modular_windows"] == prefix_windows
        assert seam[0]["canonical_delta_norm"] < 0.001

    # The reciprocal rapid turn is direct counter-evidence to a literal strict
    # smallest-three decoder: genuine retained energy temporarily exceeds one.
    notifications, dropped = read_motionpair_jsonl(captures[1][0])
    audit = analyze_native(notifications, dropped)["carrier_chart"]
    assert audit["strict_unit_constraint"]["maximum_retained_energy"] > 1.02


def test_state1_chart_transition_capture() -> None:
    capture = (
        Path(__file__).resolve().parents[1]
        / "dumps"
        / "BLE CAPTURE"
        / "pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl"
    )
    notifications, dropped = read_motionpair_jsonl(capture)
    assert dropped == 0
    summary = analyze_native(notifications, dropped)
    audit = summary["carrier_chart"]
    assert audit["state_counts"] == {0: 47, 1: 43}
    assert len(audit["transitions"]) == 1
    transition = audit["transitions"][0]
    assert transition["before_state"] == 0
    assert transition["after_state"] == 1
    assert transition["canonical_frame"] == "state0_boundary_projection"
    assert transition["canonical_delta_norm"] < 0.018
    assert audit["strict_unit_constraint"]["violations"] == 0


def test_rapid_state1_capture_does_not_compose_state0_projections() -> None:
    capture = (
        Path(__file__).resolve().parents[1]
        / "dumps"
        / "BLE CAPTURE"
        / "pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl"
    )
    notifications, dropped = read_motionpair_jsonl(capture)
    assert dropped == 0
    audit = analyze_native(notifications, dropped)["carrier_chart"]
    assert audit["state_counts"] == {0: 43, 1: 1, 3: 42}
    assert [
        (item["before_state"], item["after_state"])
        for item in audit["transitions"]
    ] == [(3, 1), (1, 0)]

    # No state-0 projection may be synthesized for the direct 3->1 edge.
    direct_nonzero = audit["transitions"][0]
    assert "canonical_frame" not in direct_nonzero
    assert "canonical_delta_norm" not in direct_nonzero
    assert direct_nonzero["cyclic_frame"] == "state1_local_projection"
    assert direct_nonzero["cyclic_topology_permutation"] == (2, 1, 0)
    assert direct_nonzero["cyclic_branch"] == "same_omitted_sign"
    assert direct_nonzero["cyclic_delta_norm"] < 0.048

    # The immediately following 1->0 edge is direct counter-evidence to
    # treating the earlier 0->1 unsigned projection as a universal map.
    anchor_edge = audit["transitions"][1]
    assert anchor_edge["canonical_frame"] == "state0_boundary_projection"
    assert anchor_edge["canonical_delta_norm"] > 1.37
    assert anchor_edge["cyclic_branch"] == "opposite_omitted_sign"
    assert anchor_edge["cyclic_delta_norm"] < 0.025

    # The stateful local transition frame can judge the previously suppressed
    # 3->1 prefix without pretending the old state-0 projections compose.
    seams = audit["prefix_transition_epochs"]
    assert len(seams) == 1
    assert seams[0]["before_state"] == 3
    assert seams[0]["after_state"] == 1
    assert seams[0]["local_frame_state"] == 1
    assert seams[0]["selected_state"] == 1
    assert seams[0]["modular_windows"] == (1, 2, -2)
    assert seams[0]["canonical_delta_norm"] < 0.009
    rejected = next(
        candidate
        for candidate in seams[0]["candidates"]
        if candidate["state"] == 3
    )
    assert rejected["canonical_delta_norm"] > 0.24


def test_state2_chart_and_prefix_seam_capture() -> None:
    capture = (
        Path(__file__).resolve().parents[1]
        / "dumps"
        / "BLE CAPTURE"
        / "pro2-chart-transition-3-to-2-2026-07-29.jsonl"
    )
    notifications, dropped = read_motionpair_jsonl(capture)
    assert dropped == 0
    audit = analyze_native(notifications, dropped)["carrier_chart"]
    assert audit["state_counts"] == {0: 25, 1: 19, 2: 26, 3: 23}
    assert [
        (item["before_state"], item["after_state"])
        for item in audit["transitions"]
    ] == [(0, 1), (1, 3), (3, 2), (2, 3)]

    state2_edges = audit["transitions"][2:]
    for transition in state2_edges:
        assert transition["cyclic_frame"] == "state2_local_projection"
        assert transition["cyclic_topology_permutation"] == (2, 0, 1)
        assert transition["cyclic_branch"] == "opposite_omitted_sign"
        assert transition["cyclic_branch_signs"] == (1, -1, -1)
        assert transition["cyclic_delta_norm"] < 0.037
        assert transition["cyclic_branch_margin"] > 0.98

    seams = audit["prefix_transition_epochs"]
    assert len(seams) == 1
    seam = seams[0]
    assert seam["before_state"] == 3
    assert seam["after_state"] == 2
    assert seam["local_frame_state"] == 2
    assert seam["cyclic_branch"] == "opposite_omitted_sign"
    assert seam["selected_state"] == 3
    assert seam["modular_windows"] == (2, 2, -2)
    assert seam["canonical_delta_norm"] < 0.004
    rejected = next(
        candidate
        for candidate in seam["candidates"]
        if candidate["state"] == 2
    )
    assert rejected["canonical_delta_norm"] > 0.19


def test_catchup_reserved_bit_zero_real_corpus() -> None:
    root = Path(__file__).resolve().parents[1] / "dumps" / "BLE CAPTURE"
    captures = (
        root / "sw2_uart_variant7_stationary_2026-07-21.jsonl",
        root / "sw2_uart_variant7_pitch90_2026-07-21.jsonl",
        root / "sw2_uart_variant7_clean_reconnect_2026-07-21.jsonl",
    )
    catchup = []
    for capture in captures:
        streams, _ = read_blecap_jsonl(capture)
        previous_tick = None
        for notification in streams[0x000E]:
            report = notification.value
            if len(report) <= 0x0E or report[0x0E] not in (0x1E, 0x28):
                continue
            length = report[0x0E]
            pdu = report[0x0F:0x0F + length]
            tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
            if length == 0x28:
                sample = decode_motion40(pdu, previous_tick)
                if sample.layout == "catchup":
                    catchup.append(sample)
            previous_tick = tick
    assert len(catchup) == 712
    assert {sample.sensor_status for sample in catchup} == {0x0F}
    assert {sample.tail_width for sample in catchup} == {1}
    assert {sample.tail_value for sample in catchup} == {0}


def main() -> int:
    test_raw_report()
    test_native_catchup_report()
    test_native_high_rate_report()
    test_native_normal_report()
    test_length30_quaternion_decoder()
    test_cadence_layout_boundaries()
    test_exact_fifo_header_rules()
    test_interleaved_native_layout_and_integrity_delta()
    test_normal_layout_keeps_joycon_cadence_boundary()
    test_dropped_capture_keeps_self_contained_layout()
    test_legacy_alias_overlap()
    test_blecap_reader()
    test_motionpair_reader()
    test_prefix_truncated_carrier_model_real_capture()
    test_predecessor_plus_four_epoch_real_capture()
    test_reciprocal_chart_transition_captures()
    test_state1_chart_transition_capture()
    test_rapid_state1_capture_does_not_compose_state0_projections()
    test_state2_chart_and_prefix_seam_capture()
    test_catchup_reserved_bit_zero_real_corpus()
    print("ns2_motion_reference tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
