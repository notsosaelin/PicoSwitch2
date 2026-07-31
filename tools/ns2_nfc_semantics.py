#!/usr/bin/env python3
"""Decode Switch 2 NFC (command 0x01) traffic into transaction-level semantics.

Why this exists
---------------
A raw hex diff of two NFC captures is nearly useless: offsets, chunk
boundaries, timeouts, and counters legitimately vary between an otherwise
identical genuine and virtual run. Meanwhile the differences that matter --
a record targeting a different page, a status the console never reached, a
device result the controller never published -- are buried inside multi-chunk
transfers that no per-record view reassembles.

During the v3 investigation this cost real hardware iterations. The correct
0x21 device result was misread as "32 meaningful bytes then zeros"; it is a
19-byte header plus the tag's complete 64-byte SRAM response. A fixed Kirby
record table looked correct until King Dedede used different pages. Both were
visible in captures already on disk.

This module is the single place where NFC wire layout is written down. It is
imported by ns2_trace.py so the decoder, the differ, and any future dissector
report identical field names.

Layout authority (keep in sync, do not re-derive):
  include/ns2_virtual_nfc.h        status payload, read-chunk framing
  include/ns2_amiibo_v3.h          sector-read descriptor and result prefix
  include/ns2_amiibo_v3_write.h    staged write envelopes
  src/nfc/ns2_virtual_nfc.c        operation-buffer prefix
  src/switch_pro2/switch_pro2.c    ns2_v3_serve() dispatch and status edges

Not covered: the controller's report NFC-state field. It travels in device
input reports, which the protocol tracer does not retain, so a state edge is
inferred here only from an observed 0x05 status reply.
"""

from __future__ import annotations

import difflib
from dataclasses import dataclass, field
from typing import Any, Iterable

NFC_COMMAND = 0x01

# Controller-facing NFC subcommands. Names follow the firmware's own dispatch
# labels so a grep for a name lands in the code that implements it.
NFC_SUBCOMMANDS = {
    0x01: "nfc_start",
    0x03: "poll",
    0x04: "stop",
    0x05: "status",
    0x06: "begin_read",
    0x08: "commit_write",
    0x0C: "nfc_identity",
    0x14: "stage_buffer",
    0x15: "read_buffer",
    0x1E: "sector_read",
    0x20: "commit_extended",
    0x21: "execute_device_command",
}

# Console-facing NFC states reported by 0x05. 0x07 is the error state; its
# companion detail byte 0x41 is what surfaces as console error 2115-0096.
# Two different root causes have produced the identical pair, so never treat
# this value alone as a diagnosis.
NFC_STATES = {
    0x00: "idle",
    0x04: "operation_active",
    0x05: "write_committed",
    0x07: "error",
    0x09: "tag_present",
    0x15: "sector_result_ready",
    0x16: "extended_complete",
    0x18: "device_result_ready",
}

# Operation-buffer prefix, from build_operation_prefix() and
# ns2_amiibo_v3_build_sector_read_result().
PREFIX_SIZE = 60
SECTOR_RESULT_PREFIX_SIZE = 64
SIGNATURE_OFFSET = 19
SIGNATURE_SIZE = 32
CHIP_IDENTITY_OFFSET = 18
CHIP_IDENTITIES = {0x00: "ntag215", 0x06: "ntag_i2c_2k_v3"}

DEVICE_COMMAND_SIZE = 74
EXTENDED_CLEAR_SIZE = 355
EXTENDED_UPDATE_SIZE = 167
WRITE_STAGING_SIZE = 454


def _hex(data: bytes) -> str:
    return data.hex().upper()


def _u16le(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 2], "little")


def state_name(value: int) -> str:
    return NFC_STATES.get(value, f"unknown_{value:02x}")


def subcommand_name(value: int) -> str:
    return NFC_SUBCOMMANDS.get(value, f"sub_{value:02x}")


