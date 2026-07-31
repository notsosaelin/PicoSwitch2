#!/usr/bin/env python3
"""Host tests for tools/amiibo_corpus.py.

Everything here is synthetic. The analyzer must classify a corpus without a
retail key, without a network, and without any of the maintainer's dumps, so
the tests build their own images in a temporary directory.

Run: python tools/test_amiibo_corpus.py
"""

import tempfile
import unittest
from pathlib import Path

import amiibo_corpus as corpus


def make_v3(uid: bytes, body_marker: int, sram_marker: int) -> bytes:
    """A structurally valid, unwritten v3 image with a correct SRAM CRC."""
    image = bytearray(corpus.V3_SIZE)
    image[0:7] = uid
    image[7], image[8] = 0x00, 0x44
    # Encrypted body stand-in: the rider axis.
    image[0x54:0x5C] = bytes([body_marker]) * 8
    image[0x100:0x140] = bytes([body_marker]) * 0x40
    # Machine axis lives entirely inside the SRAM window.
    sram = bytearray(corpus.V3_SRAM_SIZE)
    sram[0:corpus.V3_SRAM_DATA_SIZE] = bytes([sram_marker]) * corpus.V3_SRAM_DATA_SIZE
    checksum = corpus.crc16_mcrf4xx(bytes(sram[:corpus.V3_SRAM_DATA_SIZE]))
    sram[corpus.V3_SRAM_DATA_SIZE:] = checksum.to_bytes(2, "big")
    image[corpus.V3_SRAM_OFFSET:corpus.V3_SRAM_OFFSET + corpus.V3_SRAM_SIZE] = sram
    return bytes(image)


def make_ntag215() -> bytes:
    image = bytearray(540)
    image[0:3] = b"\x04\x11\x22"
    image[3] = 0x88 ^ image[0] ^ image[1] ^ image[2]
    image[4:8] = b"\x33\x44\x55\x66"
    image[8] = image[4] ^ image[5] ^ image[6] ^ image[7]
    return bytes(image)


class ClassificationTests(unittest.TestCase):
    def test_sram_crc_matches_the_firmware_implementation(self):
        # Pinned so a refactor of crc16_mcrf4xx cannot silently diverge from
        # ns2_amiibo_v3_crc16_mcrf4xx in src/nfc/ns2_amiibo_v3.c.
        self.assertEqual(corpus.crc16_mcrf4xx(b"123456789"), 0x6F91)

    def test_valid_v3_is_runtime_safe_and_reports_its_sram(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.bin").write_bytes(make_v3(b"\x04\x90\x11\xCA\xDB\x1F\x90", 1, 2))
            image = corpus.load_image(root / "a.bin", root)
            self.assertTrue(image.safe)
            self.assertEqual(image.fields["format"], "v3")
            self.assertEqual(image.fields["uid"], "049011CADB1F90")
            self.assertTrue(image.fields["sram_crc_valid"])
            self.assertEqual(image.fields["allocation"], "unwritten")

    def test_corrupt_sram_crc_is_reported_without_failing_the_image(self):
        # A stale research dump must stay analyzable; the CRC is informational,
        # not an admission gate.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytearray(make_v3(b"\x04\x00\x00\x00\x00\x00\x01", 1, 2))
            data[corpus.V3_SRAM_OFFSET + corpus.V3_SRAM_DATA_SIZE] ^= 0xFF
            (root / "a.bin").write_bytes(bytes(data))
            image = corpus.load_image(root / "a.bin", root)
            self.assertTrue(image.safe)
            self.assertFalse(image.fields["sram_crc_valid"])

    def test_bad_chip_header_is_unsafe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytearray(make_v3(b"\x04\x00\x00\x00\x00\x00\x02", 1, 2))
            data[8] = 0x00  # not the NTAG I2C 2K 0x44
            (root / "a.bin").write_bytes(bytes(data))
            image = corpus.load_image(root / "a.bin", root)
            self.assertFalse(image.safe)
            self.assertIn("NTAG I2C 2K header", image.issues[0])

    def test_non_amiibo_binaries_are_skipped_not_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "spi.bin").write_bytes(bytes(4096))
            image = corpus.load_image(root / "spi.bin", root)
            self.assertTrue(image.safe)
            self.assertIn("skipped", image.fields)

    def test_written_allocation_is_discovered_not_assumed(self):
        # King Dedede's capability page is 0x64, not Kirby's 0x00.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytearray(make_v3(b"\x04\x65\xB0\x22\x8F\x21\x90", 3, 4))
            data[corpus.V3_SECTOR1_OFFSET + 0x64 * 4:
                 corpus.V3_SECTOR1_OFFSET + 0x64 * 4 + 4] = b"\xA5\x00\x02\x00"
            data[0xB2 * 4:0xB2 * 4 + 32] = bytes(range(32))
            (root / "a.bin").write_bytes(bytes(data))
            image = corpus.load_image(root / "a.bin", root)
            self.assertEqual(image.fields["allocation"], {
                "sector1_capability_page": "0x64",
                "sector1_data_page": "0x65",
                "generation": 2,
            })
            # One 32-byte record reports one start page, not the eight
            # overlapping pages a sliding window would produce.
            self.assertEqual(image.fields["sector0_record_pages"], ["0xB2"])
            self.assertEqual(image.fields["extended_state"], "written")

    def test_records_without_a_retained_capability_page_are_flagged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytearray(make_v3(b"\x04\x00\x00\x00\x00\x00\x03", 5, 6))
            data[0x92 * 4:0x92 * 4 + 32] = bytes(range(32))
            (root / "a.bin").write_bytes(bytes(data))
            image = corpus.load_image(root / "a.bin", root)
            self.assertEqual(image.fields["allocation"], "indeterminate")
            self.assertTrue(image.fields["notes"])


