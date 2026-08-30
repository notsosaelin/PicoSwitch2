#!/usr/bin/env python3
"""Decode a captured HID report descriptor and say what the device declares.

    python tools/analyze_hid_descriptor.py dumps/kbm-pairing-<label>-<stamp>.jsonl
    python tools/analyze_hid_descriptor.py --hex 05010906A101...     # raw bytes, no log
    python tools/analyze_hid_descriptor.py <log> --label "8bitdo" --out report.md

## The question this exists to answer

A BLE peer that declares both a keyboard collection and a pointer collection is
classified as a MOUSE by the adapter, because COMBO requires a Class-of-Device
statement and BLE has none, so capability precedence decides and pointer wins
(``ns2_kbm_primary_from_caps``). That rule was written to stop a gaming mouse
with macro keys from taking the keyboard role, so it cannot simply be inverted:
a keyboard-with-media-keys and a mouse-with-macro-keys look identical to a test
that only asks "does a keyboard collection exist?".

What might separate them is the SHAPE of the keyboard collection. A real keyboard
declares a modifier bitmap plus an N-usage rollover ARRAY -- several key codes
reportable at once. A macro pad declares a handful of individual buttons, usually
as a bitmap with no array at all.

This tool measures that shape, so the question is decided by evidence from real
devices rather than by assumption. Run it on a keyboard and on a macro-key mouse;
if the numbers separate cleanly, the discriminator is sound. If they do not, say
so -- a negative result here is worth more than a plausible guess in firmware.

See docs/experiments/ble-keyboard-classified-as-mouse-2026-08-29.md.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field

# HID 1.11 item prefixes, short-item form: bits 7..4 tag, 3..2 type, 1..0 size.
TYPE_MAIN, TYPE_GLOBAL, TYPE_LOCAL = 0, 1, 2

USAGE_PAGES = {
    0x01: "Generic Desktop",
    0x02: "Simulation",
    0x06: "Generic Device",
    0x07: "Keyboard/Keypad",
    0x08: "LEDs",
    0x09: "Button",
    0x0C: "Consumer",
    0x0D: "Digitizer",
    0xFF00: "Vendor",
}

DESKTOP_USAGES = {
    0x02: "Mouse",
    0x04: "Joystick",
    0x05: "Gamepad",
    0x06: "Keyboard",
    0x07: "Keypad",
    0x30: "X",
    0x31: "Y",
    0x38: "Wheel",
    0x80: "System Control",
}


@dataclass
class Field:
    """One Input item: a run of report bits with a usage meaning."""

    report_id: int
    usage_page: int
    usages: list[int]
    usage_min: int | None
    usage_max: int | None
    count: int
    size: int
    is_array: bool
    is_constant: bool
    logical_min: int
    logical_max: int
    is_relative: bool

    @property
    def bits(self) -> int:
        return self.count * self.size


@dataclass
class Collection:
    usage_page: int
    usage: int
    fields: list[Field] = field(default_factory=list)

    @property
    def name(self) -> str:
        if self.usage_page == 0x01:
            return DESKTOP_USAGES.get(self.usage, f"0x{self.usage:02X}")
        return f"page 0x{self.usage_page:02X} usage 0x{self.usage:02X}"


def parse(descriptor: bytes) -> tuple[list[Field], list[Collection]]:
    """Walk the descriptor, returning every Input field and top-level collection.

    Deliberately tolerant: an item this build does not understand is skipped
    rather than aborting the parse. The descriptor came off real hardware, and a
    tool that refuses to describe an unusual device is useless for exactly the
    devices worth investigating.
    """
    fields: list[Field] = []
    collections: list[Collection] = []
    current: Collection | None = None

    usage_page = 0
    report_id = 0
    report_size = 0
    report_count = 0
    logical_min = 0
    logical_max = 0
    usages: list[int] = []
    usage_min: int | None = None
    usage_max: int | None = None
    depth = 0

    i = 0
    while i < len(descriptor):
        prefix = descriptor[i]
        i += 1

        if prefix == 0xFE:  # long item: skip it wholesale
            if i >= len(descriptor):
                break
            size = descriptor[i]
            i += 2 + size
            continue

        size = prefix & 0x03
        if size == 3:
            size = 4
        item_type = (prefix >> 2) & 0x03
        tag = (prefix >> 4) & 0x0F

        if i + size > len(descriptor):
            break
        value = int.from_bytes(descriptor[i:i + size], "little") if size else 0
        signed = value
        if size and value >= (1 << (size * 8 - 1)):
            signed = value - (1 << (size * 8))
        i += size

        if item_type == TYPE_GLOBAL:
            if tag == 0x0:
                usage_page = value
            elif tag == 0x1:
                logical_min = signed
            elif tag == 0x2:
                logical_max = signed
            elif tag == 0x7:
                report_size = value
            elif tag == 0x8:
                report_id = value
            elif tag == 0x9:
                report_count = value
        elif item_type == TYPE_LOCAL:
            if tag == 0x0:
                usages.append(value)
            elif tag == 0x1:
                usage_min = value
            elif tag == 0x2:
                usage_max = value
        elif item_type == TYPE_MAIN:
            if tag == 0xA:  # Collection
                if depth == 0:
                    usage = usages[0] if usages else 0
                    current = Collection(usage_page, usage)
                    collections.append(current)
                depth += 1
            elif tag == 0xC:  # End Collection
                depth = max(0, depth - 1)
            elif tag == 0x8:  # Input
                entry = Field(
                    report_id=report_id,
                    usage_page=usage_page,
                    usages=list(usages),
                    usage_min=usage_min,
                    usage_max=usage_max,
                    count=report_count,
                    size=report_size,
                    is_array=not (value & 0x02),
                    is_constant=bool(value & 0x01),
                    logical_min=logical_min,
                    logical_max=logical_max,
                    is_relative=bool(value & 0x04),
                )
                fields.append(entry)
                if current is not None:
                    current.fields.append(entry)

            # Every Main item clears the local state. Missing this is the classic
            # descriptor-parser bug: usages leak into the next field and the map
            # comes out subtly wrong.
            usages = []
            usage_min = usage_max = None

    return fields, collections


@dataclass
class Verdict:
    keyboard_array_usages: int
    keyboard_bitmap_bits: int
    has_modifier_byte: bool
    pointer_relative_axes: int
    button_count: int
    consumer_fields: int

    @property
    def looks_like_a_keyboard(self) -> bool:
        """A rollover array is what a keyboard has and a macro pad does not."""
        return self.keyboard_array_usages >= 4 and self.has_modifier_byte

    @property
    def looks_like_a_pointer(self) -> bool:
        return self.pointer_relative_axes >= 2


def judge(fields: list[Field]) -> Verdict:
    array_usages = 0
    bitmap_bits = 0
    modifier = False
    relative_axes = 0
    buttons = 0
    consumer = 0

    for entry in fields:
        if entry.is_constant:
            continue

        if entry.usage_page == 0x07:
            if entry.is_array:
                # The rollover array: N simultaneously reportable key codes.
                array_usages = max(array_usages, entry.count)
            else:
                bitmap_bits += entry.bits
                # The modifier byte is eight 1-bit flags over 0xE0..0xE7.
                if (entry.usage_min, entry.usage_max) == (0xE0, 0xE7):
                    modifier = True
        elif entry.usage_page == 0x01 and entry.is_relative:
            relative_axes += sum(1 for usage in entry.usages if usage in (0x30, 0x31))
        elif entry.usage_page == 0x09:
            buttons += entry.count
        elif entry.usage_page == 0x0C:
            consumer += 1

    return Verdict(array_usages, bitmap_bits, modifier, relative_axes, buttons, consumer)


def descriptor_from_log(path: pathlib.Path) -> tuple[bytes, dict]:
    """Pull the `btid desc` reply out of a capture log."""
    context: dict = {}
    raw = ""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except ValueError:
            continue

        why = record.get("why", "")
        if why == "session.start":
            context["label"] = record.get("label", "")
        if why in {"cmd.kbm.status", "cmd.btid.dump"}:
            context[why] = record.get("line", "")
        if why == "cmd.btid.desc":
            raw = record.get("line", "")
        if why.startswith("uart.") and re.search(r"\[BTHID", record.get("line", "")):
            context.setdefault("bthid", []).append(record["line"])

    if not raw:
        raise SystemExit(
            f"{path}: no 'btid desc' reply in the log.\n"
            "The adapter answers that only while a HID peer is connected -- "
            "re-run the capture and press Enter AFTER the device has connected."
        )

    try:
        payload = json.loads(raw)
    except ValueError as error:
        raise SystemExit(f"{path}: 'btid desc' reply is not JSON: {error}") from error

    if "bytes" not in payload:
        raise SystemExit(
            f"{path}: the adapter reported no cached descriptor "
            f"({payload.get('error', 'unknown reason')})."
        )

    context["conn"] = payload.get("conn")
    context["len"] = payload.get("len")
    context["map"] = payload.get("map")
    return bytes.fromhex(payload["bytes"]), context


def render(descriptor: bytes, context: dict, label: str) -> str:
    fields, collections = parse(descriptor)
    verdict = judge(fields)

    out: list[str] = []
    add = out.append

    add(f"# HID descriptor: {label}")
    add("")
    add(f"- bytes: **{len(descriptor)}**")
    if context.get("conn") is not None:
        add(f"- connection index: {context['conn']}")
    if context.get("cmd.kbm.status"):
        add(f"- `kbm status`: `{context['cmd.kbm.status']}`")
    add("")

    add("## What the device declares")
    add("")
    add("| | |")
    add("|---|---|")
    add(f"| Keyboard rollover array | **{verdict.keyboard_array_usages}** usages |")
    add(f"| Keyboard bitmap bits | {verdict.keyboard_bitmap_bits} |")
    add(f"| Modifier byte (0xE0..0xE7) | {'yes' if verdict.has_modifier_byte else 'no'} |")
    add(f"| Relative X/Y axes | **{verdict.pointer_relative_axes}** |")
    add(f"| Buttons | {verdict.button_count} |")
    add(f"| Consumer-control fields | {verdict.consumer_fields} |")
    add("")

    add("## Verdict")
    add("")
    add(f"- looks like a keyboard: **{verdict.looks_like_a_keyboard}** "
        "(rollover array of 4+ usages AND a modifier byte)")
    add(f"- looks like a pointer: **{verdict.looks_like_a_pointer}** "
        "(two relative axes)")
    add("")
    if verdict.looks_like_a_keyboard and verdict.looks_like_a_pointer:
        add("**This is the ambiguous case.** The adapter currently resolves it as a "
            "MOUSE, because a BLE peer can never reach COMBO and pointer wins. If a "
            "macro-key mouse measures a *smaller* rollover array than this, the array "
            "size is a usable discriminator.")
    elif verdict.looks_like_a_keyboard:
        add("Keyboard only -- no pointer collection, so precedence never arises.")
    elif verdict.looks_like_a_pointer:
        add("Pointer only -- correctly a mouse.")
    else:
        add("Neither shape was recognised. Check the collections below: this may be "
            "a device the current parser does not model at all.")
    add("")

    add("## Top-level collections")
    add("")
    for collection in collections:
        add(f"### {collection.name}")
        add("")
        add("| report | usage page | usages | count x size | kind |")
        add("|---|---|---|---|---|")
        for entry in collection.fields:
            page = USAGE_PAGES.get(entry.usage_page, f"0x{entry.usage_page:02X}")
            # A truncated or unusual descriptor can leave one half of a usage
            # range set. The parser tolerates that by design, so the renderer has
            # to as well -- crashing here would refuse to describe exactly the
            # devices worth investigating.
            if entry.usage_min is not None and entry.usage_max is not None:
                usage_text = f"0x{entry.usage_min:02X}..0x{entry.usage_max:02X}"
            elif entry.usage_min is not None:
                usage_text = f"0x{entry.usage_min:02X}.. (no maximum declared)"
            elif entry.usage_max is not None:
                usage_text = f"..0x{entry.usage_max:02X} (no minimum declared)"
            elif entry.usages:
                usage_text = ", ".join(f"0x{u:02X}" for u in entry.usages[:6])
                if len(entry.usages) > 6:
                    usage_text += f" (+{len(entry.usages) - 6})"
            else:
                usage_text = "-"
            kind = "array" if entry.is_array else "bitmap/value"
            if entry.is_constant:
                kind = "padding"
            elif entry.is_relative:
                kind += ", relative"
            add(f"| {entry.report_id} | {page} | {usage_text} | "
                f"{entry.count} x {entry.size} | {kind} |")
        add("")

    if context.get("bthid"):
        add("## What the adapter decided")
        add("")
        add("```")
        for line in context["bthid"]:
            add(line)
        add("```")
        add("")

    add("## Raw descriptor")
    add("")
    add("```")
    for offset in range(0, len(descriptor), 16):
        chunk = descriptor[offset:offset + 16]
        add(f"{offset:04X}  " + " ".join(f"{b:02X}" for b in chunk))
    add("```")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", type=pathlib.Path,
                        help="a capture_kbm_pairing.ps1 log")
    parser.add_argument("--hex", help="descriptor bytes as hex, instead of a log")
    parser.add_argument("--label", default="", help="device name for the report")
    parser.add_argument("--out", type=pathlib.Path, help="write markdown here")
    args = parser.parse_args()

    if args.hex:
        descriptor = bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", args.hex))
        context: dict = {}
    elif args.log:
        descriptor, context = descriptor_from_log(args.log)
    else:
        parser.error("give a capture log or --hex")
        return 2

    label = args.label or context.get("label") or "unknown device"
    report = render(descriptor, context, label)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(report, encoding="utf-8")
        print(f"wrote {args.out}")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