def decode_status(data: bytes) -> dict[str, Any]:
    """0x05 reply. A genuine controller answers states 0x15/0x16/0x18 with an
    otherwise empty body -- no UID, no tag identity. That emptiness is
    load-bearing, so it is reported rather than normalized away."""
    if not data:
        return {"malformed": "empty status payload"}
    fields: dict[str, Any] = {
        "state": f"0x{data[0]:02X}",
        "state_name": state_name(data[0]),
        "detail": f"0x{data[1]:02X}" if len(data) > 1 else None,
    }
    body_present = len(data) >= 16 and any(data[2:])
    fields["identity_present"] = body_present
    if body_present and len(data) >= 16 and data[8] == 0x07:
        fields["uid"] = _hex(data[9:16])
    return fields


def decode_read_descriptor(data: bytes) -> dict[str, Any]:
    """0x06 request: timeout:u16le | uid[7] | tag_type | count | (start,end)*N.

    Bytes 0..1 are a timeout, not a marker. Gating on the literal D0 07 seen in
    one capture silently rejected the 3000 ms (B8 0B) extended descriptor and
    stalled every escalated read.
    """
    if len(data) < 11:
        return {"malformed": f"descriptor is {len(data)} bytes, need >= 11"}
    count = data[10]
    fields: dict[str, Any] = {
        "timeout_ms": _u16le(data, 0),
        "uid": _hex(data[2:9]),
        "uid_selector": "any" if not any(data[2:9]) else "selected",
        "tag_type": f"0x{data[9]:02X}",
        "block_count": count,
    }
    blocks: list[str] = []
    highest = 0
    if count and 11 + count * 2 <= len(data):
        for index in range(count):
            start, end = data[11 + index * 2], data[12 + index * 2]
            blocks.append(f"0x{start:02X}-0x{end:02X}")
            highest = max(highest, (end + 1) * 4)
    else:
        fields["malformed"] = "block count overruns the request"
    fields["blocks"] = blocks
    fields["highest_byte"] = highest
    # A request reaching past 540 bytes is the console asking for the v3 page
    # set. Serving it from the NTAG215 compatibility view drops every extended
    # block and produces a silent retry loop with no console error.
    fields["needs_v3_source"] = highest > 540
    return fields


def decode_sector_descriptor(data: bytes) -> dict[str, Any]:
    """0x1E request: timeout:u16le | uid[7] | tag_type | count |
    (sector,start,end)*N | reserved[6]."""
    if len(data) < 11:
        return {"malformed": f"descriptor is {len(data)} bytes, need >= 11"}
    count = data[10]
    fields: dict[str, Any] = {
        "timeout_ms": _u16le(data, 0),
        "uid": _hex(data[2:9]),
        "tag_type": f"0x{data[9]:02X}",
        "range_count": count,
    }
    ranges: list[str] = []
    end_of_ranges = 11 + count * 3
    if count and end_of_ranges <= len(data):
        for index in range(count):
            sector, first, last = data[11 + index * 3:14 + index * 3]
            ranges.append(
                f"s{sector}:0x{first:02X}-0x{last:02X}({(last - first + 1) * 4}B)")
    else:
        fields["malformed"] = "range count overruns the request"
    fields["ranges"] = ranges
    fields["reserved_zero"] = not any(data[end_of_ranges:])
    return fields


def decode_operation_prefix(buffer: bytes) -> dict[str, Any]:
    """The 60-byte identity/signature/operation prefix that precedes tag bytes.

    Byte 18 is the chip identity. It alone drives the console's escalation from
    the NTAG215 page set to the 4-block v3 descriptor; bytes 19..50 are the
    tag's originality signature, which is a separate field that was once
    conflated with the SRAM window.
    """
    if len(buffer) < PREFIX_SIZE:
        return {"malformed": f"buffer is {len(buffer)} bytes, need >= {PREFIX_SIZE}"}
    chip = buffer[CHIP_IDENTITY_OFFSET]
    signature = buffer[SIGNATURE_OFFSET:SIGNATURE_OFFSET + SIGNATURE_SIZE]
    fields = {
        "result_type": f"0x{buffer[0]:02X}",
        "uid": _hex(buffer[8:15]) if buffer[7] == 0x07 else None,
        "chip_identity": f"0x{chip:02X}",
        "chip_identity_name": CHIP_IDENTITIES.get(chip, f"unknown_{chip:02x}"),
        "signature": _hex(signature),
        "signature_present": any(signature),
        "operation_metadata": _hex(buffer[51:60]),
        "tag_bytes": len(buffer) - PREFIX_SIZE,
    }
    return fields


