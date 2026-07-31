#!/usr/bin/env python3

from __future__ import annotations

import unittest

import audio_lab_analyze


def document(first: dict, last: dict) -> dict:
    base = {
        "audio": True,
        "usb_active": True,
        "pcm_dropped": 0,
        "pcm_short": 0,
        "opus_errors": 0,
        "pipeline_resets": 0,
        "sends": 0,
        "opus_frames": 0,
        "core1_stack_free": 20000,
    }
    return {
        "schema": "picoswitch2-audio-samples/v1",
        "samples": [
            {"elapsed_ms": 0, "audio": base | first},
            {"elapsed_ms": 1000, "audio": base | last},
        ],
    }


class AudioLabAnalyzeTests(unittest.TestCase):
    def test_clean_run_passes(self) -> None:
        result = audio_lab_analyze.analyze(
            document(
                {"sends": 10, "opus_frames": 20},
                {"sends": 110, "opus_frames": 220},
            )
        )
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(result["deltas"]["sends"], 100)

    def test_pcm_drop_is_hard_failure(self) -> None:
        result = audio_lab_analyze.analyze(
            document({"pcm_dropped": 2}, {"pcm_dropped": 5})
        )
        self.assertEqual(result["verdict"], "FAIL")
        self.assertIn("3 packet", result["failures"][0])

    def test_reset_and_low_stack_are_observations(self) -> None:
        result = audio_lab_analyze.analyze(
            document(
                {"pipeline_resets": 1, "core1_stack_free": 9000},
                {"pipeline_resets": 2, "core1_stack_free": 7000},
            )
        )
        self.assertEqual(result["verdict"], "OBSERVE")
        self.assertEqual(len(result["warnings"]), 2)

    def test_counter_decrease_is_not_wrapped_silently(self) -> None:
        result = audio_lab_analyze.analyze(
            document({"sends": 100}, {"sends": 5})
        )
        self.assertIsNone(result["deltas"]["sends"])
        self.assertTrue(any("reset/wrap" in item for item in result["warnings"]))


if __name__ == "__main__":
    unittest.main()
