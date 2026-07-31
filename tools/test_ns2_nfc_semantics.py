#!/usr/bin/env python3
"""Host tests for tools/ns2_nfc_semantics.py.

Two kinds of coverage:

* unit tests pinning each capture-derived layout, so a decoder edit that
  silently changes a field name or an offset fails here rather than in the
  middle of a hardware session;
* regression tests over real captures committed under ``dumps/``. These assert
  the specific facts the v3 investigation had to learn the expensive way --
  the escalated 4-block descriptor, the 83-byte device result, the
  allocation-relative King Dedede records, and the error-to-operation pairing.

Run: python tools/test_ns2_nfc_semantics.py
"""

import json
import unittest
from pathlib import Path

import ns2_nfc_semantics as nfc
import ns2_trace

DUMPS = Path(__file__).resolve().parent.parent / "dumps"


def bulk(sequence, subcommand, data, response=False, direction_byte=None):
    """Build one trace record around an NFC bulk payload."""
    if direction_byte is None:
        direction_byte = 0x01 if response else 0x91
    header = bytes([0x01, direction_byte, 0x00, subcommand, 0x00,
                    min(len(data), 0xFF), 0x00, 0x00])
    payload = header + data
    return {
        "trace": "record",
        "seq": sequence,
        "t_us": sequence * 1000,
        "personality": "pro2",
        "kind": "bulk_response" if response else "bulk_command",
        "dir": "device_to_console" if response else "console_to_device",
        "id": 0x01,
        "sub": subcommand,
        "length": len(payload),
        "captured": len(payload),
        "payload": payload.hex().upper(),
    }


def load_capture(name):
    with (DUMPS / name).open("r", encoding="utf-8") as stream:
        return ns2_trace.read_trace(stream, name).records


def timeline_for(name):
    return nfc.build_timeline(load_capture(name))