def _parse_write_records(data: bytes, cursor: int, count: int) -> tuple[list[str], int, str | None]:
    records: list[str] = []
    total = 0
    for _ in range(count):
        if cursor + 2 > len(data):
            return records, total, "record list overruns the envelope"
        page, length = data[cursor], data[cursor + 1]
        cursor += 2
        if cursor + length > len(data):
            return records, total, f"record at page 0x{page:02X} overruns the envelope"
        records.append(f"s0:0x{page:02X}+{length}")
        total += length
        cursor += length
    return records, total, None


def classify_stage(data: bytes) -> dict[str, Any]:
    """Classify one reassembled 0x14 envelope into its capture-derived family.

    Three families share the 0x14 transport and are completed by three
    different subcommands. Treating the sector-aware operations as harmless
    no-ops let plain System Settings writes pass while every in-game write
    failed, so the distinction is made explicit here.
    """
    fields: dict[str, Any] = {"staged_bytes": len(data)}
    if len(data) < 22:
        fields["envelope"] = "truncated"
        return fields
    fields["timeout_ms"] = _u16le(data, 0)
    fields["uid"] = _hex(data[2:9])

    if data[9] != 0x01:
        fields["envelope"] = "unknown"
        return fields

    if data[10] == 0x01 and len(data) == DEVICE_COMMAND_SIZE:
        # Executed by 0x21; the controller answers with the tag's SRAM window.
        fields["envelope"] = "device_command"
        fields["completed_by"] = "0x21"
        return fields

    if data[10] != 0x06:
        fields["envelope"] = "unknown"
        return fields

    if not any(data[11:22]) and data[22] == 0x02 and \
            data[23] == 0x00 and data[24] == 0x92 and data[25] == 0xF0:
        fields.update({
            "envelope": "extended_clear",
            "completed_by": "0x20",
            "expected_bytes": EXTENDED_CLEAR_SIZE,
            "records": ["s0:0x92+240", "s0:0xCE+80"],
        })
        return fields

    if len(data) >= 68 and data[22] == 0x03 and data[23] == 0x00 and \
            data[24] == 0x04 and data[25] == 0x04:
        # Self-describing allocation. Kirby uses sector-0 page 0x92 with
        # sector-1 pages 0x00/0x01; King Dedede uses 0xB2 with 0x64/0x65.
        # Reading these from the envelope is what removed the rider table.
        sector0_page = data[31]
        capability_page = data[13]
        fields.update({
            "envelope": "extended_update",
            "completed_by": "0x20",
            "expected_bytes": EXTENDED_UPDATE_SIZE,
            "sector0_page": f"0x{sector0_page:02X}",
            "sector1_capability_page": f"0x{capability_page:02X}",
            "sector1_data_page": f"0x{data[66]:02X}",
            "next_capability": _hex(data[18:22]),
            "records": [
                "s0:0x04+4",
                f"s0:0x{sector0_page:02X}+32",
                f"s1:0x{data[66]:02X}+96",
            ],
        })
        if data[66] != (capability_page + 1) & 0xFF:
            fields["malformed"] = (
                f"sector-1 data page 0x{data[66]:02X} does not follow "
                f"capability page 0x{capability_page:02X}")
        return fields

    count = data[21]
    if 1 <= count <= 16:
        records, total, error = _parse_write_records(data, 22, count)
        fields.update({
            "envelope": "write",
            "completed_by": "0x08",
            "expected_bytes": WRITE_STAGING_SIZE,
            "record_count": count,
            "records": records,
            "data_bytes": total,
            "static_lock": _hex(data[17:21]),
        })
        if error:
            fields["malformed"] = error
        return fields

    fields["envelope"] = "unknown"
    return fields


