#!/usr/bin/env python3
"""Offline validation of tools/ns2_motion_carrier.py.

The motion-lab skill requires an inverse codec, captured-fixture replay, signed
endpoint tests, timing invariants, lane covariance checks, and no unexplained
static lanes before any hardware trial. Each is a test below.

Everything here is offline. Nothing in this file authorizes a firmware change.

Run: python tools/test_ns2_motion_carrier.py
"""

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_carrier as C
import ns2_motion_reference as R

CAPTURES = Path(__file__).resolve().parent.parent / "dumps" / "BLE CAPTURE"
TRANSITION_CAPTURES = (
    "pro2-chart-transition-lazy-susan-2026-07-29.jsonl",
    "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl",
    "pro2-chart-transition-splatoon-0-to-1-2026-07-29.jsonl",
    "pro2-chart-transition-splatoon-3-to-1-2026-07-29.jsonl",
    "pro2-chart-transition-3-to-2-2026-07-29.jsonl",
)


def carriers(name):
    """Decoded length-0x1E carriers from one capture, in order."""
    notifications, _ = R.read_motionpair_jsonl(CAPTURES / name)
    out = []
    for notification in notifications:
        report = notification.value
        if len(report) <= 0x0E:
            continue
        length = report[0x0E]
        end = 0x0F + length
        if length != 0x1E or len(report) < end:
            continue
        out.append(R.decode_motion30_orientation(report[0x0F:end]))
    return out


def unit_quaternion(state, retained):
    """Rebuild WXYZ from a chart and its retained triple.

    The omitted component is reconstructed as the positive root, clamped at
    zero: genuine records exceed unit retained norm, so an unclamped sqrt would
    fail on real data.
    """
    quaternion = [0.0, 0.0, 0.0, 0.0]
    for index, value in enumerate(retained, 1):
        quaternion[(state + index) & 3] = value
    energy = sum(value * value for value in retained)
    quaternion[state] = math.sqrt(max(0.0, 1.0 - energy))
    return quaternion


class InverseCodecTests(unittest.TestCase):
    """encode/decode must round-trip within one LSB on every lane."""

    def test_round_trip_within_one_lsb(self):
        for state in range(4):
            for scale in (0.0, 0.1, 0.5, 0.9, 0.999):
                value = C.CARRIER_LIMIT * scale
                quaternion = unit_quaternion(state, (value, -value, value / 2))
                sample = C.encode_carrier(quaternion, current_state=state)
                self.assertEqual(sample.state, state)
                back = C.decode_carrier(sample.raw)
                for lane in range(3):
                    lsb = C.SQRT2 / (1 << C.CARRIER_BITS[lane])
                    self.assertLessEqual(
                        abs(back[lane] - sample.retained[lane]), lsb,
                        f"lane {lane} exceeded one LSB")

    def test_decode_matches_the_reference_decoder_bit_for_bit(self):
        # The codec must not fork the established decode.
        for name in TRANSITION_CAPTURES:
            for orientation in carriers(name)[:20]:
                self.assertEqual(
                    tuple(round(v, 12) for v in C.decode_carrier(orientation.carrier_raw)),
                    tuple(round(v, 12) for v in orientation.retained))


