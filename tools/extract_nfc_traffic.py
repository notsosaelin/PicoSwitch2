#!/usr/bin/env python3
"""Extract all command-0x01 (NFC) traffic from a raw genuine-controller USB capture.

Read-only, offline analysis tool -- scans `usbpcaptures/genuine_procon_2.pcapng` (or any
similarly-captured USBPcap file) for the Switch 2 command envelope signature
`[cmd=0x01][req_type=0x91][transport]` (transport 0x00=USB, 0x01=BLE, per this repo's own
confirmed convention -- see src/switch_pro2/switch_pro2.c's ns2_dispatch()) and prints every match
with surrounding context, so real command-0x01 (NFC, per ndeadly/switch2_controller_research and
this repo's own switch_pro2.c dispatcher) traffic already present in this repo's own capture file
can be found and cross-checked, without capturing anything new.

This does not decode USB framing (the capture appears to be a raw USBPcap/USB-monitor dump; this
script treats each packet as an opaque byte blob and searches for the command signature directly
inside it, which is sufficient to locate candidate command bytes without needing full USB
transfer-descriptor parsing). False positives are possible if the 3-byte signature appears
coincidentally inside unrelated data (e.g. report 0x09's own high-entropy motion bytes) -- matches
are printed with enough surrounding context (subcommand byte, following bytes) to manually assess
plausibility (a real command frame is followed by a length field and has a bounded, sane length).

Usage:
    python tools/extract_nfc_traffic.py [path-to-pcapng]
"""
import sys
from scapy.all import PcapNgReader

PATH = sys.argv[1] if len(sys.argv) > 1 else "usbpcaptures/genuine_procon_2.pcapng"

# Signature: cmd=0x01 (NFC family, per ndeadly's commands.md and this repo's ns2_dispatch()),
# req_type=0x91 (this repo's own confirmed constant across every command family), transport byte
# (0x00=USB, 0x01=BLE -- both checked, this capture is USB so 0x00 is expected to dominate).
SIGNATURES = [bytes([0x01, 0x91, 0x00]), bytes([0x01, 0x91, 0x01])]


def find_all(haystack, needle):
    start = 0
    out = []
    while True:
        idx = haystack.find(needle, start)
        if idx < 0:
            break
        out.append(idx)
        start = idx + 1
    return out


def main():
    total_packets = 0
    total_matches = 0
    match_records = []
    for i, pkt in enumerate(PcapNgReader(PATH)):
        total_packets += 1
        raw = bytes(pkt)
        for sig in SIGNATURES:
            for idx in find_all(raw, sig):
                # Print a window: signature + subcommand + next ~20 bytes (length field + payload
                # start), and a few bytes of context before the signature.
                window = raw[max(0, idx - 4):idx + 28]
                match_records.append((i, idx, window))
                total_matches += 1

    print(f"Scanned {total_packets} packets in {PATH}")
    print(f"Found {total_matches} candidate command-0x01 (NFC) signature matches\n")
    for pkt_idx, byte_idx, window in match_records:
        sub = window[7] if len(window) > 7 else None
        sub_str = f"sub=0x{sub:02X}" if sub is not None else "sub=?"
        print(f"  packet #{pkt_idx:6d}  byte_offset={byte_idx:6d}  {sub_str}  "
              f"bytes={window.hex()}")

    if total_matches == 0:
        print("  (none found -- either this capture predates/postdates any NFC command exchange, "
              "or the signature doesn't appear in this particular file/session)")


if __name__ == "__main__":
    main()