@dataclass
class Step:
    """One semantic event. ``key`` is what the differ aligns on; ``fields`` is
    what it reports when two aligned steps disagree."""

    seq: int
    t_us: int
    kind: str
    key: tuple[Any, ...]
    fields: dict[str, Any] = field(default_factory=dict)

    def label(self) -> str:
        detail = " ".join(
            f"{name}={value}" for name, value in self.fields.items()
            if name in ("state_name", "envelope", "blocks", "ranges", "records",
                        "chip_identity_name", "total_bytes", "response"))
        return f"{self.kind}{(' ' + detail) if detail else ''}"


class Reassembler:
    """Rebuild transaction-level steps from a flat NFC record stream.

    Multi-chunk 0x14 staging and 0x15 retrieval are reassembled in full before
    being classified. Partial views are what produced the "32 meaningful bytes
    then zeros" misreading of the device result.
    """

    def __init__(self) -> None:
        self.steps: list[Step] = []
        self._pending_read_offset: int | None = None
        self._read_buffer = bytearray()
        self._read_declared = 0
        self._read_started: dict[str, Any] | None = None
        self._stage = bytearray()
        self._stage_seq = 0
        self._stage_t = 0
        self._stage_truncated = False
        self._last_command: dict[str, Any] | None = None
        self.warnings: list[str] = []

    # -- helpers -----------------------------------------------------------
    def _emit(self, record: dict[str, Any], kind: str, key: tuple[Any, ...],
              fields: dict[str, Any] | None = None) -> None:
        self.steps.append(Step(record["seq"], record["t_us"], kind, key, fields or {}))

    def _flush_stage(self, record: dict[str, Any], completed_by: str) -> None:
        if not self._stage:
            return
        fields = classify_stage(bytes(self._stage))
        fields["completed_by_observed"] = completed_by
        if self._stage_truncated:
            # The bytes are a tracer-bounded prefix, so length-based claims
            # about this envelope are not evidence of anything.
            fields["capture_truncated"] = True
        if fields.get("completed_by") and fields["completed_by"] != completed_by:
            fields["malformed"] = (
                f"envelope {fields['envelope']} expects {fields['completed_by']} "
                f"but was completed by {completed_by}")
        expected = fields.get("expected_bytes")
        if expected is not None and expected != len(self._stage) \
                and not self._stage_truncated:
            # An envelope completed at the wrong length is the signature of a
            # split USB read: a 88-byte 0x14 delivered as 64 + 24 turned the
            # tail into a second bogus command and crashed the console.
            fields["malformed"] = (
                f"{fields['envelope']} staged {len(self._stage)} bytes, "
                f"expected {expected}")
        key = (fields.get("envelope"), completed_by, tuple(fields.get("records", ())))
        self.steps.append(Step(self._stage_seq, self._stage_t, "stage", key, fields))
        self._stage = bytearray()
        self._stage_truncated = False

    def _flush_read(self, record: dict[str, Any]) -> None:
        if not self._read_buffer:
            return
        buffer = bytes(self._read_buffer)
        fields = decode_operation_prefix(buffer)
        # Each chunk declares its own length, so the buffer's true size is
        # known even when an older tracer retained only a prefix of the bytes.
        # Reporting the declared size keeps a 664-byte escalated read
        # recognizable in captures that predate full NFC payload retention.
        fields["total_bytes"] = max(self._read_declared, len(buffer))
        if len(buffer) < self._read_declared:
            fields["captured_bytes"] = len(buffer)
            fields["capture_truncated"] = True
        if self._read_started:
            fields["for_descriptor"] = self._read_started.get("blocks")
        self._emit(record, "read_buffer",
                   ("read_buffer", fields["total_bytes"], fields.get("chip_identity"),
                    fields.get("result_type")), fields)
        self._read_buffer = bytearray()
        self._read_declared = 0

    # -- main --------------------------------------------------------------
    def feed(self, record: dict[str, Any]) -> None:
        if record["id"] != NFC_COMMAND:
            return
        if record["kind"] not in ("bulk_command", "bulk_response"):
            return
        raw = bytes.fromhex(record["payload"])
        if len(raw) < 8:
            self.warnings.append(f"seq {record['seq']}: truncated bulk header")
            return
        sub = raw[3]
        data = raw[8:]

        if record["kind"] == "bulk_command":
            self._on_command(record, sub, data)
        else:
            self._on_response(record, sub, raw, data)

    def _on_command(self, record: dict[str, Any], sub: int, data: bytes) -> None:
        self._last_command = {"sub": sub, "seq": record["seq"]}
        name = subcommand_name(sub)

        if sub == 0x03:
            self._emit(record, "poll", ("poll",), {"data": _hex(data)})
        elif sub == 0x04:
            self._flush_stage(record, "0x04(abandoned)")
            self._emit(record, "stop", ("stop",))
        elif sub == 0x05:
            pass  # the reply carries the meaning
        elif sub == 0x06:
            self._flush_read(record)
            descriptor = decode_read_descriptor(data)
            self._read_started = descriptor
            self._emit(record, "begin_read",
                       ("begin_read", tuple(descriptor.get("blocks", ())),
                        descriptor.get("uid_selector")), descriptor)
        elif sub == 0x1E:
            self._flush_read(record)
            descriptor = decode_sector_descriptor(data)
            self._read_started = descriptor
            self._emit(record, "sector_read",
                       ("sector_read", tuple(descriptor.get("ranges", ()))), descriptor)
        elif sub == 0x15:
            if len(data) >= 2:
                self._pending_read_offset = _u16le(data, 0)
                if self._pending_read_offset == 0:
                    self._read_buffer = bytearray()
        elif sub == 0x14:
            if len(data) < 4:
                self.warnings.append(f"seq {record['seq']}: short 0x14 request")
                return
            offset, declared = _u16le(data, 0), _u16le(data, 2)
            body = data[4:4 + declared]
            if len(body) != declared:
                # Two very different causes. The tracer keeps a bounded prefix
                # of each event, so an older capture legitimately holds fewer
                # bytes than the wire carried; `captured < length` says so. A
                # record that was captured in full and still falls short is a
                # transport fault -- that is how an 88-byte 0x14 delivered as
                # 64 + 24 crashed the console with 2168-0002.
                wire_body = max(record["length"] - 8, 0)
                if record["captured"] < record["length"] and declared + 4 <= wire_body:
                    self._stage_truncated = True
                else:
                    self.warnings.append(
                        f"seq {record['seq']}: 0x14 declared {declared} bytes but "
                        f"carried {len(body)}")
            if offset == 0:
                self._flush_stage(record, "0x14(restarted)")
                self._stage_seq, self._stage_t = record["seq"], record["t_us"]
            if len(self._stage) < offset + len(body):
                self._stage.extend(bytes(offset + len(body) - len(self._stage)))
            self._stage[offset:offset + len(body)] = body
        elif sub == 0x08:
            self._flush_stage(record, "0x08")
            self._emit(record, "commit_write", ("commit_write",))
        elif sub == 0x20:
            self._flush_stage(record, "0x20")
            self._emit(record, "commit_extended", ("commit_extended",))
        elif sub == 0x21:
            self._flush_stage(record, "0x21")
            self._emit(record, "execute_device_command", ("execute_device_command",))
        else:
            self._emit(record, name, (name,), {"data": _hex(data)})

    def _on_response(self, record: dict[str, Any], sub: int, raw: bytes,
                     data: bytes) -> None:
        # Direction byte 0x04 is a bare ACK, 0x01 carries a data reply.
        acknowledgement = raw[1] == 0x04
        if sub == 0x05:
            fields = decode_status(data)
            fields["payload_bytes"] = len(data)
            self._emit(record, "status",
                       ("status", fields.get("state"), fields.get("identity_present")),
                       fields)
        elif sub == 0x15 and not acknowledgement:
            if len(data) < 3:
                self.warnings.append(f"seq {record['seq']}: short read chunk")
                return
            last, length = data[0], _u16le(data, 1)
            chunk = data[3:3 + length]
            offset = self._pending_read_offset
            if offset is None:
                self.warnings.append(
                    f"seq {record['seq']}: read chunk with no matching request")
                offset = len(self._read_buffer)
            self._read_declared = max(self._read_declared, offset + length)
            if len(self._read_buffer) < offset + len(chunk):
                self._read_buffer.extend(
                    bytes(offset + len(chunk) - len(self._read_buffer)))
            self._read_buffer[offset:offset + len(chunk)] = chunk
            self._pending_read_offset = None
            if last:
                self._flush_read(record)
        elif sub in (0x08, 0x20, 0x21, 0x06, 0x1E, 0x03, 0x04):
            # Bare ACKs carry no payload; their meaning arrives in the next
            # 0x05 status, which is why a missing status edge stalls the
            # console rather than producing an error.
            if not acknowledgement and data:
                self._emit(record, f"{subcommand_name(sub)}_reply",
                           (f"{subcommand_name(sub)}_reply", len(data)),
                           {"data": _hex(data)})

    def finish(self) -> list[Step]:
        terminator = {"seq": self.steps[-1].seq if self.steps else 0,
                      "t_us": self.steps[-1].t_us if self.steps else 0}
        self._flush_stage(terminator, "end-of-capture")
        self._flush_read(terminator)
        return self.steps


