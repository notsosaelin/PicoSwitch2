#!/usr/bin/env python3
"""Decode Android BTSNOOZ/btsnoop captures and summarize ACL lifetimes.

The input may be either a raw btsnoop file or an Android bugreport containing
``BTSNOOP_LOG_SUMMARY``.  Output is deliberately limited to the host/controller
HCI boundary; it must not be interpreted as an over-the-air capture.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import io
import json
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path
from typing import BinaryIO, Iterator


BTSNOOP_MAGIC = b"btsnoop\x00"
BTSNOOP_EPOCH_DELTA_US = 0x00DC_DDB3_0F2F_8000

TYPE_IN_EVT = 0x10
TYPE_IN_ACL = 0x11
TYPE_IN_SCO = 0x12
TYPE_IN_ISO = 0x17
TYPE_OUT_CMD = 0x20
TYPE_OUT_ACL = 0x21
TYPE_OUT_SCO = 0x22
TYPE_OUT_ISO = 0x2D

H4_COMMAND = 0x01
H4_ACL = 0x02
H4_SCO = 0x03
H4_EVENT = 0x04
H4_ISO = 0x05

EVENT_CONNECTION_COMPLETE = 0x03
EVENT_DISCONNECTION_COMPLETE = 0x05
EVENT_HARDWARE_ERROR = 0x10
EVENT_COMMAND_COMPLETE = 0x0E
EVENT_COMMAND_STATUS = 0x0F
EVENT_LE_META = 0x3E
EVENT_VENDOR = 0xFF

LE_CONNECTION_COMPLETE = 0x01
LE_CONNECTION_UPDATE_COMPLETE = 0x03
LE_ENHANCED_CONNECTION_COMPLETE = 0x0A

OPCODE_NAMES = {
    0x0405: "Create Connection",
    0x0406: "Disconnect",
    0x040D: "Read Remote Version Information",
    0x0C03: "Reset",
    0x1401: "Read Failed Contact Counter",
    0x1403: "Get Link Quality",
    0x1405: "Read RSSI",
    0x1406: "Read AFH Channel Map",
    0x200D: "LE Create Connection",
    0x2013: "LE Connection Update",
    0x2043: "LE Extended Create Connection",
    0xFD5E: "Bluetooth Quality Report Configure",
}

EVENT_NAMES = {
    EVENT_CONNECTION_COMPLETE: "Connection Complete",
    EVENT_DISCONNECTION_COMPLETE: "Disconnection Complete",
    EVENT_HARDWARE_ERROR: "Hardware Error",
    EVENT_COMMAND_COMPLETE: "Command Complete",
    EVENT_COMMAND_STATUS: "Command Status",
    EVENT_LE_META: "LE Meta",
    EVENT_VENDOR: "Vendor Specific",
}


def _h4_for_snooz_type(packet_type: int) -> bytes:
    if packet_type == TYPE_OUT_CMD:
        return bytes((H4_COMMAND,))
    if packet_type in (TYPE_IN_ACL, TYPE_OUT_ACL):
        return bytes((H4_ACL,))
    if packet_type in (TYPE_IN_SCO, TYPE_OUT_SCO):
        return bytes((H4_SCO,))
    if packet_type == TYPE_IN_EVT:
        return bytes((H4_EVENT,))
    if packet_type in (TYPE_IN_ISO, TYPE_OUT_ISO):
        return bytes((H4_ISO,))
    raise ValueError(f"unknown BTSNOOZ packet type 0x{packet_type:02X}")


def _direction_for_snooz_type(packet_type: int) -> int:
    return int(packet_type in (TYPE_IN_EVT, TYPE_IN_ACL, TYPE_IN_SCO, TYPE_IN_ISO))


def decode_btsnooz(report: bytes) -> bytes:
    """Return a standard btsnoop stream from an Android bugreport."""
    begin_marker = b"--- BEGIN:BTSNOOP_LOG_SUMMARY"
    end_marker = b"--- END:BTSNOOP_LOG_SUMMARY"
    begin = report.find(begin_marker)
    if begin < 0:
        raise ValueError("bugreport has no BTSNOOP_LOG_SUMMARY")
    begin = report.find(b"\n", begin)
    end = report.find(end_marker, begin)
    if begin < 0 or end < 0:
        raise ValueError("bugreport has an incomplete BTSNOOP_LOG_SUMMARY")

    encoded = b"".join(report[begin:end].split())
    snooz = base64.b64decode(encoded, validate=True)
    if len(snooz) < 10:
        raise ValueError("BTSNOOZ payload is truncated")
    version, last_timestamp_ms = struct.unpack_from("=bQ", snooz)
    if version not in (1, 2):
        raise ValueError(f"unsupported BTSNOOZ version {version}")
    decompressed = zlib.decompress(snooz[9:])

    records: list[tuple[int, int, int, bytes]] = []
    offset = 0
    elapsed_ms = 0
    while offset < len(decompressed):
        if version == 1:
            included, delta_ms, snooz_type = struct.unpack_from("=HIb", decompressed, offset)
            original = included
            offset += 7
        else:
            included, original, delta_ms, snooz_type = struct.unpack_from(
                "=HHIb", decompressed, offset
            )
            offset += 9
        payload_length = included - 1
        if payload_length < 0 or offset + payload_length > len(decompressed):
            raise ValueError("BTSNOOZ record is truncated")
        payload = _h4_for_snooz_type(snooz_type) + decompressed[offset : offset + payload_length]
        offset += payload_length
        elapsed_ms += delta_ms
        records.append((original, included, snooz_type, payload))

    # Keep AOSP's historical field naming: both the preamble timestamp and
    # per-record deltas are written directly into the microsecond btsnoop
    # timestamp domain despite their ``*_ms`` names.
    first_timestamp_us = last_timestamp_ms + BTSNOOP_EPOCH_DELTA_US - elapsed_ms
    output = io.BytesIO()
    output.write(BTSNOOP_MAGIC)
    output.write(struct.pack(">II", 1, 1002))

    # Re-read just the deltas for a faithful second pass.
    offset = 0
    timestamp_us = first_timestamp_us
    for original, included, snooz_type, payload in records:
        if version == 1:
            _, delta_ms, _ = struct.unpack_from("=HIb", decompressed, offset)
            offset += 7 + included - 1
        else:
            _, _, delta_ms, _ = struct.unpack_from("=HHIb", decompressed, offset)
            offset += 9 + included - 1
        timestamp_us += delta_ms
        output.write(struct.pack(">IIIIQ", original, included, _direction_for_snooz_type(snooz_type), 0, timestamp_us))
        output.write(payload)
    return output.getvalue()


def load_capture(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw.startswith(BTSNOOP_MAGIC):
        return raw
    return decode_btsnooz(raw)


def iter_records(data: bytes) -> Iterator[dict[str, object]]:
    stream: BinaryIO = io.BytesIO(data)
    if stream.read(8) != BTSNOOP_MAGIC:
        raise ValueError("not a btsnoop capture")
    version, datalink = struct.unpack(">II", stream.read(8))
    if version != 1:
        raise ValueError(f"unsupported btsnoop version {version}")
    if datalink not in (1001, 1002):
        raise ValueError(f"unsupported btsnoop datalink {datalink}")

    index = 0
    while True:
        header = stream.read(24)
        if not header:
            return
        if len(header) != 24:
            raise ValueError("truncated btsnoop record header")
        original, included, flags, drops, timestamp_us = struct.unpack(">IIIIQ", header)
        payload = stream.read(included)
        if len(payload) != included:
            raise ValueError("truncated btsnoop record payload")
        unix_us = timestamp_us - BTSNOOP_EPOCH_DELTA_US
        # Android/QTI's snoop clock aligns with the device-local wall time in
        # the surrounding bugreport rather than a trustworthy UTC rendering.
        # Keep it timezone-naive so correlation does not falsely claim UTC.
        timestamp = dt.datetime.fromtimestamp(
            unix_us / 1_000_000, tz=dt.timezone.utc
        ).replace(tzinfo=None)
        yield {
            "index": index,
            "timestamp": timestamp.isoformat(timespec="microseconds"),
            "unix_us": unix_us,
            "direction": "controller_to_host" if flags & 1 else "host_to_controller",
            "flags": flags,
            "drops": drops,
            "original_length": original,
            "included_length": included,
            "payload": payload,
        }
        index += 1


def _address(raw: bytes) -> str:
    return ":".join(f"{value:02X}" for value in reversed(raw))


def _acl_fields(payload: bytes) -> dict[str, object]:
    if len(payload) < 5:
        return {"kind": "acl_truncated"}
    handle_flags, declared_length = struct.unpack_from("<HH", payload, 1)
    handle = handle_flags & 0x0FFF
    packet_boundary = (handle_flags >> 12) & 0x03
    broadcast = (handle_flags >> 14) & 0x03
    result: dict[str, object] = {
        "kind": "acl",
        "handle": handle,
        "packet_boundary": packet_boundary,
        "broadcast": broadcast,
        "declared_length": declared_length,
    }
    if packet_boundary in (0, 2) and len(payload) >= 9:
        l2cap_length, cid = struct.unpack_from("<HH", payload, 5)
        result["l2cap_length"] = l2cap_length
        result["cid"] = cid
        if cid == 0x0004 and len(payload) >= 10:
            result["att_opcode"] = payload[9]
    return result


def _event_fields(payload: bytes) -> dict[str, object]:
    if len(payload) < 3:
        return {"kind": "event_truncated"}
    event_code = payload[1]
    params = payload[3 : 3 + payload[2]]
    result: dict[str, object] = {
        "kind": "event",
        "event_code": event_code,
        "event": EVENT_NAMES.get(event_code, f"Event 0x{event_code:02X}"),
    }
    if event_code == EVENT_CONNECTION_COMPLETE and len(params) >= 11:
        result.update(
            status=params[0],
            handle=struct.unpack_from("<H", params, 1)[0] & 0x0FFF,
            address=_address(params[3:9]),
            link_type=params[9],
            transport="BR/EDR" if params[9] in (0, 1) else "unknown",
            encryption=params[10],
        )
    elif event_code == EVENT_DISCONNECTION_COMPLETE and len(params) >= 4:
        result.update(
            status=params[0],
            handle=struct.unpack_from("<H", params, 1)[0] & 0x0FFF,
            reason=params[3],
        )
    elif event_code == EVENT_HARDWARE_ERROR and params:
        result["hardware_code"] = params[0]
    elif event_code == EVENT_LE_META and params:
        subevent = params[0]
        result["subevent"] = subevent
        if subevent in (LE_CONNECTION_COMPLETE, LE_ENHANCED_CONNECTION_COMPLETE):
            # Enhanced Connection Complete inserts local and peer RPAs after
            # the peer address; fields through the address are common.
            if len(params) >= 12:
                result.update(
                    status=params[1],
                    handle=struct.unpack_from("<H", params, 2)[0] & 0x0FFF,
                    role=params[4],
                    peer_address_type=params[5],
                    address=_address(params[6:12]),
                    transport="LE",
                )
                tail = 12 if subevent == LE_CONNECTION_COMPLETE else 24
                if len(params) >= tail + 8:
                    result.update(
                        interval_units=struct.unpack_from("<H", params, tail)[0],
                        interval_ms=struct.unpack_from("<H", params, tail)[0] * 1.25,
                        latency=struct.unpack_from("<H", params, tail + 2)[0],
                        supervision_timeout_units=struct.unpack_from("<H", params, tail + 4)[0],
                        supervision_timeout_ms=struct.unpack_from("<H", params, tail + 4)[0] * 10,
                    )
        elif subevent == LE_CONNECTION_UPDATE_COMPLETE and len(params) >= 10:
            result.update(
                status=params[1],
                handle=struct.unpack_from("<H", params, 2)[0] & 0x0FFF,
                interval_units=struct.unpack_from("<H", params, 4)[0],
                interval_ms=struct.unpack_from("<H", params, 4)[0] * 1.25,
                latency=struct.unpack_from("<H", params, 6)[0],
                supervision_timeout_units=struct.unpack_from("<H", params, 8)[0],
                supervision_timeout_ms=struct.unpack_from("<H", params, 8)[0] * 10,
            )
    elif event_code == EVENT_COMMAND_COMPLETE and len(params) >= 3:
        opcode = struct.unpack_from("<H", params, 1)[0]
        result.update(opcode=opcode, command=OPCODE_NAMES.get(opcode, f"Opcode 0x{opcode:04X}"))
        returned = params[3:]
        if returned:
            result["status"] = returned[0]
            result["returned_hex"] = returned.hex().upper()
        if opcode == 0xFD5E and len(returned) >= 5:
            result["bqr_current_event_mask"] = struct.unpack_from("<I", returned, 1)[0]
        if opcode in (0x1401, 0x1403, 0x1405, 0x1406) and len(returned) >= 3:
            result["handle"] = struct.unpack_from("<H", returned, 1)[0] & 0x0FFF
            if opcode == 0x1401 and len(returned) >= 5:
                result["failed_contact_counter"] = struct.unpack_from("<H", returned, 3)[0]
            elif opcode == 0x1403 and len(returned) >= 4:
                result["link_quality"] = returned[3]
            elif opcode == 0x1405 and len(returned) >= 4:
                result["rssi_dbm"] = struct.unpack_from("b", returned, 3)[0]
    elif event_code == EVENT_COMMAND_STATUS and len(params) >= 4:
        opcode = struct.unpack_from("<H", params, 2)[0]
        result.update(
            status=params[0],
            opcode=opcode,
            command=OPCODE_NAMES.get(opcode, f"Opcode 0x{opcode:04X}"),
        )
    elif event_code == EVENT_VENDOR:
        result["vendor_hex"] = params.hex().upper()
        if len(params) >= 2 and params[0] == 0x58:
            report = params[1:]
            result.update(
                vendor_subevent="Bluetooth Quality Report",
                bqr_report_id=report[0],
            )
            if len(report) >= 48 and report[0] in (0x01, 0x02, 0x03, 0x04):
                result.update(
                    bqr_packet_type=report[1],
                    handle=struct.unpack_from("<H", report, 2)[0] & 0x0FFF,
                    bqr_role=report[4],
                    tx_power_dbm=struct.unpack_from("b", report, 5)[0],
                    rssi_dbm=struct.unpack_from("b", report, 6)[0],
                    snr_db=report[7],
                    unused_afh_channels=report[8],
                    unideal_afh_channels=report[9],
                    lsto_clock_units=struct.unpack_from("<H", report, 10)[0],
                    lsto_ms=struct.unpack_from("<H", report, 10)[0] * 0.3125,
                    piconet_clock=struct.unpack_from("<I", report, 12)[0],
                    retransmission_count=struct.unpack_from("<I", report, 16)[0],
                    no_rx_count=struct.unpack_from("<I", report, 20)[0],
                    nak_count=struct.unpack_from("<I", report, 24)[0],
                    last_tx_ack_timestamp=struct.unpack_from("<I", report, 28)[0],
                    flow_off_count=struct.unpack_from("<I", report, 32)[0],
                    last_flow_on_timestamp=struct.unpack_from("<I", report, 36)[0],
                    overflow_count=struct.unpack_from("<I", report, 40)[0],
                    underflow_count=struct.unpack_from("<I", report, 44)[0],
                )
    return result


def describe_record(record: dict[str, object]) -> dict[str, object]:
    payload = record.pop("payload")
    assert isinstance(payload, bytes)
    if not payload:
        fields: dict[str, object] = {"kind": "empty"}
    elif payload[0] == H4_ACL:
        fields = _acl_fields(payload)
    elif payload[0] == H4_EVENT:
        fields = _event_fields(payload)
    elif payload[0] == H4_COMMAND and len(payload) >= 4:
        opcode = struct.unpack_from("<H", payload, 1)[0]
        fields = {
            "kind": "command",
            "opcode": opcode,
            "command": OPCODE_NAMES.get(opcode, f"Opcode 0x{opcode:04X}"),
        }
        params = payload[4 : 4 + payload[3]]
        if opcode == 0x0406 and len(params) >= 3:
            fields.update(handle=struct.unpack_from("<H", params)[0] & 0x0FFF, reason=params[2])
        elif opcode in (0x1401, 0x1403, 0x1405, 0x1406) and len(params) >= 2:
            fields["handle"] = struct.unpack_from("<H", params)[0] & 0x0FFF
        elif opcode == 0xFD5E and len(params) >= 7:
            fields.update(
                bqr_action=params[0],
                bqr_requested_event_mask=struct.unpack_from("<I", params, 1)[0],
                bqr_minimum_interval_ms=struct.unpack_from("<H", params, 5)[0],
            )
    else:
        fields = {"kind": {H4_SCO: "sco", H4_ISO: "iso"}.get(payload[0], "unknown")}
    return {**record, **fields}


def write_jsonl(path: Path, records: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        for record in records:
            output.write(json.dumps(record, sort_keys=True) + "\n")


def print_summary(records: list[dict[str, object]]) -> None:
    if not records:
        print("No HCI records")
        return
    print(f"HCI boundary (device wall clock): {records[0]['timestamp']} through {records[-1]['timestamp']}")
    print(f"Records: {len(records)}")

    acl: dict[int, dict[str, object]] = defaultdict(
        lambda: {"host_to_controller": 0, "controller_to_host": 0, "first": None, "last": None}
    )
    for record in records:
        if record.get("kind") == "acl" and "handle" in record:
            handle = int(record["handle"])
            stats = acl[handle]
            direction = str(record["direction"])
            stats[direction] = int(stats[direction]) + 1
            stats["first"] = stats["first"] or record["timestamp"]
            stats["last"] = record["timestamp"]

    print("ACL activity:")
    for handle in sorted(acl):
        stats = acl[handle]
        print(
            f"  handle {handle} (0x{handle:04X}): "
            f"TX={stats['host_to_controller']} RX={stats['controller_to_host']} "
            f"first={stats['first']} last={stats['last']}"
        )

    print("Lifecycle/quality events:")
    interesting_events = {
        EVENT_CONNECTION_COMPLETE,
        EVENT_DISCONNECTION_COMPLETE,
        EVENT_HARDWARE_ERROR,
        EVENT_VENDOR,
    }
    for record in records:
        interesting = record.get("kind") == "command" and record.get("opcode") in OPCODE_NAMES
        interesting |= record.get("kind") == "event" and (
            record.get("event_code") in interesting_events
            or record.get("subevent")
            in (LE_CONNECTION_COMPLETE, LE_CONNECTION_UPDATE_COMPLETE, LE_ENHANCED_CONNECTION_COMPLETE)
            or record.get("opcode") in (0x1401, 0x1403, 0x1405, 0x1406)
        )
        if interesting:
            display = {key: value for key, value in record.items() if key not in ("unix_us", "flags", "drops")}
            print("  " + json.dumps(display, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="btsnoop file or Android bugreport")
    parser.add_argument("--write-btsnoop", type=Path, help="write decoded btsnoop data")
    parser.add_argument("--jsonl", type=Path, help="write every decoded HCI record as JSONL")
    args = parser.parse_args()

    data = load_capture(args.capture)
    if args.write_btsnoop:
        args.write_btsnoop.write_bytes(data)
    records = [describe_record(record) for record in iter_records(data)]
    if args.jsonl:
        write_jsonl(args.jsonl, records)
    print_summary(records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
