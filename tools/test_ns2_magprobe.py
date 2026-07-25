#!/usr/bin/env python3
"""Host tests for tools/ns2_magprobe.py."""

import io
import json
import math
import unittest

import ns2_magprobe


PDU30 = bytes.fromhex(
    "3C44000C0006E2ED01436CE70044BF863BBC1E00707F24FDE07F98100002"
)
PDU40 = bytes.fromhex(
    "1978000D439093F789F1535C77FDC50002433DD026F4EEFFE3FBFFFCFE3F950C"
    "2030D4AB62428001"
)


def record(timestamp, pdu):
    return {
        "motionpair": "record",
        "t_us": timestamp,
        "native_len": len(pdu),
        "native": pdu.hex().upper(),
        "ds5_valid": False,
        "ds5_seq": 0,
        "ds5_t_us": 0,
        "ds5_age_us": 0,
        "ds5_sensor": 0,
        "cal_state": 0,
        "raw_g": [0, 0, 0],
        "raw_a": [0, 0, 0],
        "cal_g": [0, 0, 0],
        "cal_a": [0, 0, 0],
    }


def capture_stream(records, dropped=0):
    lines = [json.dumps(item, separators=(",", ":")) for item in records]
    lines.append(json.dumps({
        "motionpair": "end",
        "records": len(records),
        "dropped": dropped,
    }, separators=(",", ":")))
    return io.StringIO("\n".join(lines))


def ble_capture_stream(pdus, dropped=0):
    records = []
    for index, pdu in enumerate(pdus):
        report = bytearray(63)
        report[14] = len(pdu)
        report[15:15 + len(pdu)] = pdu
        records.append({
            "blecap": "record",
            "t_us": 100 + index * 7500,
            "kind": "input",
            "handle": "0x000E",
            "length": 63,
            "captured": 63,
            "payload": bytes(report).hex().upper(),
        })
    records.append({
        "blecap": "end",
        "records": len(pdus),
        "dropped": dropped,
        "variant": 9,
    })
    return io.StringIO("\n".join(
        json.dumps(item, separators=(",", ":")) for item in records
    ))


def rawmag_ble_capture_stream(vectors, dropped=0):
    records = []
    for index, vector in enumerate(vectors):
        report = bytearray(63)
        for offset, value in zip((25, 27, 29), vector):
            report[offset:offset + 2] = int(value).to_bytes(
                2, "little", signed=True
            )
        records.append({
            "blecap": "record",
            "t_us": 200 + index * 7500,
            "kind": "input",
            "handle": "0x000A",
            "length": 63,
            "captured": 63,
            "payload": bytes(report).hex().upper(),
        })
    records.append({
        "blecap": "end",
        "records": len(vectors),
        "dropped": dropped,
        "variant": 10,
    })
    return io.StringIO("\n".join(
        json.dumps(item, separators=(",", ":")) for item in records
    ))