def build_timeline(records: Iterable[dict[str, Any]]) -> tuple[list[Step], list[str]]:
    reassembler = Reassembler()
    for record in records:
        reassembler.feed(record)
    return reassembler.finish(), reassembler.warnings


def error_context(steps: list[Step]) -> tuple[list[str], list[str]]:
    """Split 07/41 states into genuine failures and expected removal edges.

    The console-facing 07/41 pair is ambiguous by itself. It is what a
    fail-closed record rejection produces -- and it is also, deliberately, how
    the controller signals logical removal after a committed write, because the
    console needs that edge to leave its amiibo UI. Reporting every occurrence
    as a failure makes a completely healthy write/remove/rescan cycle look
    broken, which is the same "treat a wire value as a diagnosis" error that
    cost the v3 investigation real hardware runs.

    What disambiguates them is the operation in flight: a Stop that follows a
    committed write is a removal, anything else is a failure. Confirmed against
    dumps/experiments/20260729-101834-v3-post-extraction, where the firmware
    independently reported errors=0 for a trace containing one 07/41.

    Returns (failures, removals).
    """
    failures: list[str] = []
    removals: list[str] = []
    committed_since_poll = False
    for index, step in enumerate(steps):
        if step.kind in ("poll", "begin_read", "sector_read"):
            committed_since_poll = False
        if step.kind in ("commit_write", "commit_extended"):
            committed_since_poll = True
        if step.kind == "status" and step.fields.get("state") == "0x05":
            committed_since_poll = True   # write_committed
        if step.kind != "status" or step.fields.get("state") != "0x07":
            continue
        cause = next(
            (steps[back] for back in range(index - 1, -1, -1)
             if steps[back].kind != "status"), None)
        detail = step.fields.get("detail")
        where = f"seq {cause.seq} {cause.label()}" if cause else "no prior operation"
        if committed_since_poll and cause is not None and cause.kind == "stop":
            removals.append(
                f"seq {step.seq}: tag removed after a committed write ({where})")
            committed_since_poll = False
        else:
            failures.append(
                f"seq {step.seq}: error state (detail {detail}) after {where}")
    return failures, removals


