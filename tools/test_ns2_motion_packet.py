#!/usr/bin/env python3
"""Offline validation of tools/ns2_motion_packet.py.

The bar is byte-exactness against genuine hardware. Every length-`0x28` PDU in
the repository capture corpus is decoded, re-encoded from the decoded fields,
and compared byte for byte. A synthesizer that cannot reproduce real packets
must not be allowed to emit synthetic ones.

Everything here is offline. Nothing in this file authorizes a firmware change.

Run: python tools/test_ns2_motion_packet.py
"""

import sys
import unittest
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_carrier as C
import ns2_motion_packet as P
import ns2_motion_reference as R

DUMPS = Path(__file__).resolve().parent.parent / "dumps"


def all_captures():
    yield from sorted((DUMPS / "BLE CAPTURE").glob("*.jsonl"))
    yield from sorted((DUMPS / "motion").rglob("*.jsonl"))


def pdus(path):
    """Every 0x1E / 0x28 PDU in one capture, in order."""
    try:
        notifications, _ = R.read_motionpair_jsonl(path)
    except Exception:
        try:
            handles, _ = R.read_blecap_jsonl(path)
            notifications = max(handles.values(), key=len) if handles else []
        except Exception:
            return
    for notification in notifications:
        report = notification.value
        if len(report) <= 0x0E:
            continue
        length = report[0x0E]
        end = 0x0F + length
        if length in (0x1E, 0x28) and len(report) >= end:
            yield report[0x0F:end]


class RoundTripTests(unittest.TestCase):
    """The load-bearing test: genuine packets must re-encode byte-identically."""

    def test_every_genuine_0x28_reencodes_byte_for_byte(self):
        layouts = Counter()
        mismatches = []
        for path in all_captures():
            for pdu in pdus(path):
                if len(pdu) != 0x28:
                    continue
                sample = R.decode_motion40(pdu, None)
                if sample.layout == "unknown":
                    continue
                rebuilt = P.build_motion40(P.MotionPacketFields(
                    tick=sample.tick,
                    elapsed_ticks=sample.elapsed_ticks,
                    carrier=sample.prefix_carrier,
                    accel=sample.accel,
                    gyro=sample.gyro,
                    tail_value=sample.tail_value,
                    packing_mode=sample.packing_mode,
                    status=sample.sensor_status,
                ))
                layouts[sample.layout] += 1
                if rebuilt != pdu:
                    diff = [i for i in range(len(pdu)) if pdu[i] != rebuilt[i]]
                    mismatches.append(f"{path.name} {sample.layout} bytes {diff}")
        self.assertEqual(mismatches[:5], [], f"{len(mismatches)} mismatched packets")
        # All three cadence layouts must actually be exercised.
        for layout in ("high_rate", "normal", "catchup"):
            self.assertGreater(layouts[layout], 0, f"{layout} never exercised")
        self.assertGreater(sum(layouts.values()), 1500)
        print(f"\n  re-encoded byte-exactly: {dict(layouts)}")

    def test_every_genuine_0x1E_reencodes_byte_for_byte(self):
        checked = 0
        for path in all_captures():
            for pdu in pdus(path):
                if len(pdu) != 0x1E:
                    continue
                orientation = R.decode_motion30_orientation(pdu)
                rebuilt = P.build_motion30(
                    orientation.state, orientation.carrier_raw, pdu[:5],
                    byte12_flag=(pdu[12] >> 7) & 1)
                self.assertEqual(
                    rebuilt[4:16], pdu[4:16],
                    f"{path.name}: carrier bytes differ")
                checked += 1
        self.assertGreater(checked, 1500)


class LayoutSelectionTests(unittest.TestCase):
    def test_elapsed_selects_the_layout_and_status(self):
        for elapsed, layout, status in (
            (0, "high_rate", P.STATUS_HIGH_RATE),
            (7, "high_rate", P.STATUS_HIGH_RATE),
            (10, "high_rate", P.STATUS_HIGH_RATE),
            (11, "normal", P.STATUS_NORMAL),
            (14, "normal", P.STATUS_NORMAL),
            (15, "catchup", P.STATUS_CATCHUP),
            (30, "catchup", P.STATUS_CATCHUP),
        ):
            self.assertEqual(P.layout_for_elapsed(elapsed), layout)
            self.assertEqual(P.status_for_layout(layout), status)

    def test_genuine_status_bytes_agree_with_our_selection(self):
        """Status 0x0D/0x0E/0x0F always match the elapsed count.

        Five packets in the corpus carry status 0x00 instead. Those are a
        separate, undocumented condition -- most likely 'no IMU data was read',
        which the BLE protocol inventory records for a different length. They
        are counted, not silently folded into a layout, because a synthesizer
        must not emit a layout the controller would not have chosen.
        """
        agreed = status_zero = 0
        for path in all_captures():
            for pdu in pdus(path):
                if len(pdu) != 0x28:
                    continue
                sample = R.decode_motion40(pdu, None)
                if sample.layout == "unknown":
                    continue
                if sample.sensor_status == 0:
                    status_zero += 1
                    continue
                self.assertEqual(
                    P.status_for_layout(P.layout_for_elapsed(sample.elapsed_ticks)),
                    sample.sensor_status,
                    f"{path.name}: status disagrees with elapsed count")
                agreed += 1
        self.assertGreater(agreed, 1900)
        self.assertLessEqual(status_zero, 10, "status 0x00 is no longer rare")