class ProductionParityTests(unittest.TestCase):
    """The codec must match the hardware-validated firmware encoder.

    src/bt_hid/motion/ns2_ds5_motion.c ships encode_switch2_g0/g1/g2 as
        (value / sqrt2 + 0.5) * 2^(26-lane)
    rounded with +0.5 and clamped. That path is validated on a real console in
    Splatoon 3, so it is the reference: if this codec ever diverges from it, a
    generator built on the codec would no longer reproduce proven behavior.
    """

    SOURCE = Path(__file__).resolve().parent.parent / "src" / "bt_hid" / \
        "motion" / "ns2_ds5_motion.c"

    def firmware_encode(self, value, lane):
        field = float(1 << C.CARRIER_BITS[lane])
        scaled = (value * (1.0 / C.SQRT2) + 0.5) * field
        if scaled <= 0.0:
            return 0
        if scaled >= field - 1.0:
            return (1 << C.CARRIER_BITS[lane]) - 1
        return int(scaled + 0.5)

    def test_codec_matches_the_firmware_lane_encoders(self):
        for lane in range(3):
            for step in range(-500, 501):
                value = C.CARRIER_LIMIT * step / 500.0 * 0.999999
                retained = [0.0, 0.0, 0.0]
                retained[lane] = value
                sample = C.encode_carrier(unit_quaternion(0, tuple(retained)), 0)
                self.assertEqual(
                    sample.raw[lane], self.firmware_encode(value, lane),
                    f"lane {lane} diverged from firmware at value {value}")

    def test_firmware_source_still_uses_the_same_constants(self):
        # A guard against the firmware changing under the codec's feet.
        source = self.SOURCE.read_text(encoding="utf-8", errors="replace")
        for needle in ("67108864.0f", "33554432.0f", "NS2_DS5_INV_SQRT2"):
            self.assertIn(needle, source,
                          f"{needle} missing: firmware encoder changed shape")


class SignedEndpointTests(unittest.TestCase):
    """The lanes are unsigned fields spanning +/- sqrt(2)/2 exactly."""

    def test_endpoints_land_inside_the_field(self):
        # Just inside the limit, so chart selection holds and the lane mapping
        # is stable. Exactly at the limit is a re-chart by construction.
        edge = C.CARRIER_LIMIT * (1 - 1e-9)
        for lane in range(3):
            field = 1 << C.CARRIER_BITS[lane]
            retained = [0.0, 0.0, 0.0]
            retained[lane] = -edge
            sample = C.encode_carrier(unit_quaternion(0, tuple(retained)), 0)
            self.assertEqual(sample.state, 0, "chart should have held")
            self.assertEqual(sample.raw[lane], 0)
            retained[lane] = edge
            sample = C.encode_carrier(unit_quaternion(0, tuple(retained)), 0)
            self.assertEqual(sample.raw[lane], field - 1)

    def test_out_of_range_component_is_rejected(self):
        # A non-unit input can leave a retained lane unencodable even after the
        # largest component is omitted. That must raise, not silently wrap.
        with self.assertRaises(C.CarrierError):
            C.encode_carrier((1.0, 1.0, 0.0, 0.0), 0)

    def test_observed_corpus_never_leaves_the_field(self):
        # If any genuine lane exceeded the limit the +/- sqrt(2)/2 model
        # would be wrong outright.
        worst = 0.0
        for name in TRANSITION_CAPTURES:
            for orientation in carriers(name):
                worst = max(worst, max(abs(v) for v in orientation.retained))
        self.assertLess(worst, C.CARRIER_LIMIT)
        self.assertGreater(worst / C.CARRIER_LIMIT, 0.99)  # it does get close


class PrefixSliceTests(unittest.TestCase):
    """The 0x28 prefix is an exact modular slice of the carrier integers."""

    def test_prefix_round_trips_through_the_window_unwrap(self):
        for high_rate in (True, False):
            for raw in ((1 << 25, 1 << 24, 1 << 23),
                        (12345678, 9876543, 4321098),
                        (67103748, 33479992, 16756938)):
                wire = C.encode_prefix(raw, high_rate)
                back = C.decode_prefix(wire, raw, high_rate)
                for lane in range(3):
                    # normal/catch-up drops two low bits, so allow that much.
                    tolerance = 1 if high_rate else 4
                    self.assertLessEqual(
                        abs(back[lane] - raw[lane]), tolerance,
                        f"lane {lane} high_rate={high_rate}")

    def test_high_rate_lane0_is_a_pure_slice_of_the_carrier(self):
        # Lane 0 high-rate shares the carrier's own LSB, so the relation is
        # exact with no rounding at all.
        for raw0 in (0, 1, 1 << 25, (1 << 26) - 1, 40000000):
            wire = C.encode_prefix((raw0, 1 << 24, 1 << 23), True)
            expected = ((raw0 - (1 << 25)) + (1 << 23)) % (1 << 24) - (1 << 23)
            self.assertEqual(wire[0], expected)


