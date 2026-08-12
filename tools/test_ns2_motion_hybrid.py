#!/usr/bin/env python3

from __future__ import annotations

import unittest
import json
import tempfile
from pathlib import Path

import ns2_motion_hybrid as H
import ns2_motion_packet as K


MOTION30_A = bytes.fromhex(
    "3C44000C0006E2ED01436CE70044BF863BBC1E00707F24FDE07F98100002"
)
MOTION30_B = bytes.fromhex(
    "7A34000C0001020300040506000708090000100000200000300000400000"
)


def motion40(tick: int, elapsed: int, seed: int) -> bytes:
    layout = K.layout_for_elapsed(elapsed)
    accel_count, gyro_count = K.LAYOUT_SAMPLE_COUNTS[layout]
    accel = tuple(
        tuple(seed + slot * 3 + axis for axis in range(3))
        for slot in range(accel_count)
    )
    gyro = tuple(
        tuple(seed * 2 + slot * 3 + axis for axis in range(3))
        for slot in range(gyro_count)
    )
    return K.build_motion40(K.MotionPacketFields(
        tick=tick,
        elapsed_ticks=elapsed,
        carrier=(seed, -seed, seed + 1),
        accel=accel,
        gyro=gyro,
        tail_value=0 if layout == "catchup" else 0x01C0,
        packing_mode=3,
        status=K.status_for_layout(layout),
    ))


class PartitionTests(unittest.TestCase):
    def test_every_bit_has_exactly_one_owner(self) -> None:
        packets = [MOTION30_A]
        packets.extend(motion40(100, elapsed, 20) for elapsed in (8, 12, 20))
        for packet in packets:
            groups = H.packet_groups(packet)
            total = sum(span.width for ranges in groups.values() for span in ranges)
            self.assertEqual(total, len(packet) * 8)

    def test_motion30_unknown_flag_is_not_part_of_prefix(self) -> None:
        prefix = H.group_mask(MOTION30_A, "prefix")
        flags = H.group_mask(MOTION30_A, "flags_reserved")
        self.assertEqual(prefix & flags, 0)
        self.assertTrue(flags & (1 << (12 * 8 + 7)))


class FitTests(unittest.TestCase):
    def test_matching_layout_fits_but_does_not_claim_physical_alignment(self) -> None:
        result = H.structural_fit(motion40(100, 8, 10), motion40(108, 8, 30))
        self.assertTrue(result["structural_fit"])
        self.assertFalse(result["physical_alignment_proven"])
        self.assertEqual(result["layout"], "high_rate")

    def test_length_mismatch_is_rejected(self) -> None:
        with self.assertRaises(H.HybridError):
            H.structural_fit(MOTION30_A, motion40(100, 8, 10))

    def test_layout_mismatch_is_rejected(self) -> None:
        with self.assertRaises(H.HybridError):
            H.structural_fit(motion40(100, 8, 10), motion40(112, 12, 10))


class SpliceTests(unittest.TestCase):
    def test_one_group_changes_no_unselected_bit(self) -> None:
        base = motion40(100, 8, 10)
        donor = motion40(108, 8, 30)
        output = H.splice(base, donor, ["gyro"])
        mask = H.group_mask(base, "gyro")
        left = int.from_bytes(base, "little")
        right = int.from_bytes(donor, "little")
        mixed = int.from_bytes(output, "little")
        self.assertEqual((mixed ^ left) & ~mask, 0)
        self.assertEqual(mixed & mask, right & mask)

    def test_all_groups_reproduce_donor_exactly(self) -> None:
        base = motion40(100, 8, 10)
        donor = motion40(108, 8, 30)
        output = H.splice(base, donor, list(H.packet_groups(base)))
        self.assertEqual(output, donor)

    def test_motion30_prefix_preserves_unexplained_flag(self) -> None:
        base = bytearray(MOTION30_A)
        donor = bytearray(MOTION30_B)
        base[12] |= K.MOTION30_BYTE12_FLAG
        donor[12] &= ~K.MOTION30_BYTE12_FLAG
        output = H.splice(bytes(base), bytes(donor), ["prefix"])
        self.assertTrue(output[12] & K.MOTION30_BYTE12_FLAG)

    def test_unknown_group_is_rejected(self) -> None:
        with self.assertRaises(H.HybridError):
            H.splice(MOTION30_A, MOTION30_B, ["muffler_bearing"])


class CaptureAuditTests(unittest.TestCase):
    def _capture(self, root: Path, *, delta: bytes, reason: str = "applied") -> Path:
        base = motion40(100, 8, 10)
        records = [
            {
                "motionhybrid": "record",
                "t_us": 1000,
                "native_len": 40,
                "mode": "gyro",
                "reason": reason,
                "requested_groups": H.GROUP_BITS["gyro"],
                "changed_bits": sum(byte.bit_count() for byte in delta),
                "ds5_age_us": 100,
                "ds5_seq": 9,
                "cal_state": 2,
                "pose_aligned": True,
                "base": base.hex().upper(),
                "output_xor": delta.hex().upper(),
            },
            {"motionhybrid": "end", "records": 1, "dropped": 0},
        ]
        path = root / "capture.jsonl"
        path.write_text(
            "\n".join(json.dumps(record) for record in records) + "\n",
            encoding="utf-8",
        )
        return path

    def test_selected_group_capture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = motion40(100, 8, 10)
            donor = motion40(100, 8, 30)
            output = H.splice(base, donor, ["gyro"])
            delta = bytes(left ^ right for left, right in zip(base, output))
            audit = H.audit_live_capture(self._capture(root, delta=delta))
            self.assertTrue(audit["fail_closed"])
            self.assertEqual(audit["applied"], 1)

    def test_interleaved_motion30_prefix_capture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = H.splice(MOTION30_A, MOTION30_B, ["prefix"])
            delta = bytes(
                left ^ right for left, right in zip(MOTION30_A, output)
            )
            records = [
                {
                    "motionhybrid": "record",
                    "t_us": 1000,
                    "native_len": 30,
                    "mode": "prefix",
                    "reason": "applied",
                    "requested_groups": H.GROUP_BITS["prefix"],
                    "changed_bits": sum(byte.bit_count() for byte in delta),
                    "ds5_age_us": 100,
                    "ds5_seq": 9,
                    "cal_state": 2,
                    "pose_aligned": True,
                    "base": MOTION30_A.hex().upper(),
                    "output_xor": delta.hex().upper(),
                },
                {"motionhybrid": "end", "records": 1, "dropped": 0},
            ]
            path = root / "capture30.jsonl"
            path.write_text(
                "\n".join(json.dumps(record) for record in records) + "\n",
                encoding="utf-8",
            )
            audit = H.audit_live_capture(path)
            self.assertTrue(audit["fail_closed"])
            self.assertEqual(audit["applied"], 1)

    def test_unselected_change_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            delta = bytes([1] + [0] * 39)  # tick bit, never gyro-owned
            with self.assertRaisesRegex(H.HybridError, "unselected"):
                H.audit_live_capture(self._capture(root, delta=delta))

    def test_fallback_must_be_byte_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            delta = bytes([0] * 39 + [1])
            with self.assertRaisesRegex(H.HybridError, "non-applied"):
                H.audit_live_capture(
                    self._capture(root, delta=delta, reason="wait_pose")
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