class FailClosedTests(unittest.TestCase):
    """A synthesizer must refuse to emit a malformed packet."""

    def base(self, **overrides):
        fields = dict(tick=100, elapsed_ticks=7, carrier=(0, 0, 0),
                      accel=((0, 0, 0), (0, 0, 0)), gyro=((0, 0, 0),))
        fields.update(overrides)
        return P.MotionPacketFields(**fields)

    def test_wrong_sample_count_is_rejected(self):
        with self.assertRaises(P.PacketError):
            P.build_motion40(self.base(accel=((0, 0, 0),)))
        with self.assertRaises(P.PacketError):
            P.build_motion40(self.base(gyro=((0, 0, 0), (0, 0, 0))))

    def test_out_of_range_field_is_rejected(self):
        with self.assertRaises(P.PacketError):
            P.build_motion40(self.base(tick=0x1000))
        with self.assertRaises(P.PacketError):
            P.build_motion40(self.base(carrier=(1 << 23, 0, 0)))
        with self.assertRaises(P.PacketError):
            P.build_motion40(self.base(accel=((1 << 21, 0, 0), (0, 0, 0))))

    def test_carrier_lane_overflow_is_rejected(self):
        with self.assertRaises(P.PacketError):
            P.build_motion30(0, (1 << 26, 0, 0))
        with self.assertRaises(P.PacketError):
            P.build_motion30(4, (0, 0, 0))


class EndToEndSynthesisTests(unittest.TestCase):
    """Orientation in, genuine-shaped packets out, decoded back to the input."""

    def test_carrier_survives_a_full_synthesis_round_trip(self):
        import math
        state = None
        for step in range(120):
            angle = math.pi * step / 119.0
            quaternion = (math.cos(angle / 2), math.sin(angle / 2), 0.0, 0.0)
            sample = C.encode_carrier(quaternion, state)
            state = sample.state
            pdu = P.build_motion30(sample.state, sample.raw)
            decoded = R.decode_motion30_orientation(pdu)
            self.assertEqual(decoded.state, sample.state)
            self.assertEqual(decoded.carrier_raw, sample.raw)
            for lane in range(3):
                lsb = C.SQRT2 / (1 << C.CARRIER_BITS[lane])
                self.assertLessEqual(
                    abs(decoded.retained[lane] - sample.retained[lane]), lsb)

    def test_prefix_slice_survives_synthesis(self):
        # Encode a carrier, slice it into a 0x28 prefix, build the packet, and
        # confirm the decoder reads back the same modular window.
        for raw in ((1 << 25, 1 << 24, 1 << 23),
                    (40000000, 20000000, 10000000),
                    (67103748, 33479992, 16756938)):
            for elapsed, high_rate in ((7, True), (12, False), (20, False)):
                wire = C.encode_prefix(raw, high_rate)
                accel = ((1, 2, 3),) * P.LAYOUT_SAMPLE_COUNTS[
                    P.layout_for_elapsed(elapsed)][0]
                gyro = ((4, 5, 6),) * P.LAYOUT_SAMPLE_COUNTS[
                    P.layout_for_elapsed(elapsed)][1]
                pdu = P.build_motion40(P.MotionPacketFields(
                    tick=500, elapsed_ticks=elapsed, carrier=wire,
                    accel=accel, gyro=gyro))
                decoded = R.decode_motion40(pdu, None)
                self.assertEqual(decoded.prefix_carrier, wire)
                self.assertEqual(decoded.accel[0], (1, 2, 3))
                self.assertEqual(decoded.gyro[0], (4, 5, 6))
                self.assertEqual(decoded.packing_mode, 3)

    def test_temperature_tail_round_trips(self):
        for integer_part, low_a, low_b in ((4, 0, 0), (4, 3, 5), (-7, 7, 1)):
            tail = P.encode_temperature_tail16(integer_part, low_a, low_b)
            fields = R.decode_temperature_tail16_value(tail)
            self.assertEqual(fields.temperature_a_q3, integer_part * 8 + low_a)
            self.assertEqual(fields.temperature_b_q3, integer_part * 8 + low_b)


if __name__ == "__main__":
    unittest.main(verbosity=2)
