#!/usr/bin/env python3

from __future__ import annotations

import unittest

from audit_firmware_capture_space import SECTOR_SIZE, audit


class FirmwareCaptureSpaceTests(unittest.TestCase):
    def test_current_sized_image_leaves_candidate_region(self) -> None:
        result = audit(
            image_size=0xE4000,
            flash_size=0x400000,
            capture_size=0x100000,
            guard_sectors=1,
        )
        self.assertTrue(result["fits_current_binary"])
        self.assertEqual(result["capture_end"], 0x3FA000)
        self.assertEqual(result["capture_start"], 0x2FA000)
        self.assertTrue(result["candidate_only"])

    def test_overlap_fails_closed(self) -> None:
        result = audit(
            image_size=0x310000,
            flash_size=0x400000,
            capture_size=0x100000,
            guard_sectors=1,
        )
        self.assertFalse(result["fits_current_binary"])

    def test_unaligned_capture_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            audit(
                image_size=SECTOR_SIZE,
                flash_size=0x400000,
                capture_size=0x100001,
                guard_sectors=1,
            )


if __name__ == "__main__":
    unittest.main()