def render_timeline(steps: list[Step], warnings: list[str]) -> str:
    lines = [f"{len(steps)} NFC transaction steps"]
    first = steps[0].t_us if steps else 0
    for step in steps:
        elapsed = (step.t_us - first) & 0xFFFFFFFF
        lines.append(f"{step.seq:5d} {elapsed / 1000:9.3f}ms  {step.label()}")
        for name, value in step.fields.items():
            if name in ("signature", "data") or value in (None, "", [], False):
                continue
            lines.append(f"          {name}: {value}")
    failures, removals = error_context(steps)
    if removals:
        lines.append("")
        lines.append("Expected removal edges (07/41 is also the TagRemoved signal):")
        lines.extend(f"  {finding}" for finding in removals)
    if failures:
        lines.append("")
        lines.append("Error states and the operation in flight:")
        lines.extend(f"  {finding}" for finding in failures)
        lines.append("  Read `amiibo v3diag` for the internal cause; the wire "
                     "cannot distinguish them.")
    for warning in warnings:
        lines.append(f"WARNING {warning}")
    return "\n".join(lines)


# Fields that legitimately differ between two runs of the same operation.
# ``uid`` and ``signature`` differ whenever two different figures are compared,
# which is a deliberate workflow: the King Dedede allocation was only proved to
# be allocation-relative by comparing it against a Kirby capture.
IDENTITY_FIELDS = ("uid", "signature", "timeout_ms", "for_descriptor")
NEVER_COMPARED = ("data", "signature", "payload_bytes")