class GroupingTests(unittest.TestCase):
    def test_rider_and_machine_axes_are_grouped_independently(self):
        # The whole point: four riders x four machines is sixteen files but
        # only four distinct bodies and four distinct SRAM windows. Collapsing
        # them into one hash is what made a single figure look representative.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = []
            for rider in range(4):
                for machine in range(4):
                    uid = bytes([0x04, rider, 0, 0, 0, 0, 1])
                    path = root / f"r{rider}m{machine}.bin"
                    path.write_bytes(make_v3(uid, rider + 1, machine + 1))
                    images.append(corpus.load_image(path, root))
            corpus.group_images(images)
            manifest = corpus.build_manifest(root, images, {})
            self.assertEqual(manifest["summary"]["images"], 16)
            self.assertEqual(manifest["summary"]["body_groups"], 4)
            self.assertEqual(manifest["summary"]["sram_groups"], 4)
            self.assertEqual(manifest["summary"]["unsafe"], 0)
            pairs = {(image.fields["body_group"], image.fields["sram_group"])
                     for image in images}
            self.assertEqual(len(pairs), 16)

    def test_shared_uids_are_surfaced_as_a_sampling_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            uid = b"\x04\xB4\x43\x8A\xDB\x1F\x90"
            images = []
            for machine in range(3):
                path = root / f"m{machine}.bin"
                path.write_bytes(make_v3(uid, 1, machine + 1))
                images.append(corpus.load_image(path, root))
            corpus.group_images(images)
            manifest = corpus.build_manifest(root, images, {})
            shared = manifest["summary"]["shared_uids"]
            self.assertIn("04B4438ADB1F90", shared)
            self.assertEqual(len(shared["04B4438ADB1F90"]), 3)

    def test_ntag215_images_classify_alongside_v3(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "old.bin").write_bytes(make_ntag215())
            image = corpus.load_image(root / "old.bin", root)
            self.assertTrue(image.safe)
            self.assertEqual(image.fields["format"], "ntag215")
            self.assertEqual(image.fields["uid"], "0411223344556 6".replace(" ", ""))


class RunTests(unittest.TestCase):
    def test_main_exits_zero_on_a_clean_corpus(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.bin").write_bytes(make_v3(b"\x04\x00\x00\x00\x00\x00\x04", 1, 1))
            self.assertEqual(corpus.main([str(root), "--quiet"]), 0)

    def test_main_exits_one_when_an_image_is_unsafe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytearray(make_v3(b"\x04\x00\x00\x00\x00\x00\x05", 1, 1))
            data[0] = 0x00
            (root / "a.bin").write_bytes(bytes(data))
            self.assertEqual(corpus.main([str(root), "--quiet"]), 1)


if __name__ == "__main__":
    unittest.main()