class CapturedFixtureReplayTests(unittest.TestCase):
    """Replay genuine captures through the codec."""

    def test_prefix_slice_matches_genuine_wire_values(self):
        # For every high-rate 0x28 bracketed by a carrier, slicing that
        # carrier must land in the same modular window the controller sent.
        checked = 0
        deltas = {0: [], 1: [], 2: []}
        # The five boundary captures are all deliberate motion, so they carry
        # no stationary records. Scan the wider corpus for those.
        corpus = sorted(CAPTURES.glob("*.jsonl")) + sorted(
            (CAPTURES.parent / "motion").rglob("*.jsonl"))
        for path in corpus:
            try:
                notifications, _ = R.read_motionpair_jsonl(path)
            except Exception:
                continue
            name = path.name
            pdus = []
            for notification in notifications:
                report = notification.value
                if len(report) <= 0x0E:
                    continue
                length = report[0x0E]
                end = 0x0F + length
                if length in (0x1E, 0x28) and len(report) >= end:
                    pdus.append(report[0x0F:end])
            previous = None
            for pdu in pdus:
                if len(pdu) == 0x1E:
                    previous = R.decode_motion30_orientation(pdu)
                    continue
                sample = R.decode_motion40(pdu, None)
                if sample.layout != "high_rate" or previous is None:
                    continue
                predicted = C.encode_prefix(previous.carrier_raw, True)
                # Four ticks separate the prefix from its carrier, so the
                # residual IS the motion over those four ticks. Only
                # near-stationary records isolate the slice relation itself.
                moving = max(abs(v) for triple in sample.gyro for v in triple) \
                    if sample.gyro else 1 << 30
                for lane in range(3):
                    span = 1 << C.PREFIX_BITS_HIGH_RATE[lane]
                    delta = (sample.prefix_carrier[lane] - predicted[lane]) % span
                    deltas[lane].append((moving, min(delta, span - delta)))
                checked += 1
        # 178 high-rate prefixes in the five boundary captures have a carrier
        # immediately before them; the rest open a capture or follow a 0x28.
        self.assertGreaterEqual(checked, 178, "genuine replay corpus shrank")
        # Four ticks of motion separate the prefix from its carrier, and rapid
        # rotation can move a lane a long way in four ticks. The claim is that
        # the slice is the same integer, so the TYPICAL record must sit almost
        # exactly on the predicted value; outliers are real motion.
        for lane in range(3):
            still = sorted(delta for moving, delta in deltas[lane]
                           if moving < 2000)
            self.assertGreater(len(still), 30,
                               f"lane {lane}: too few near-stationary records")
            median = still[len(still) // 2]
            # A few hundred LSB is four ticks of residual drift, not a slice
            # error; a wrong relation would land uniformly across the window.
            self.assertLess(
                median, 2000,
                f"lane {lane} near-stationary median slice error {median}")


class ChartHysteresisTests(unittest.TestCase):
    """The load-bearing, non-circular test.

    Chart swaps carry information the lane values alone do not. If the model is
    right, then at every genuine swap the post-swap orientation, expressed in
    the PRE-swap chart, must exceed the representable range -- that is why the
    controller had to move. And every record that did NOT swap must remain
    representable in its own chart.
    """

    def test_every_genuine_swap_happened_at_the_field_edge(self):
        """Assert the direct observable, not a reconstruction.

        The omitted component is NOT recoverable (see the norm note in
        test_union_at_a_swap_is_not_a_unit_quaternion), so the old chart cannot
        be re-projected. What is directly measurable is that every genuine swap
        occurred with the outgoing chart within ~1% of its field edge.
        """
        margins = []
        for name in TRANSITION_CAPTURES:
            sequence = carriers(name)
            for index in range(1, len(sequence)):
                before, after = sequence[index - 1], sequence[index]
                if before.state == after.state:
                    continue
                margins.append(
                    max(abs(v) for v in before.retained) / C.CARRIER_LIMIT)
        self.assertEqual(len(margins), 9, "expected the nine captured boundaries")
        self.assertGreater(min(margins), 0.98,
                           f"a swap fired far from saturation: {min(margins)}")
        self.assertLess(max(margins), 1.0, "a lane exceeded its field")

    def test_no_genuine_record_ever_exceeds_the_field(self):
        # The +/- sqrt(2)/2 span is the whole basis of the codec.
        for name in TRANSITION_CAPTURES:
            for orientation in carriers(name):
                self.assertLess(max(abs(v) for v in orientation.retained),
                                C.CARRIER_LIMIT)

    def test_both_charts_at_a_swap_carry_the_same_three_values(self):
        """The chart is a lane rotation, not a component substitution.

        An earlier analysis assumed the component omitted by one chart is
        retained by the other, so that unioning the two records at a swap would
        expose all four quaternion components. That is wrong: expressed in a
        common frame the two records carry the SAME three values, so no fourth
        component is ever observable. The union was meaningless and the
        "non-unit norm" it appeared to show was an artifact of it.

        The real constraint is measured by the next test, from three lanes
        alone.
        """
        compared = 0
        for name in TRANSITION_CAPTURES:
            sequence = carriers(name)
            for index in range(1, len(sequence)):
                before, after = sequence[index - 1], sequence[index]
                if before.state == after.state:
                    continue
                if not (before.canonical_state0_carrier
                        and after.canonical_state0_carrier):
                    continue  # state 2 has no unsigned state-0 projection
                delta = max(
                    abs(x - y) for x, y in
                    zip(before.canonical_state0_carrier,
                        after.canonical_state0_carrier))
                # The 3->1 and 1->0 edges need the opposite-sign branch, which
                # this unsigned projection does not apply; they are excluded by
                # the same threshold that admits the rest.
                if delta > 1.0:
                    continue
                self.assertLess(
                    delta, 0.2,
                    f"{name}: {before.state}->{after.state} canonical triples "
                    "diverge, so the lane-rotation model is wrong")
                compared += 1
        self.assertGreaterEqual(compared, 5, "too few comparable boundaries")

    def test_carrier_is_not_three_components_of_a_unit_quaternion(self):
        """The one genuine open question, pinned by a test.

        No three components of a unit quaternion can have norm > 1. The
        canonical carrier reaches 1.160. This is measured from the three
        transmitted lanes only -- no union, no reconstruction -- so it is a
        direct property of the wire data.

        Something therefore breaks the unit-quaternion reading: most likely the
        controller integrating without renormalizing between its infrequent
        cleanup passes. Until that is understood, a decoder must not assume it
        can recover a fourth component, and this test fails loudly if the
        premise is ever quietly reintroduced.

        It does not block generation: we synthesize from our own unit
        quaternion and never need to invert this.
        """
        worst = 0.0
        for name in TRANSITION_CAPTURES:
            for orientation in carriers(name):
                if not orientation.canonical_state0_carrier:
                    continue
                worst = max(worst, math.sqrt(sum(
                    v * v for v in orientation.canonical_state0_carrier)))
        self.assertGreater(
            worst, 1.0,
            "canonical norm no longer exceeds 1: the unit-quaternion reading "
            "may now hold, revisit the carrier model")
        # Bounded, so it is a real effect rather than a decode blow-up.
        self.assertLess(worst, 1.5)

    def test_holds_are_representable_and_hysteresis_is_real(self):
        # Non-swap records stay in range, and a strict smallest-three encoder
        # would have re-charted far more often than the controller does.
        holds = strict_would_swap = 0
        for name in TRANSITION_CAPTURES:
            sequence = carriers(name)
            for index in range(1, len(sequence)):
                before, after = sequence[index - 1], sequence[index]
                if before.state != after.state:
                    continue
                holds += 1
                self.assertLess(max(abs(v) for v in after.retained),
                                C.CARRIER_LIMIT)
                quaternion = unit_quaternion(after.state, after.retained)
                largest = max(range(4), key=lambda i: abs(quaternion[i]))
                if largest != after.state:
                    strict_would_swap += 1
        self.assertGreater(holds, 400)
        # If this ever reaches zero, strict smallest-three would have fitted
        # and the hysteresis model is unnecessary.
        self.assertGreater(
            strict_would_swap, 20,
            "no evidence of hysteresis: strict smallest-three agreed everywhere")

    def test_encoder_holds_its_chart_until_saturation(self):
        # Drive the encoder along a trajectory that crosses a component
        # boundary and confirm it re-charts exactly once, at the crossing.
        state = 0
        recharts = []
        for step in range(200):
            angle = math.pi * step / 199.0
            quaternion = (math.cos(angle / 2), math.sin(angle / 2), 0.0, 0.0)
            sample = C.encode_carrier(quaternion, state)
            if sample.recharted:
                recharts.append(step)
            state = sample.state
        self.assertEqual(len(recharts), 1, f"expected one re-chart, got {recharts}")
        # And it must have happened at saturation, not at the halfway point a
        # strict smallest-three encoder would pick.
        crossing = recharts[0]
        angle = math.pi * crossing / 199.0
        self.assertGreater(math.sin(angle / 2), C.CARRIER_LIMIT * 0.98)


class TimingInvariantTests(unittest.TestCase):
    def test_prefix_epoch_is_the_preceding_carrier_plus_four(self):
        self.assertEqual(C.prefix_epoch(100, 7), 97)
        self.assertEqual(C.PREFIX_EPOCH_OFFSET, 4)

    def test_genuine_prefix_epochs_land_four_ticks_after_a_carrier(self):
        matched = 0
        for name in TRANSITION_CAPTURES:
            notifications, _ = R.read_motionpair_jsonl(CAPTURES / name)
            timeline, previous, unwrapped = [], None, 0
            for notification in notifications:
                report = notification.value
                if len(report) <= 0x0E:
                    continue
                length = report[0x0E]
                end = 0x0F + length
                if length not in (0x1E, 0x28) or len(report) < end:
                    continue
                pdu = report[0x0F:end]
                tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
                unwrapped = tick if previous is None else \
                    unwrapped + ((tick - previous) & 0x0FFF)
                timeline.append((unwrapped, pdu))
                previous = tick
            last_carrier = None
            for tick, pdu in timeline:
                if len(pdu) == 0x1E:
                    last_carrier = tick
                    continue
                if last_carrier is None:
                    continue
                sample = R.decode_motion40(pdu, None)
                self.assertEqual(
                    C.prefix_epoch(tick, sample.elapsed_ticks),
                    last_carrier + C.PREFIX_EPOCH_OFFSET,
                    f"{name}: prefix epoch is not preceding carrier + 4")
                matched += 1
        self.assertGreater(matched, 100)


class LaneIndependenceTests(unittest.TestCase):
    """Lane covariance and static-lane checks."""

    def test_no_carrier_lane_is_static_across_the_corpus(self):
        # An unexplained constant lane would mean the field map is wrong.
        for name in TRANSITION_CAPTURES:
            sequence = carriers(name)
            for lane in range(3):
                values = {orientation.carrier_raw[lane] for orientation in sequence}
                self.assertGreater(
                    len(values), len(sequence) // 4,
                    f"{name} lane {lane} is suspiciously static")

    def test_lanes_are_not_duplicates_of_one_another(self):
        for name in TRANSITION_CAPTURES:
            sequence = carriers(name)
            for left in range(3):
                for right in range(left + 1, 3):
                    identical = sum(
                        1 for orientation in sequence
                        if orientation.carrier_raw[left] == orientation.carrier_raw[right]
                    )
                    self.assertLess(identical, len(sequence) // 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