class LayoutTests(unittest.TestCase):
    def test_read_descriptor_timeout_is_not_a_marker(self):
        # D0 07 is 2000 ms, not a "D0 07 marker". Gating on the literal bytes
        # rejected the 3000 ms escalated descriptor and stalled every v3 read.
        request = bytes.fromhex("B80B" + "00" * 7 + "01" + "04" +
                                "003B" + "3C77" + "7891" + "E2E6")
        fields = nfc.decode_read_descriptor(request)
        self.assertEqual(fields["timeout_ms"], 3000)
        self.assertEqual(fields["uid_selector"], "any")
        self.assertEqual(fields["blocks"],
                         ["0x00-0x3B", "0x3C-0x77", "0x78-0x91", "0xE2-0xE6"])
        # 0xE6 + 1 pages * 4 = 924 bytes, well past the 540-byte view.
        self.assertEqual(fields["highest_byte"], 924)
        self.assertTrue(fields["needs_v3_source"])

    def test_plain_540_descriptor_does_not_need_the_v3_source(self):
        request = bytes.fromhex("D007" + "00" * 7 + "01" + "03" +
                                "003B" + "3C77" + "7886")
        fields = nfc.decode_read_descriptor(request)
        self.assertEqual(fields["highest_byte"], 540)
        self.assertFalse(fields["needs_v3_source"])

    def test_operation_prefix_separates_chip_identity_from_signature(self):
        prefix = bytearray(60)
        prefix[0] = 0x04
        prefix[7] = 0x07
        prefix[8:15] = bytes.fromhex("049011CADB1F90")
        prefix[18] = 0x06
        prefix[19:51] = bytes(range(0x40, 0x60))
        prefix[51:60] = bytes.fromhex("03003B3C77788600" + "00")
        fields = nfc.decode_operation_prefix(bytes(prefix) + bytes(540))
        self.assertEqual(fields["chip_identity_name"], "ntag_i2c_2k_v3")
        self.assertEqual(fields["uid"], "049011CADB1F90")
        self.assertTrue(fields["signature_present"])
        self.assertEqual(fields["tag_bytes"], 540)

    def test_status_states_15_16_18_are_reported_as_bodiless(self):
        for state in (0x15, 0x16, 0x18):
            fields = nfc.decode_status(bytes([state]) + bytes(60))
            self.assertEqual(fields["state_name"], nfc.NFC_STATES[state])
            self.assertFalse(fields["identity_present"])

    def test_status_with_identity_exposes_the_uid(self):
        payload = bytearray(61)
        payload[0] = 0x09
        payload[4], payload[5], payload[6], payload[8] = 0x01, 0x01, 0x02, 0x07
        payload[9:16] = bytes.fromhex("049011CADB1F90")
        fields = nfc.decode_status(bytes(payload))
        self.assertEqual(fields["state_name"], "tag_present")
        self.assertEqual(fields["uid"], "049011CADB1F90")

    def test_device_command_envelope(self):
        data = bytearray(nfc.DEVICE_COMMAND_SIZE)
        data[0:2] = (2000).to_bytes(2, "little")
        data[2:9] = bytes.fromhex("049011CADB1F90")
        data[9], data[10] = 0x01, 0x01
        fields = nfc.classify_stage(bytes(data))
        self.assertEqual(fields["envelope"], "device_command")
        self.assertEqual(fields["completed_by"], "0x21")

    def test_extended_update_reads_its_allocation_from_the_envelope(self):
        # King Dedede's allocation: sector-0 page 0xB2, sector-1 capability
        # page 0x64 with the data record at 0x65. A fixed Kirby table (0x92,
        # 0x00/0x01) rejected this and produced 2115-0096.
        data = bytearray(nfc.EXTENDED_UPDATE_SIZE)
        data[2:9] = bytes.fromhex("0465B0228F2190")
        data[9], data[10] = 0x01, 0x06
        data[11], data[12] = 0x01, 0x01
        data[13] = 0x64
        data[14:18] = b"\xFF\xFF\xFF\xFF"
        data[18:22] = bytes.fromhex("A5000200")
        data[22], data[23], data[24], data[25] = 0x03, 0x00, 0x04, 0x04
        data[30], data[31], data[32] = 0x00, 0xB2, 0x20
        data[65], data[66], data[67] = 0x01, 0x65, 0x60
        fields = nfc.classify_stage(bytes(data))
        self.assertEqual(fields["envelope"], "extended_update")
        self.assertEqual(fields["sector0_page"], "0xB2")
        self.assertEqual(fields["sector1_capability_page"], "0x64")
        self.assertEqual(fields["records"],
                         ["s0:0x04+4", "s0:0xB2+32", "s1:0x65+96"])
        self.assertNotIn("malformed", fields)

    def test_ordinary_write_records_are_parsed(self):
        data = bytearray(nfc.WRITE_STAGING_SIZE)
        data[2:9] = bytes.fromhex("049011CADB1F90")
        data[9], data[10] = 0x01, 0x06
        data[21] = 2
        data[22], data[23] = 0x05, 32
        data[56], data[57] = 0x30, 16
        fields = nfc.classify_stage(bytes(data))
        self.assertEqual(fields["envelope"], "write")
        self.assertEqual(fields["records"], ["s0:0x05+32", "s0:0x30+16"])
        self.assertEqual(fields["data_bytes"], 48)