class MagprobeTests(unittest.TestCase):
    def test_decodes_known_orientation_carriers(self):
        carriers = ns2_magprobe.decode_orientation_carriers(PDU30)
        self.assertEqual(carriers, (0x01EDE206, 0x00E76C43, 0x0086BF44))
        omitted, rebuilt, quaternion = ns2_magprobe.decode_quaternion(PDU30)
        self.assertEqual(omitted, 0)
        self.assertEqual(rebuilt, carriers)
        self.assertAlmostEqual(sum(value * value for value in quaternion), 1.0, places=6)

    def test_decodes_known_normal_0x28_fields(self):
        tick, sub_index = ns2_magprobe.decode_timing(PDU40)
        self.assertEqual((tick, sub_index), (2073, 7))
        self.assertEqual(
            ns2_magprobe.decode_unknown_middle_28(PDU40),
            (-801291518, -1117146, -50332701),
        )
        magnetic = ns2_magprobe.decode_magnetic(PDU40)
        expected = (
            0.01737765140322374,
            -0.06050736442139182,
            0.36673018131171964,
        )
        for actual, wanted in zip(magnetic, expected):
            self.assertAlmostEqual(actual, wanted, places=12)
        self.assertEqual(
            ns2_magprobe.decode_g678_raw(PDU40),
            (51539, -179454, 271914),
        )
        candidate = ns2_magprobe.decode_magneto_quaternion_candidate(PDU40)
        self.assertAlmostEqual(
            sum(value * value for value in candidate), 1.0, places=12
        )
        self.assertGreater(candidate[0], 0.0)
        with self.assertRaisesRegex(ns2_magprobe.MagprobeError, "length-30"):
            ns2_magprobe.decode_accel_1e(PDU40)

    def test_capture_validation_and_timestamp_wrap(self):
        capture = ns2_magprobe.read_capture(capture_stream([
            record(0xFFFFFFF0, PDU30),
            record(5, PDU40),
        ]))
        self.assertEqual(capture.records[0].elapsed_us, 0)
        self.assertEqual(capture.records[1].elapsed_us, 21)
        self.assertEqual(capture.dropped, 0)

        bad = record(1, PDU40)
        bad["native_len"] = 30
        with self.assertRaisesRegex(ns2_magprobe.MagprobeError, "hex length"):
            ns2_magprobe.read_capture(capture_stream([bad]))

    def test_summary_separates_normal_and_escalation(self):
        escalation = bytearray(PDU40)
        escalation[2] = 1
        escalation[3] = 0x0F
        capture = ns2_magprobe.read_capture(capture_stream([
            record(100, PDU30),
            record(7600, PDU40),
            record(15100, bytes(escalation)),
        ]))
        summary = ns2_magprobe.summarize(capture)
        self.assertEqual(summary["length_30"], 1)
        self.assertEqual(summary["length_40"], 2)
        self.assertEqual(summary["normal_28"], 1)
        self.assertEqual(summary["escalation_28"], 1)
        self.assertTrue(math.isfinite(summary["magnetic_norm"]["mean"]))

    def test_compare_movement_gate(self):
        baseline = ns2_magprobe.summarize(ns2_magprobe.read_capture(
            capture_stream([record(100, PDU30), record(7600, PDU40)]),
            "baseline",
        ))
        moved30 = bytearray(PDU30)
        # A valid state-0 carrier with a materially different G0 component.
        carriers = list(ns2_magprobe.decode_orientation_carriers(moved30))
        carriers[0] += 4_000_000
        moved30[5] = carriers[0] & 0xFF
        moved30[6] = (carriers[0] >> 8) & 0xFF
        moved30[7] = (carriers[0] >> 16) & 0xFF
        moved30[8] = (moved30[8] & 0xFC) | ((carriers[0] >> 24) & 3)
        stimulus = ns2_magprobe.summarize(ns2_magprobe.read_capture(
            capture_stream([record(100, bytes(moved30)), record(7600, PDU40)]),
            "stimulus",
        ))
        _, result = ns2_magprobe.compare_summaries(baseline, stimulus)
        self.assertTrue(result["movement_warning"])
        self.assertGreater(result["quaternion_angle_degrees"], 1.0)

    def test_reads_blecap_and_corpus_rejects_raw_int16_wire_format(self):
        capture = ns2_magprobe.read_ble_capture(
            ble_capture_stream([PDU30, PDU40], dropped=3),
            "ble",
        )
        self.assertEqual(capture.capture_format, "blecap")
        self.assertEqual(capture.dropped, 3)
        self.assertEqual([len(record.native) for record in capture.records], [30, 40])

        corpus = ns2_magprobe.summarize_corpus([capture])
        self.assertEqual(corpus["normal_28"], 1)
        self.assertFalse(corpus["all_samples_fit_signed_int16"])
        self.assertTrue(corpus["all_norms_at_most_one"])
        self.assertTrue(
            corpus["interpretation"]["raw_ak09919c_wire_format_rejected"]
        )

    def test_reads_and_summarizes_separate_rawmag_channel(self):
        capture = ns2_magprobe.read_rawmag_ble_capture(
            rawmag_ble_capture_stream([
                (123, -456, 789),
                (124, -454, 790),
                (122, -455, 792),
            ], dropped=2),
            "rawmag",
        )
        self.assertEqual(capture.dropped, 2)
        self.assertEqual(capture.records[0].axes, (123, -456, 789))
        summary = ns2_magprobe.summarize_rawmag(capture)
        self.assertEqual(summary["records"], 3)
        self.assertEqual(summary["nonzero_records"], 3)
        self.assertEqual(summary["unique_vectors"], 3)
        self.assertEqual(summary["axes"][0]["minimum"], 122)
        self.assertEqual(summary["axes"][1]["maximum"], -454)
        self.assertEqual(summary["maximum_step"], [2, 2, 2])
        self.assertTrue(summary["interpretation"]["varying_data"])

    def test_stationary_epochs_separate_live_and_candidate_drift(self):
        baseline = ns2_magprobe.read_capture(capture_stream([
            record(100, PDU30), record(7600, PDU40),
        ]), "epoch0")
        moved30 = bytearray(PDU30)
        carriers = list(ns2_magprobe.decode_orientation_carriers(moved30))
        carriers[0] += 3_000_000
        moved30[5] = carriers[0] & 0xFF
        moved30[6] = (carriers[0] >> 8) & 0xFF
        moved30[7] = (carriers[0] >> 16) & 0xFF
        moved30[8] = (moved30[8] & 0xFC) | ((carriers[0] >> 24) & 3)
        later = ns2_magprobe.read_capture(capture_stream([
            record(100, bytes(moved30)), record(7600, PDU40),
        ]), "epoch1")
        summary = ns2_magprobe.summarize_epochs([baseline, later])
        self.assertGreater(summary["final_live_delta_degrees"], 1.0)
        self.assertAlmostEqual(summary["final_candidate_delta_degrees"], 0.0)
        self.assertTrue(
            summary["interpretation"]["candidate_more_stable_than_live"]
        )


if __name__ == "__main__":
    unittest.main()
