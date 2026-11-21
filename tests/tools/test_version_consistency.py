#!/usr/bin/env python3
"""One product version, stated once.

`VERSION` at the repository root is the release every artifact belongs to. The
firmware reads it at configure time (firmware/CMakeLists.txt) and reports it in
the management `info` reply. The Android and Windows manifests carry their own
copy because their build systems need a literal; this test is what keeps those
copies equal to the file.

It exists because the firmware reported "2.5" in the 2.6.0 release: the value
was a string literal in config.c that no release step touched.
"""

from __future__ import annotations

import re
import unittest

import _repo  # puts tools/ on sys.path

ROOT = _repo.ROOT


def product_version() -> str:
    return (ROOT / "VERSION").read_text(encoding="utf-8").strip()


class VersionConsistencyTests(unittest.TestCase):
    def test_version_file_is_semver(self) -> None:
        self.assertRegex(product_version(), r"^\d+\.\d+\.\d+$")

    def test_firmware_reads_the_version_file(self) -> None:
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("../VERSION", cmake)
        self.assertIn('PICOSWITCH_VERSION="${PICOSWITCH_VERSION}"', cmake)
        config = (ROOT / "firmware" / "src" / "settings" / "config.c").read_text(
            encoding="utf-8"
        )
        # The `info` reply must format the macro, never a literal version.
        self.assertIn("PICOSWITCH_VERSION,", config)
        self.assertIsNone(re.search(r'\\"version\\":\\"\d', config))

    def test_android_manifest_matches(self) -> None:
        gradle = (
            ROOT / "companions" / "android" / "app" / "build.gradle.kts"
        ).read_text(encoding="utf-8")
        name = re.search(r'versionName\s*=\s*"([^"]+)"', gradle)
        code = re.search(r"versionCode\s*=\s*(\d+)", gradle)
        self.assertIsNotNone(name)
        self.assertIsNotNone(code)
        version = product_version()
        self.assertEqual(name.group(1), version)
        major, minor, patch = (int(part) for part in version.split("."))
        # versionCode scheme documented in build.gradle.kts.
        self.assertEqual(int(code.group(1)), major * 10000 + minor * 100 + patch)

    def test_windows_manifest_matches(self) -> None:
        manifest = (
            ROOT
            / "companions"
            / "windows"
            / "src"
            / "PicoSwitch.Companion.App"
            / "Package.appxmanifest"
        ).read_text(encoding="utf-8")
        match = re.search(r'<Identity\b[^>]*\bVersion="([^"]+)"', manifest, re.S)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), product_version() + ".0")


if __name__ == "__main__":
    unittest.main()
