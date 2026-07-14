#!/usr/bin/env python3
"""Build a real report-0x09 time series from a genuine-controller USBPcap capture, filtered by
USBPcap header fields (endpoint address + transfer type), not by first-payload-byte.

`extract_nfc_traffic.py`'s first-payload-byte approach (`payload[0] == 0x09`) produced a false
lead: 17 matches, all actually USB Configuration Descriptors (`bLength=0x09, bDescriptorType=0x02`)
-- 0x09 is simultaneously a valid report ID and a valid descriptor bLength, and that filter can't
tell them apart. This tool instead parses the actual USBPcap per-packet header (27 bytes, matches
USBPcap's USBPCAP_BUFFER_PACKET_HEADER struct) and filters on:
  - endpoint byte (offset 21) == 0x81  (Interrupt IN, endpoint 1 -- the confirmed report-0x09/0x05
    HID endpoint, docs/switch2/usb-spec.md SS2/4)
  - transfer byte (offset 22) == 1     (Interrupt transfer type; USBPcap: 0=Isoch,1=Interrupt,
    2=Control,3=Bulk)
  - dataLength (offset 23, u32 LE) > 0 (skip the zero-length "submit" IRP record USBPcap logs
    alongside the "complete" record that actually carries data)
This is real per-transfer metadata from the capturing driver, not inferred from packet content, so
it can't collide with an unrelated 0x09 byte appearing elsewhere in a descriptor or payload.

Once isolated, each report is checked for report ID 0x09 (should be ~all of them, since Report ID
is the very first payload byte and this endpoint should only ever carry HID input reports) and the
NFC-state byte at offset 0x0C (per docs/switch2/nfc-protocol-inventory.md SS3) is tracked across the
whole session to see whether it ever leaves 0x00 (idle).

Usage:
    python tools/extract_report09_timeseries.py [path-to-pcapng]
"""
import struct
import sys
from collections import Counter

from scapy.all import PcapNgReader

PATH = sys.argv[1] if len(sys.argv) > 1 else "usbpcaptures/genuine_procon_2.pcapng"

USBPCAP_HEADER_LEN = 27
EP_HID_IN = 0x81
TRANSFER_INTERRUPT = 1
NFC_STATE_OFFSET = 0x0C  # within the report-0x09 payload (report ID is payload[0])


def parse_header(raw):
    """Parse USBPCAP_BUFFER_PACKET_HEADER. Returns None if too short to be one."""
    if len(raw) < USBPCAP_HEADER_LEN:
        return None
    header_len, irp_id, status, function, info, bus, device, endpoint, transfer, data_length = \
        struct.unpack_from("<H Q i H B H H B B I", raw, 0)
    return {
        "header_len": header_len,
        "irp_id": irp_id,
        "status": status,
        "function": function,
        "info": info,
        "bus": bus,
        "device": device,
        "endpoint": endpoint,
        "transfer": transfer,
        "data_length": data_length,
    }


def main():
    total_packets = 0
    total_ep81_interrupt = 0
    total_with_data = 0
    total_report09 = 0
    devices_seen = Counter()
    report_ids_seen = Counter()
    nfc_state_values = Counter()
    nfc_state_series = []  # (packet_index, nfc_state_byte)
    transitions = []

    for i, pkt in enumerate(PcapNgReader(PATH)):
        total_packets += 1
        raw = bytes(pkt)
        hdr = parse_header(raw)
        if hdr is None:
            continue
        if hdr["endpoint"] != EP_HID_IN or hdr["transfer"] != TRANSFER_INTERRUPT:
            continue
        total_ep81_interrupt += 1
        if hdr["data_length"] == 0:
            continue
        total_with_data += 1
        devices_seen[hdr["device"]] += 1

        # Payload starts right after the header, per USBPcap's own headerLen field (should equal
        # USBPCAP_HEADER_LEN for interrupt transfers with no extra control-stage fields).
        payload = raw[hdr["header_len"]:hdr["header_len"] + hdr["data_length"]]
        if not payload:
            continue
        report_ids_seen[payload[0]] += 1
        if payload[0] != 0x09:
            continue
        total_report09 += 1

        if len(payload) > NFC_STATE_OFFSET:
            state = payload[NFC_STATE_OFFSET]
            nfc_state_values[state] += 1
            nfc_state_series.append((i, state))
            if nfc_state_series and len(nfc_state_series) > 1 and \
                    nfc_state_series[-2][1] != state:
                transitions.append((i, nfc_state_series[-2][1], state))

    print(f"Scanned {total_packets} packets in {PATH}")
    print(f"EP 0x81 Interrupt-transfer records (incl. zero-length submits): {total_ep81_interrupt}")
    print(f"  ...with actual data (dataLength > 0): {total_with_data}")
    print(f"  ...report ID 0x09 (report-0x09 input reports): {total_report09}")
    print()
    print(f"Devices seen on this endpoint: {dict(devices_seen)}")
    print(f"Report IDs seen on this endpoint: "
          f"{ {hex(k): v for k, v in report_ids_seen.most_common()} }")
    print()
    print(f"NFC-state byte (report-0x09 offset 0x{NFC_STATE_OFFSET:02X}) value distribution: "
          f"{ {hex(k): v for k, v in nfc_state_values.most_common()} }")
    print(f"NFC-state transitions observed: {len(transitions)}")
    for pkt_idx, before, after in transitions[:50]:
        print(f"  packet #{pkt_idx:6d}: 0x{before:02X} -> 0x{after:02X}")
    if not transitions:
        print("  (none -- NFC-state byte never changed value across the whole report-0x09 series)")


if __name__ == "__main__":
    main()
