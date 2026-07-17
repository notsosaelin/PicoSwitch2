#!/usr/bin/env python3
"""Structural regression test for the Pro Controller 2 UAC1 function.

This intentionally checks both halves of the contract:
  * the full NS2_AUDIO descriptor still advertises the captured retail layout;
  * the custom driver still owns/activates both isochronous streams and handles
    every UAC1 Feature Unit request implied by that descriptor.

Usage: python tools/verify_ns2_audio.py
Exit code 0 = all checks passed.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_PATH = ROOT / "src" / "switch_pro2" / "switch_pro2.c"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"OK   {message}")


def extract_audio_config(source: str) -> bytes:
    matches = re.findall(
        r"static const uint8_t ns2_config_desc\[\]\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    )
    check(len(matches) == 2, "source contains no-audio and full-audio config arrays")
    body = re.sub(r"//.*", "", matches[1])
    values = []
    for token in (part.strip() for part in body.split(",")):
        if not token:
            continue
        values.append(
            eval(token, {"__builtins__": {}}, {"NS2_CONFIG_LEN": 268}) & 0xFF
        )
    return bytes(values)


def descriptors(config: bytes):
    result = []
    offset = 0
    while offset < len(config):
        length = config[offset]
        check(length >= 2, f"descriptor at offset {offset} has a valid length")
        end = offset + length
        check(end <= len(config), f"descriptor at offset {offset} stays in bounds")
        result.append((offset, config[offset:end]))
        offset = end
    return result


def find_descriptor(descs, predicate, label):
    found = [(offset, desc) for offset, desc in descs if predicate(desc)]
    check(len(found) == 1, f"exactly one {label} descriptor is present")
    return found[0]


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    config = extract_audio_config(source)
    check(len(config) == 268, "full configuration descriptor is exactly 268 bytes")
    check(config[2] | (config[3] << 8) == len(config),
          "wTotalLength matches the full descriptor")
    check(config[4] == 5, "configuration declares HID, vendor, and three audio interfaces")

    descs = descriptors(config)
    _, audio_iad = find_descriptor(
        descs,
        lambda d: d[1] == 0x0B and d[2:8] == bytes([2, 3, 1, 1, 0, 0]),
        "audio IAD",
    )
    check(audio_iad[2] == 2 and audio_iad[3] == 3,
          "audio IAD owns interfaces 2 through 4")

    _, ac_if = find_descriptor(
        descs,
        lambda d: d[1] == 0x04 and d[2:8] == bytes([2, 0, 0, 1, 1, 0]),
        "Audio Control interface",
    )
    check(ac_if[2] == 2, "Audio Control is interface 2")

    _, ac_header = find_descriptor(
        descs,
        lambda d: d[1:3] == bytes([0x24, 0x01]) and len(d) == 10,
        "UAC1 Audio Control header",
    )
    check(ac_header[3:5] == bytes([0x00, 0x01]), "Audio Control version is UAC1.0")
    check(ac_header[5:7] == bytes([0x47, 0x00]), "Audio Control tree length is 71 bytes")
    check(ac_header[7:] == bytes([2, 3, 4]),
          "Audio Control header links speaker and microphone streams")

    feature_units = [
        d for _, d in descs if d[1:3] == bytes([0x24, 0x06])
    ]
    check({d[3] for d in feature_units} == {0x02, 0x05},
          "speaker and microphone Feature Units are both present")
    for unit in feature_units:
        check(unit[5] == 1 and unit[6] == 0x03,
              f"Feature Unit 0x{unit[3]:02X} advertises master mute and volume")

    expected_format = bytes([0x0B, 0x24, 0x02, 0x01, 0x02, 0x02,
                             0x10, 0x01, 0x80, 0xBB, 0x00])
    check(sum(1 for _, d in descs if d == expected_format) == 2,
          "both streams are fixed at 48 kHz stereo 16-bit PCM")

    for interface, endpoint in ((3, 0x03), (4, 0x83)):
        _, alt_one = find_descriptor(
            descs,
            lambda d, interface=interface:
                d[1] == 0x04 and d[2:5] == bytes([interface, 1, 1]),
            f"interface {interface} alternate setting 1",
        )
        check(alt_one[5:8] == bytes([1, 2, 0]),
              f"interface {interface} alt 1 is an Audio Streaming interface")
        _, ep = find_descriptor(
            descs,
            lambda d, endpoint=endpoint:
                d[1] == 0x05 and d[2] == endpoint,
            f"audio endpoint 0x{endpoint:02X}",
        )
        check(ep[3] == 0x0D, f"endpoint 0x{endpoint:02X} is synchronous isochronous data")
        check((ep[4] | (ep[5] << 8)) == 192 and ep[6] == 1,
              f"endpoint 0x{endpoint:02X} carries 192 bytes every 1 ms")

    required_driver_markers = [
        "usbd_edpt_iso_alloc",
        "usbd_edpt_iso_activate",
        "UAC1_REQ_SET_CUR",
        "UAC1_REQ_GET_CUR",
        "UAC1_REQ_GET_MIN",
        "UAC1_REQ_GET_MAX",
        "UAC1_REQ_GET_RES",
        "ns2_audio_set_alt",
        "ns2_audio_xfer",
        "ns2_audio_mic_silence",
    ]
    for marker in required_driver_markers:
        check(marker in source, f"driver contains {marker}")
    check("audio_stub" not in source, "legacy descriptor-only audio stub is gone")

    print("\nAll NS2 audio checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}")
        raise SystemExit(1)