def compare_timelines(left: list[Step], right: list[Step],
                      left_name: str, right_name: str,
                      ignore_identity: bool = False) -> tuple[str, bool]:
    """Report the first semantic divergence, not every byte that moved.

    Alignment uses each step's key, so an extra status poll or a different
    chunk split does not cascade into hundreds of false differences. A bounded
    ring buffer also means two captures rarely start at the same point in the
    lifecycle; a leading-only offset is reported as alignment, not divergence.
    """
    skipped = set(NEVER_COMPARED) | (set(IDENTITY_FIELDS) if ignore_identity else set())
    matcher = difflib.SequenceMatcher(
        None, [step.key for step in left], [step.key for step in right],
        autojunk=False)
    lines = [f"genuine={left_name} ({len(left)} steps)",
             f"virtual={right_name} ({len(right)} steps)"]
    matched = 0
    opcodes = matcher.get_opcodes()
    for index, (tag, i1, i2, j1, j2) in enumerate(opcodes):
        if tag == "equal":
            for offset in range(i2 - i1):
                left_step, right_step = left[i1 + offset], right[j1 + offset]
                differences = [
                    f"{name}: {left_step.fields.get(name)!r} -> {right_step.fields.get(name)!r}"
                    for name in sorted(set(left_step.fields) | set(right_step.fields))
                    if left_step.fields.get(name) != right_step.fields.get(name)
                    and name not in skipped
                ]
                if not differences:
                    matched += 1
                    continue
                lines.append(f"MATCH through {matched} steps")
                lines.append(f"DIVERGENCE at step {matched + 1} / {left_step.kind}")
                lines.append(f"  genuine seq {left_step.seq}: {left_step.label()}")
                lines.append(f"  virtual seq {right_step.seq}: {right_step.label()}")
                lines.extend(f"    {difference}" for difference in differences)
                return "\n".join(lines), False
            continue
        if index == 0 and matched == 0:
            # One capture simply began earlier in the lifecycle than the other.
            lines.append(
                f"ALIGNMENT: skipped {i2 - i1} leading genuine and {j2 - j1} "
                "leading virtual steps before the first common point")
            continue
        lines.append(f"MATCH through {matched} steps")
        lines.append("DIVERGENCE: the step sequences differ")
        for step in left[i1:i2]:
            lines.append(f"  genuine only  seq {step.seq}: {step.label()}")
        for step in right[j1:j2]:
            lines.append(f"  virtual only  seq {step.seq}: {step.label()}")
        return "\n".join(lines), False

    lines.append(f"MATCH: all {matched} steps agree semantically")
    return "\n".join(lines), True