class ReassemblyTests(unittest.TestCase):
    def test_chunked_stage_is_reassembled_before_classification(self):
        body = bytearray(nfc.DEVICE_COMMAND_SIZE)
        body[2:9] = bytes.fromhex("049011CADB1F90")
        body[9], body[10] = 0x01, 0x01
        records = []
        for index, offset in enumerate((0, 40)):
            piece = bytes(body[offset:offset + 40])
            request = (offset.to_bytes(2, "little") +
                       len(piece).to_bytes(2, "little") + piece)
            records.append(bulk(index, 0x14, request))
        records.append(bulk(2, 0x21, b""))
        steps, warnings = nfc.build_timeline(records)
        self.assertEqual(warnings, [])
        stage = next(step for step in steps if step.kind == "stage")
        self.assertEqual(stage.fields["envelope"], "device_command")
        self.assertEqual(stage.fields["staged_bytes"], nfc.DEVICE_COMMAND_SIZE)
        self.assertEqual(stage.fields["completed_by_observed"], "0x21")

    def test_declared_length_mismatch_is_flagged_as_a_transport_fault(self):
        # An 88-byte 0x14 read into a 64-byte buffer split into two commands
        # and crashed the console with 2168-0002. The decoder must not quietly
        # accept a short body.
        request = (0).to_bytes(2, "little") + (74).to_bytes(2, "little") + bytes(40)
        steps, warnings = nfc.build_timeline([bulk(0, 0x14, request)])
        self.assertTrue(any("declared 74" in warning for warning in warnings))

    def test_error_state_is_paired_with_the_operation_in_flight(self):
        records = [bulk(0, 0x20, b""),
                   bulk(1, 0x05, bytes([0x07, 0x41]) + bytes(59), response=True)]
        steps, _ = nfc.build_timeline(records)
        failures, removals = nfc.error_context(steps)
        self.assertEqual(len(failures), 1)
        self.assertIn("commit_extended", failures[0])
        self.assertEqual(removals, [])

    def test_removal_edge_is_not_reported_as_a_failure(self):
        # 07/41 after a Stop that followed a committed write is how the
        # controller signals logical removal. Hardware confirmed it: a trace
        # containing exactly this pattern reported errors=0 over UART.
        records = [
            bulk(0, 0x03, b""),                                    # poll
            bulk(1, 0x08, b""),                                    # commit
            bulk(2, 0x05, bytes([0x05]) + bytes(60), response=True),
            bulk(3, 0x04, b""),                                    # stop
            bulk(4, 0x05, bytes([0x07, 0x41]) + bytes(59), response=True),
        ]
        steps, _ = nfc.build_timeline(records)
        failures, removals = nfc.error_context(steps)
        self.assertEqual(failures, [])
        self.assertEqual(len(removals), 1)
        self.assertIn("tag removed", removals[0])

    def test_error_after_a_stop_without_a_commit_is_still_a_failure(self):
        records = [
            bulk(0, 0x03, b""),
            bulk(1, 0x04, b""),
            bulk(2, 0x05, bytes([0x07, 0x41]) + bytes(59), response=True),
        ]
        steps, _ = nfc.build_timeline(records)
        failures, removals = nfc.error_context(steps)
        self.assertEqual(len(failures), 1)
        self.assertEqual(removals, [])


