#!/usr/bin/env python3
"""Synthesize genuine-shaped Switch 2 motion PDUs.

This is the exact inverse of ``ns2_motion_reference.decode_motion40`` and of the
length-`0x1E` decode. It builds complete packets: preamble, mode-3 orientation
carrier, packed multi-sample IMU payload, and the temperature tail.

Its correctness bar is byte-exactness, not plausibility: every genuine `0x28` in
the repository corpus decodes and re-encodes to the identical 40 bytes
(``test_ns2_motion_packet.py``). A generator that cannot reproduce real packets
has no business emitting synthetic ones.

Nothing here is wired into firmware. Production DualSense/Edge motion remains on
the validated length-`0x1E` path, and `AGENTS.md` records that a previous
template-derived `0x28` generator caused random motion on hardware and was
removed. Enabling this requires a gated hardware A/B, not a merge.

Layout selection is driven by the packet's own elapsed count, exactly as the
controller's status byte reports it:

    elapsed 0..10   high-rate   status 0x0D   accel22, gyro22, accel22
    elapsed 11..14  normal      status 0x0E   accel14, gyro13, accel13, gyro14, accel14
    elapsed 15+     catch-up    status 0x0F   accel14, gyro16, accel13, gyro16, accel14

Sample-count note
-----------------
The layouts demand two or three acceleration samples and one or two gyro
samples per packet, at the controller's 800 Hz internal tick. A translated
source reporting more slowly than that has fewer real samples than slots. This
module never invents data: the caller supplies exactly the samples each layout
needs and is responsible for how they were obtained. ``LAYOUT_SAMPLE_COUNTS``
exposes the requirement so a caller can check before building.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence

PDU_SIZE = 0x28
PAYLOAD_BITS = 288
CARRIER_PDU_SIZE = 0x1E

STATUS_HIGH_RATE = 0x0D
STATUS_NORMAL = 0x0E
STATUS_CATCHUP = 0x0F

# (accel samples, gyro samples) each layout must be given.
LAYOUT_SAMPLE_COUNTS = {
    "high_rate": (2, 1),
    "normal": (3, 2),
    "catchup": (3, 2),
}

# (bit offset, width) per vector, in the order the decoder reads them.
_LAYOUT_FIELDS = {
    "high_rate": {
        "accel": ((74, 22), (206, 22)),
        "gyro": ((140, 22),),
        "carrier": ((2, 24), (26, 23), (49, 25)),
        "tail": (272, 16),
    },
    "normal": {
        "accel": ((68, 14), (149, 13), (230, 14)),
        "gyro": ((110, 13), (188, 14)),
        "carrier": ((2, 22), (24, 21), (45, 23)),
        "tail": (272, 16),
    },
    "catchup": {
        "accel": ((68, 14), (158, 13), (245, 14)),
        "gyro": ((110, 16), (197, 16)),
        "carrier": ((2, 22), (24, 21), (45, 23)),
        "tail": (287, 1),
    },
}


class PacketError(ValueError):
    """A field does not fit its slot, or a required sample is missing."""


def counts_to_wire(layout: str, kind: str, slot: int,
                   vector: Sequence[float]) -> tuple[int, int, int]:
    """Convert one IMU sample from ordinary ICM counts to this slot's wire form.

    Each layout packs its slots at a different fixed-point scale, and slot width
    does not determine it -- see ``ns2_motion_reference.WIRE_TO_COUNTS`` for the
    measured factors and their corpus-wide verification.

    A generator that skips this emits values off by up to 256x. They pack
    without error, decode without error, and are wrong: high-rate acceleration
    supplied in ordinary counts reads back as 1/256 g. Nothing downstream can
    detect that, which is why the conversion lives here rather than in callers.
    """

    import ns2_motion_reference as _reference

    if len(vector) != 3:
        raise PacketError("IMU vectors have three axes")
    factor = _reference.wire_to_counts(layout, kind, slot)
    return tuple(int(round(component / factor)) for component in vector)


# The controller runs in two observed emission modes, and the 12-bit elapsed
# count means different things in each. A generator that picks the wrong one
# mislabels every packet's layout.
#
# Measured across the corpus, the split is perfect -- no capture is mixed:
#
#   0x28-only mode   (14 captures, 1,210 packets, zero 0x1E)
#       elapsed == tick delta since the previous 0x28, in 100.0% of packets.
#       The pro2-native-interval-* sweeps run in this mode from 8 to 24 ticks.
#
#   interleaved mode (24 captures, zero 0x1E excluded)
#       elapsed == tick delta since the previous 0x28 in ~0% of packets. It
#       instead counts back only to the most recent PDU of ANY length, sitting
#       near a constant 7 while the 0x28-to-0x28 delta varies over 11..30.
#       elapsed * (intervening carriers + 1) predicts that delta but overshoots
#       by 3..5 ticks; the exact relation is NOT resolved and is not needed.
#
# TRANSLATION POLICY: emit 0x28-only. It is a genuine hardware mode, its elapsed
# rule is exactly verified, and it removes the unresolved interleaved semantics
# from the problem entirely. ``elapsed_ticks`` then carries the emit interval
# directly, which is what ``layout_for_elapsed`` expects.
#
# Slot budget at 800 Hz, for a source reporting near 250 Hz:
#   elapsed  8 (10.0 ms) -> high_rate, needs 2 accel + 1 gyro  (~2.5 available)
#   elapsed 12 (15.0 ms) -> normal,    needs 3 accel + 2 gyro  (~3.8 available)
#   elapsed 24 (30.0 ms) -> catchup,   needs 3 accel + 2 gyro  (~7.5 available)
# The longer intervals carry comfortable margin; high-rate is marginal and a
# source slower than 200 Hz cannot fill it without inventing samples, which
# this module refuses to do.
TRANSLATION_MODE = "0x28-only"


def layout_for_elapsed(elapsed_ticks: int) -> str:
    if elapsed_ticks < 0 or elapsed_ticks > 0xFFF:
        raise PacketError(f"elapsed ticks out of range: {elapsed_ticks}")
    if elapsed_ticks >= 15:
        return "catchup"
    if elapsed_ticks < 11:
        return "high_rate"
    return "normal"


def status_for_layout(layout: str) -> int:
    return {"high_rate": STATUS_HIGH_RATE,
            "normal": STATUS_NORMAL,
            "catchup": STATUS_CATCHUP}[layout]


class _BitWriter:
    """LSB-first bit packer over a fixed-size payload."""

    def __init__(self, size_bytes: int) -> None:
        self._value = 0
        self._size = size_bytes

    def put(self, offset: int, width: int, value: int) -> None:
        if offset < 0 or width <= 0 or offset + width > self._size * 8:
            raise PacketError(
                f"bit field [{offset}, {offset + width}) exceeds "
                f"{self._size * 8}-bit payload")
        mask = (1 << width) - 1
        # Two's-complement wrap is intended: several fields are modular windows.
        self._value |= (value & mask) << offset

    def put_signed(self, offset: int, width: int, value: int) -> None:
        limit = 1 << (width - 1)
        if not -limit <= value < limit:
            raise PacketError(
                f"signed value {value} does not fit {width} bits at {offset}")
        self.put(offset, width, value)

    def put_vector(self, offset: int, width: int,
                   vector: Sequence[int]) -> None:
        if len(vector) != 3:
            raise PacketError("IMU vectors have three axes")
        for axis, component in enumerate(vector):
            self.put_signed(offset + axis * width, width, component)

    def bytes(self) -> bytes:
        return self._value.to_bytes(self._size, "little")


@dataclass(frozen=True)
class MotionPacketFields:
    """Everything one length-0x28 PDU carries."""

    tick: int
    elapsed_ticks: int
    carrier: tuple[int, int, int]
    accel: tuple[tuple[int, int, int], ...]
    gyro: tuple[tuple[int, int, int], ...]
    tail_value: int = 0
    packing_mode: int = 3
    status: int | None = None


def encode_temperature_tail16(integer_part: int, low_a: int, low_b: int) -> int:
    """Pack two Q3 temperature samples sharing a signed ten-bit integer part."""

    if not -512 <= integer_part <= 511:
        raise PacketError(f"temperature integer part out of range: {integer_part}")
    if not 0 <= low_a <= 7 or not 0 <= low_b <= 7:
        raise PacketError("temperature fractional eighths must be 0..7")
    return ((integer_part & 0x3FF) << 6) | ((low_b & 7) << 3) | (low_a & 7)


def build_motion40(fields: MotionPacketFields) -> bytes:
    """Build one complete 40-byte motion PDU."""

    layout = layout_for_elapsed(fields.elapsed_ticks)
    spec = _LAYOUT_FIELDS[layout]
    want_accel, want_gyro = LAYOUT_SAMPLE_COUNTS[layout]
    if len(fields.accel) != want_accel or len(fields.gyro) != want_gyro:
        raise PacketError(
            f"{layout} needs {want_accel} accel and {want_gyro} gyro samples, "
            f"got {len(fields.accel)} and {len(fields.gyro)}")
    if not 0 <= fields.tick <= 0xFFF:
        raise PacketError(f"tick out of range: {fields.tick}")
    if not 0 <= fields.packing_mode <= 3:
        raise PacketError(f"packing mode out of range: {fields.packing_mode}")

    writer = _BitWriter(PDU_SIZE - 4)
    writer.put(0, 2, fields.packing_mode)
    for lane, (offset, width) in enumerate(spec["carrier"]):
        writer.put_signed(offset, width, fields.carrier[lane])
    for sample, (offset, width) in zip(fields.accel, spec["accel"]):
        writer.put_vector(offset, width, sample)
    for sample, (offset, width) in zip(fields.gyro, spec["gyro"]):
        writer.put_vector(offset, width, sample)
    tail_offset, tail_width = spec["tail"]
    writer.put(tail_offset, tail_width, fields.tail_value)

    status = fields.status if fields.status is not None \
        else status_for_layout(layout)
    # Preamble: 12-bit tick, then the 12-bit elapsed count split across the
    # high nibble of byte 1 and all of byte 2, then the layout/status byte.
    header = bytes((
        fields.tick & 0xFF,
        ((fields.tick >> 8) & 0x0F) | ((fields.elapsed_ticks & 0x0F) << 4),
        (fields.elapsed_ticks >> 4) & 0xFF,
        status & 0xFF,
    ))
    return header + writer.bytes()


# Byte 12 bit 7 of the length-0x1E carrier is a real, undocumented field.
#
# Measured over 2,070 genuine carriers (docs/experiments/pro2-carrier-unknown-
# fields-2026-07-31.md):
#   - set in 280 (13.5%);
#   - it tracks MOTION. Median inter-record carrier change is 0.001280 when set
#     versus 0.000012 when clear -- a 100x separation. It is 0% across
#     stationary captures and 23-48% across moving ones;
#   - it is not a simple threshold on that change (precision only 21%), and it
#     alternates rapidly within moving captures;
#   - ruled out: sign of any retained lane, lane-1 bit extension, tick parity,
#     autocorrelation, retained energy > 0.75 or > 1.0, proximity to saturation,
#     second-largest magnitude. None beat the 86.5% trivial-zero baseline.
#
# REFUTED: it is not the whole-quaternion sign canonicalization. If it were,
# negating the later record would restore continuity whenever the flag toggles.
# Across 160 same-chart toggles that never happens (0/160); the trajectory is
# already smooth without negation (median 0.0025 versus 1.32 negated). Also not
# related to the 0x1E/0x28 interleaving or the cadence: the rate is a flat
# 12-14% whichever slice you take.
#
# It remains characterized but unexplained.
#
# Bits 2..6 of this byte, and bits 2..7 of bytes 4 and 8, are zero in every
# genuine record; bit 1 is never set, which confirms lane 1 is 25 bits, not 26.
#
# The bit is preserved verbatim rather than guessed at, because a synthesizer
# that zeroes an unexplained changing lane is exactly the refuted static-template
# mistake recorded in AGENTS.md.
MOTION30_BYTE12_FLAG = 0x80


def build_motion30(state: int, carrier_raw: Sequence[int],
                   header: Sequence[int] = (0, 0, 0, 0, 0),
                   byte12_flag: int = 0) -> bytes:
    """Build one 30-byte orientation carrier PDU.

    Lane packing mirrors ``decode_motion30_orientation``: lanes 0 and 1 occupy
    bytes 5-8 and 9-12, lane 2 is split so that its top two bits live in the low
    two bits of byte 4 as the chart state. ``byte12_flag`` carries the
    unexplained bit documented above; pass the genuine value when reproducing a
    captured packet.
    """

    if len(carrier_raw) != 3:
        raise PacketError("carrier needs three lanes")
    if not 0 <= state <= 3:
        raise PacketError(f"chart state out of range: {state}")
    if byte12_flag not in (0, 1):
        raise PacketError("byte12 flag is one bit")
    for lane, bits in enumerate((26, 25, 24)):
        if not 0 <= carrier_raw[lane] < (1 << bits):
            raise PacketError(f"lane {lane} does not fit {bits} bits")

    pdu = bytearray(CARRIER_PDU_SIZE)
    pdu[0:5] = bytes(header[:5])
    g0, g1, g2 = carrier_raw
    pdu[4] = (pdu[4] & 0xFC) | (state & 3)
    pdu[5] = g0 & 0xFF
    pdu[6] = (g0 >> 8) & 0xFF
    pdu[7] = (g0 >> 16) & 0xFF
    pdu[8] = (pdu[8] & 0xFC) | ((g0 >> 24) & 3)
    pdu[9] = g1 & 0xFF
    pdu[10] = (g1 >> 8) & 0xFF
    pdu[11] = (g1 >> 16) & 0xFF
    pdu[12] = ((g1 >> 24) & 3) | (MOTION30_BYTE12_FLAG if byte12_flag else 0)
    pdu[13] = g2 & 0xFF
    pdu[14] = (g2 >> 8) & 0xFF
    pdu[15] = (g2 >> 16) & 0xFF
    return bytes(pdu)
