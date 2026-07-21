#!/usr/bin/env python3
"""Host tests for tools/ns2_trace.py."""

import io
import json
import unittest

import ns2_trace


def record(sequence, timestamp, kind, direction, identifier, subcommand, data):
    return {
        "trace": "record",
        "seq": sequence,
        "t_us": timestamp,
        "personality": "pro2",
        "kind": kind,
        "dir": direction,
        "id": identifier,
        "sub": subcommand,
        "length": len(data),
        "captured": min(len(data), 24),
        "payload": data[:24].hex().upper(),
    }


def stream_for(records, overwritten=0):
    lines = [json.dumps(item, separators=(",", ":")) for item in records]
    lines.append(json.dumps({
        "trace": "end", "records": len(records), "overwritten": overwritten,
    }, separators=(",", ":")))
    return io.StringIO("\n".join(lines))


class TraceToolTests(unittest.TestCase):
    def test_decode_known_handshake_fields_and_redacts_identity(self):
        setup = bytes.fromhex("C003000000004000")
        identity = bytes.fromhex("010048454A373130303131323132343700007E0569200106")
        memory = bytes.fromhex("0291000400080000407E000080300100")
        firmware = bytes.fromhex("1001000100F80000020104020C00000000020300")
        capture = ns2_trace.read_trace(stream_for([
            record(0, 0xFFFFFFF0, "ep0_setup", "console_to_device", 3, 0, setup),
            record(1, 5, "ep0_response", "device_to_console", 3, 0, identity),
            record(2, 35, "bulk_command", "console_to_device", 2, 4, memory),
            record(3, 65, "bulk_response", "device_to_console", 16, 1, firmware),
        ]))
        output = ns2_trace.render_decode(capture)
        self.assertIn("EP0 SETUP identity", output)
        self.assertIn("vid=0x057E pid=0x2069", output)
        self.assertNotIn("HEJ71001121247", output)
        self.assertIn("memory_address=0x00013080 memory_length=64", output)
        self.assertIn("controller_version=2.1.4", output)
        self.assertIn("bluetooth_version=12.0.0", output)
        self.assertIn("dsp_version=0.2.3", output)
        self.assertIn("   0.021", output)  # uint32 timestamp wrap is handled

    def test_rejects_sequence_and_payload_corruption(self):
        first = record(0, 10, "hid_output", "console_to_device", 2, 0, b"\x00")
        second = record(2, 20, "hid_output", "console_to_device", 2, 0, b"\x01")
        with self.assertRaisesRegex(ns2_trace.TraceError, "does not follow"):
            ns2_trace.read_trace(stream_for([first, second]))

        first["payload"] = "GG"
        with self.assertRaisesRegex(ns2_trace.TraceError, "not hexadecimal"):
            ns2_trace.read_trace(stream_for([first]))

    def test_rejects_missing_end_and_header_summary_disagreement(self):
        packet = bytes.fromhex("0391000D000800000100000000000000")
        item = record(0, 10, "bulk_command", "console_to_device", 3, 13, packet)
        with self.assertRaisesRegex(ns2_trace.TraceError, "end record is missing"):
            ns2_trace.read_trace(io.StringIO(json.dumps(item)))

        item["id"] = 4
        with self.assertRaisesRegex(ns2_trace.TraceError, "disagrees with payload"):
            ns2_trace.read_trace(stream_for([item]))

    def test_semantic_diff_ignores_pairing_material_unless_strict(self):
        left_packet = bytes.fromhex("159100020011000000") + bytes(range(16))
        right_packet = bytes.fromhex("159100020011000000") + bytes(range(16, 32))
        left = ns2_trace.read_trace(stream_for([
            record(0, 100, "bulk_command", "console_to_device", 0x15, 2, left_packet),
        ]))
        right = ns2_trace.read_trace(stream_for([
            record(0, 200, "bulk_command", "console_to_device", 0x15, 2, right_packet),
        ]))
        output, identical = ns2_trace.render_diff(left, right, "left", "right")
        self.assertTrue(identical)
        self.assertIn("changed=0", output)

        output, identical = ns2_trace.render_diff(left, right, "left", "right", strict=True)
        self.assertFalse(identical)
        self.assertIn("raw_payload", output)

    def test_semantic_diff_keys_memory_reads_by_address(self):
        read_a = bytes.fromhex("0291000400080000407E000080300100")
        read_b = bytes.fromhex("0291000400080000407E0000C0300100")
        left = ns2_trace.read_trace(stream_for([
            record(0, 1, "bulk_command", "console_to_device", 2, 4, read_a),
        ]))
        right = ns2_trace.read_trace(stream_for([
            record(0, 1, "bulk_command", "console_to_device", 2, 4, read_b),
        ]))
        output, identical = ns2_trace.render_diff(left, right, "left", "right")
        self.assertFalse(identical)
        self.assertIn("deleted=1", output)
        self.assertIn("inserted=1", output)


if __name__ == "__main__":
    unittest.main()