@unittest.skipUnless(DUMPS.is_dir(), "captures are not available")
class CaptureRegressionTests(unittest.TestCase):
    """Assertions against real committed captures, not synthetic bytes."""

    def test_recognition_capture_shows_escalation_and_device_result(self):
        steps, warnings = timeline_for("v3-RECOGNIZED-2026-07-27.jsonl")
        self.assertEqual(warnings, [])
        kinds = [step.kind for step in steps]
        self.assertIn("execute_device_command", kinds)

        escalated = [step for step in steps if step.kind == "begin_read"
                     and step.fields.get("needs_v3_source")]
        self.assertTrue(escalated, "no escalated v3 descriptor in the capture")
        self.assertEqual(escalated[0].fields["blocks"],
                         ["0x00-0x3B", "0x3C-0x77", "0x78-0x91", "0xE2-0xE6"])

        # The device result is a 19-byte header plus the tag's complete
        # 64-byte SRAM response. Reading it as "32 bytes then zeros" is what
        # made downloaded dumps look unusable.
        results = [step for step in steps if step.kind == "read_buffer"
                   and step.fields.get("result_type") == "0x18"]
        self.assertTrue(results, "no device result buffer")
        self.assertEqual(results[0].fields["total_bytes"], 19 + 64)

        for step in steps:
            if step.kind == "read_buffer":
                self.assertEqual(step.fields["chip_identity_name"], "ntag_i2c_2k_v3")

    def test_genuine_write_capture_decodes_both_extended_operations(self):
        steps, warnings = timeline_for(
            "amiibo/genuine-kirby-warp-air-riders-write-usb-2026-07-28.jsonl")
        self.assertEqual(warnings, [])
        envelopes = [step.fields["envelope"] for step in steps if step.kind == "stage"]
        self.assertIn("extended_update", envelopes)
        self.assertIn("write", envelopes)

        update = next(step for step in steps if step.kind == "stage"
                      and step.fields["envelope"] == "extended_update")
        self.assertEqual(update.fields["completed_by_observed"], "0x20")
        self.assertEqual(update.fields["records"],
                         ["s0:0x04+4", "s0:0x92+32", "s1:0x01+96"])

        ordinary = next(step for step in steps if step.kind == "stage"
                        and step.fields["envelope"] == "write")
        self.assertEqual(ordinary.fields["completed_by_observed"], "0x08")
        self.assertEqual(ordinary.fields["records"],
                         ["s0:0x05+32", "s0:0x30+240", "s0:0x6C+152"])

    def test_dedede_failure_names_the_allocation_and_the_failing_operation(self):
        steps, _ = timeline_for("amiibo/v3-dedede-fix1-2115-0096-2026-07-28.jsonl")
        update = next(step for step in steps if step.kind == "stage"
                      and step.fields["envelope"] == "extended_update")
        # The whole Dedede diagnosis, straight off a capture that already
        # existed: a different, self-described allocation.
        self.assertEqual(update.fields["sector0_page"], "0xB2")
        self.assertEqual(update.fields["sector1_capability_page"], "0x64")

        failures, removals = nfc.error_context(steps)
        self.assertTrue(failures)
        self.assertIn("commit_extended", failures[0])
        # These are real rejections, not removal edges: no write ever committed.
        self.assertEqual(removals, [])

    def test_540_capture_still_decodes_without_v3_escalation(self):
        # This capture predates full NFC payload retention, so its records are
        # tracer-truncated. That must degrade the decode gracefully rather than
        # look like a protocol fault.
        steps, warnings = timeline_for("amiibo-540-working-read-2026-07-26.jsonl")
        self.assertEqual(warnings, [])
        reads = [step for step in steps if step.kind == "read_buffer"]
        self.assertTrue(reads)
        for step in reads:
            self.assertTrue(step.fields.get("capture_truncated"))
        # The declared chunk lengths still reconstruct the true buffer sizes:
        # two 600-byte NTAG215 operation buffers and one 64-byte write-prep
        # buffer from the targeted page-3 read.
        self.assertEqual([step.fields["total_bytes"] for step in reads],
                         [600, 600, 64])
        for step in reads:
            if step.fields["total_bytes"] == 600:
                self.assertEqual(step.fields["chip_identity_name"], "ntag215")
        for step in steps:
            if step.kind == "begin_read":
                self.assertFalse(step.fields["needs_v3_source"])


class TraceIntegrationTests(unittest.TestCase):
    def test_ns2_trace_names_nfc_subcommands_from_the_shared_module(self):
        # One vocabulary. If these drift, two tools describe the same byte
        # differently and a comparison stops meaning anything.
        for subcommand, name in nfc.NFC_SUBCOMMANDS.items():
            self.assertEqual(ns2_trace.command_name(0x01, subcommand),
                             f"nfc.{name}")

    def test_per_record_decode_exposes_status_fields(self):
        payload = bytearray(61)
        payload[0] = 0x18
        record = bulk(0, 0x05, bytes(payload), response=True)
        fields = ns2_trace.semantic_record(record)
        self.assertEqual(fields["state_name"], "device_result_ready")


if __name__ == "__main__":
    unittest.main()
