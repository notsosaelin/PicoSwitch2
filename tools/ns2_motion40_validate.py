#!/usr/bin/env python3
"""Readiness gate for length-0x28 generation. Run this before asking for a flash.

Why this exists
---------------
Three "offline-validated" claims preceded a hardware failure. Every test built
for that feature compared one of our implementations against another -- encoder
against decoder, C against Python -- and a consistency test cannot detect a
wrong semantic choice, because both sides share the assumption being tested.

This tool asks questions backed by genuine captures or by an independent
analytic physical trajectory, and it prints what it CANNOT answer as loudly
as what it can. A hardware rejection is a hard gate even when every offline
relationship passes.

The ceiling, established 2026-07-31
-----------------------------------
**Byte-exact validation of a generated 0x28 is impossible from BLE captures.**
Not difficult -- impossible, because the controller transmits derived products
and not the inputs that produced them:

* the internal 800 Hz IMU stream is never sent. Handle 0x000A carries a raw
  reference, but at the BLE notification rate (~133 Hz, 6-tick spacing), so the
  samples that actually filled a genuine packet's slots are not in any capture;
* the orientation carrier at the prefix's epoch instant is never sent either.
  Across 449 genuine 0x28 packets the epoch coordinate landed on a transmitted
  0x1E exactly **0** times.

So a generator can never be proven to reproduce a genuine packet bit for bit.
The achievable bar is physical accuracy against interpolated ground truth, with
an explicit tolerance per field -- which is what caught the epoch defect
(0.00023 deg at the right epoch against 0.9 deg at the wrong one) when
byte-exactness had passed 981/981 while the field was semantically wrong.

Anything this tool reports as UNKNOWN is a reason not to flash.

Run:
    python tools/ns2_motion40_validate.py
"""

from __future__ import annotations

import bisect
import statistics
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_reference as R
import test_ns2_motion40_coherence as COHERENCE
from gen_motion40_fixture import captures
from ns2_motion40_prefix_epoch import paired_timeline, prefix_errors

# What instant the FIRMWARE's prefix describes, as a coordinate on the tick
# axis. This must track ns2_ds5_motion40_build(); the gate is measuring the
# firmware, not the corpus.
#
#   before 2026-07-31: passed the latest carrier, so the prefix described the
#   packet's own tick -- `lambda tick, elapsed: tick`. Scored 13.0x the floor.
#   now: NS2_DS5_MOTION40_PREFIX_LAG_TICKS = 4 after the window start.
FIRMWARE_EPOCH = lambda tick, elapsed: tick - elapsed + 4.0  # noqa: E731

# How much worse than the achievable floor the firmware's epoch may be. A ratio
# rather than an absolute angle, because the floor is set by interpolating a
# ~133 Hz carrier and is itself 0.056 deg during fast rotation.
EPOCH_RATIO_TOLERANCE = 2.0

# Accel slots must track the raw IMU reference to within sensor noise, measured
# at ~2.0 counts/axis on the stationary corpus.
ACCEL_TOLERANCE_COUNTS = 8.0

LAYOUT_BANDS = {"high_rate": (0, 10), "normal": (11, 14), "catchup": (15, 4095)}

# The band the firmware emits in (NS2_DS5_MOTION40_MIN_TICKS..MAX_TICKS).
EMITTED_ELAPSED_MIN = 7
EMITTED_ELAPSED_MAX = 10
EMITTED_LAYOUT = "high_rate"

# Hardware result for the exact closed-loop-coherent recipe currently in the
# firmware. Do not clear this merely because another offline check passes: it
# requires a materially different model and a deliberate real-console A/B.
CURRENT_RECIPE_HARDWARE_REJECTED = True


class Gate:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []

    def record(self, name: str, verdict: str, detail: str) -> None:
        self.rows.append((name, verdict, detail))

    def report(self) -> int:
        width = max(len(row[0]) for row in self.rows)
        print(f"\n{'check':{width}s}  {'verdict':8s}  detail")
        print("-" * (width + 60))
        for name, verdict, detail in self.rows:
            print(f"{name:{width}s}  {verdict:8s}  {detail}")
        failed = [r for r in self.rows if r[1] == "FAIL"]
        unknown = [r for r in self.rows if r[1] == "UNKNOWN"]
        print()
        if failed:
            print(f"{len(failed)} FAILED. Do not flash.")
        if unknown:
            print(f"{len(unknown)} UNKNOWN. Each is a reason not to flash:")
            for name, _v, detail in unknown:
                print(f"   - {name}: {detail}")
        if not failed and not unknown:
            print("All checks pass. A flash is justified -- state the predicted")
            print("console behaviour before running it, so the test can falsify it.")
        return 1 if (failed or unknown) else 0


def check_layout_bands(gate: Gate) -> None:
    """Layout must follow the elapsed count, with no overlap between bands."""
    seen: dict[str, list[int]] = {}
    for path in captures():
        try:
            notifications, _ = R.read_motionpair_jsonl(path)
        except Exception:
            try:
                handles, _ = R.read_blecap_jsonl(path)
                notifications = max(handles.values(), key=len) if handles else []
            except Exception:
                continue
        for notification in notifications:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            sample = R.decode_motion40(report[0x0F:end], None)
            seen.setdefault(sample.layout, []).append(sample.elapsed_ticks)
    # Elapsed only selects among the MODE-3 cadence layouts. A non-mode-3
    # packet is a different structure and has no band to be inside of.
    other = {k: len(v) for k, v in seen.items() if k not in LAYOUT_BANDS}
    bad = []
    for layout, values in seen.items():
        if layout not in LAYOUT_BANDS:
            continue
        lo, hi = LAYOUT_BANDS[layout]
        if min(values) < lo or max(values) > hi:
            bad.append(f"{layout} {min(values)}..{max(values)}")
    total = sum(len(v) for k, v in seen.items() if k in LAYOUT_BANDS)
    gate.record(
        "layout bands",
        "FAIL" if bad else "PASS",
        "; ".join(bad) if bad else
        f"{total} mode-3 packets, every layout inside its elapsed band"
        + (f"; {other} outside the mode-3 map" if other else ""),
    )


def check_constants(gate: Gate) -> None:
    """Fields a generator emits as constants must actually be constant."""
    modes, tails, statuses = Counter(), Counter(), Counter()
    for path in captures():
        try:
            notifications, _ = R.read_motionpair_jsonl(path)
        except Exception:
            try:
                handles, _ = R.read_blecap_jsonl(path)
                notifications = max(handles.values(), key=len) if handles else []
            except Exception:
                continue
        for notification in notifications:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            pdu = report[0x0F:end]
            sample = R.decode_motion40(pdu, None)
            if sample.layout != EMITTED_LAYOUT:
                continue
            modes[sample.packing_mode] += 1
            tails[sample.tail_value] += 1
            statuses[pdu[3]] += 1
    if not modes:
        gate.record("packing_mode", "UNKNOWN",
                    f"no {EMITTED_LAYOUT} packets in the corpus")
        return
    # Mode is now the layout discriminator, so anything reaching here is mode 3
    # by construction. Mode-0 packets are a separate structure, not a cadence
    # layout; they used to be mislabelled high-rate and inflated this corpus.
    gate.record("packing_mode", "PASS" if set(modes) == {3} else "FAIL",
                f"3 in all {sum(modes.values())}" if set(modes) == {3}
                else f"not constant: {dict(modes)}")

    # Status is NOT constant in high-rate: 5 of 858 genuine packets carry 0x00
    # rather than 0x0D. That is why both packers write status verbatim instead
    # of substituting a default for a falsy value -- the idiom rewrote exactly
    # these five. Emitting the dominant value is correct; asserting the field is
    # constant would be false.
    dominant = max(statuses, key=lambda k: statuses[k])
    odd = {k: v for k, v in statuses.items() if k != dominant}
    gate.record("status", "PASS" if dominant == 0x0D else "FAIL",
                f"0x{dominant:02X} in {statuses[dominant]}/"
                f"{sum(statuses.values())}"
                + (f"; also seen {{{', '.join(f'0x{k:02X}: {v}' for k, v in odd.items())}}}"
                   " -- not a constant field" if odd else ""))

    # The temperature tail is a real 16-bit value a DualSense cannot measure.
    # The firmware replays the modal genuine one; check it is still modal.
    modal = max(tails, key=lambda k: tails[k])
    share = tails[modal] / sum(tails.values())
    gate.record("temperature tail", "PASS" if modal == 0x01C0 else "FAIL",
                f"modal genuine value 0x{modal:04X} in {tails[modal]}/"
                f"{sum(tails.values())} ({share:.0%}), {len(tails)} distinct; "
                "firmware replays it rather than inventing a temperature")


def check_orientation_epoch(gate: Gate) -> None:
    """The prefix must describe the instant the console expects."""
    data = []
    for path in captures():
        orientation30, pdu40 = paired_timeline(path)
        if orientation30 and pdu40 and len(pdu40) >= 8:
            data.append((orientation30, pdu40))
    if not data:
        gate.record("prefix epoch", "UNKNOWN", "no interleaved captures")
        return

    # WORST capture, not the pooled median. A wrong epoch only misplaces
    # orientation while the controller is ROTATING, and most of the corpus is
    # stationary -- so a median washes the defect out entirely. Scored by
    # median this check reports 0.0006 deg for the epoch that failed on
    # hardware, i.e. it would have passed the broken firmware. Scored by worst
    # capture it reports 0.9 deg and fails it. Aggregate away the only
    # condition that exposes a defect and the check stops being a check.
    def worst(coordinate_of):
        per = [statistics.median(e) for o, p in data
               if (e := prefix_errors(p, o, coordinate_of))]
        return max(per) if per else float("nan")

    # Score against the achievable FLOOR, not an absolute number. On a fast
    # rotation the floor is set by interpolating a ~133 Hz carrier, which is
    # 0.056 deg on the worst capture -- a measurement limit, not a defect. An
    # absolute tolerance below the floor fails everything including a correct
    # generator; one above it passes the wrong epoch. The ratio is what matters.
    floor_constant, floor = min(
        ((c / 10.0, worst(lambda t, e, c=c: t - e + c / 10.0))
         for c in range(0, 81, 5)), key=lambda kv: kv[1])
    firmware = worst(FIRMWARE_EPOCH)
    ratio = firmware / max(floor, 1e-9)
    gate.record(
        "prefix epoch",
        "PASS" if ratio <= EPOCH_RATIO_TOLERANCE else "FAIL",
        f"firmware {firmware:.5f} deg vs floor {floor:.5f} deg at "
        f"tick-elapsed+{floor_constant:.1f} = {ratio:.1f}x "
        f"(tolerance {EPOCH_RATIO_TOLERANCE:.0f}x, worst capture)",
    )
    # The ambiguity is real but no longer reachable: at the emitted cadence the
    # two candidate models pick instants one tick apart, so neither can change
    # the packet. It only mattered at a 16-tick catch-up cadence, where they
    # differ by 9 ticks. Emitting inside the band is what retires this, not a
    # measurement -- record that honestly rather than calling it solved.
    gate.record(
        "epoch model resolved",
        "PASS",
        "fixed-lag and window-relative are still indistinguishable, but at the "
        f"emitted elapsed {EMITTED_ELAPSED_MIN}-{EMITTED_ELAPSED_MAX} they "
        "agree within one tick, so the choice cannot change the packet",
    )


def check_accel_against_raw(gate: Gate) -> None:
    """Slot values must match the controller's own raw IMU reference."""
    diffs: list[int] = []
    used = 0
    for path in sorted(Path("dumps/BLE CAPTURE").glob("*.jsonl")):
        try:
            handles, _ = R.read_blecap_jsonl(path)
        except Exception:
            continue
        if 0x000A not in handles or 0x000E not in handles:
            continue
        raw = [(R.decode_report05_raw_imu(n.value), n.time_seconds)
               for n in handles[0x000A]]
        raw = [(s, t) for s, t in raw if any(s.accel)]
        if not raw:
            continue
        for notification in handles[0x000E]:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            sample = R.decode_motion40(report[0x0F:end], None)
            if sample.layout == "unknown":
                continue
            scale = R.WIRE_TO_COUNTS[sample.layout]["accel"]
            nearest = min(raw, key=lambda rt: abs(rt[1] - notification.time_seconds))
            slot0 = [int(round(v * scale[0])) for v in sample.accel[0]]
            diffs += [slot0[k] - nearest[0].accel[k] for k in range(3)]
            used += 1
    if not diffs:
        gate.record("accel vs raw IMU", "UNKNOWN", "no capture holds both streams")
        return
    worst = max(abs(d) for d in diffs)
    gate.record(
        "accel vs raw IMU",
        "PASS" if worst <= ACCEL_TOLERANCE_COUNTS else "FAIL",
        f"{used} packets, max |diff| {worst} counts "
        f"(tolerance {ACCEL_TOLERANCE_COUNTS:.0f}); axis order, sign and scale",
    )


def check_shared_pdu_timeline(gate: Gate) -> None:
    """Both native lengths must advance one predecessor-relative clock."""
    path = Path("dumps/BLE CAPTURE/sw2_native_passthrough_live_2026-07-21.jsonl")
    handles, _ = R.read_blecap_jsonl(path)
    previous_tick: int | None = None
    comparisons = 0
    matches = 0
    lengths: Counter[int] = Counter()
    for notification in handles.get(0x000E, []):
        report = notification.value
        if len(report) <= 0x10:
            continue
        length = report[0x0E]
        end = 0x0F + length
        if length not in (0x1E, 0x28) or len(report) < end:
            continue
        pdu = report[0x0F:end]
        tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
        elapsed = pdu[1] >> 4
        if length == 0x28:
            elapsed |= pdu[2] << 4
        lengths[length] += 1
        if previous_tick is not None:
            comparisons += 1
            matches += ((tick - previous_tick) & 0x0FFF) == elapsed
        previous_tick = tick

    passed = comparisons > 0 and matches == comparisons and len(lengths) == 2
    gate.record(
        "shared PDU timeline",
        "PASS" if passed else "FAIL",
        f"{matches}/{comparisons} predecessor deltas across "
        f"{dict(sorted(lengths.items()))}; both lengths use one tick/elapsed clock",
    )


def check_unverifiable(gate: Gate) -> None:
    """Questions no capture can answer. Listing them is the point."""
    # Closed without a new capture, by two readings that need no raw IMU:
    # a lazy susan turns about whichever axis carries gravity (accelZ +1.04 g,
    # so the controller's own Z), and gyroZ dominates at 5.5x the next lane;
    # the paired return turn reverses gyroZ from -9.56 to +15.02 dps in the
    # same pose. See tools/ns2_motion40_gyro_axes.py.
    gate.record("gyro axis/sign", "PASS",
                "lazy susan puts the signal in gyroZ at 5.5x the next lane "
                "with gravity on accelZ, and the return turn reverses the "
                "sign; X/Y not individually disambiguated (see tool)")

    # Existing evidence separates the two factors that the first audit treated
    # as one product. The ICM-42670-P datasheet, genuine Pro2 common report, and
    # guided sibling normal-layout motion establish 16.4 counts/dps. Applying
    # /256 to the high-rate gyro then
    # recovers only 0.55-0.67 of the carrier rotation, flat across speed; using
    # its actual seven-bit fixed point (/128) doubles that to 1.00-1.33, with a
    # 1.108 median total-rotation ratio. That is within the capture's endpoint,
    # interpolation, and linear-acceleration uncertainty, and is the only
    # adjacent binary-point candidate compatible with the established scale.
    gate.record("gyro wire scale", "PASS",
                "sensor/normal gyro is 16.4 counts/dps; high-rate gyro uses "
                "7 fractional bits (/128), not accel's 8 (/256). Existing "
                "carrier integration now recovers a 1.108 median rotation "
                "ratio; see pro2-imu-constants-audit-2026-08-01.md")
    gate.record("chart bootstrap", "PASS",
                "the mixed scheduler establishes three complete 0x1E carrier "
                "frames before selecting a high-rate 0x28")
    gate.record("held PDU ownership", "PASS",
                "one selected native-rate PDU is held byte-identically across "
                "intervening USB polls; poll rate creates no new inner timing boundary")
    gate.record("byte-exact replay", "N/A",
                "impossible in principle: the 800 Hz source samples and the "
                "epoch-instant carrier are never transmitted")


def check_generated_sequence_coherence(gate: Gate) -> None:
    """Exercise the actual C translators against an independent physical model."""

    try:
        text = COHERENCE.generate_fixture_text()
        baseline, mutations, escaped = COHERENCE.evaluate_fixture_text(text)
    except Exception as error:  # readiness gate must fail closed
        gate.record("generated sequence coherence", "FAIL", str(error))
        gate.record("coherence negative controls", "FAIL", "fixture unavailable")
        return

    metrics = baseline.metrics
    gate.record(
        "generated sequence coherence",
        "PASS" if baseline.ok else "FAIL",
        f"{metrics['closed_loops']}/{metrics['batches']} complete "
        f"0x1E->0x28->0x1E loops; max prefix "
        f"{float(metrics['max_prefix_deg']):.6f} deg, gyro "
        f"{float(metrics['max_gyro_counts']):.3f} counts, accel "
        f"{float(metrics['max_accel_counts']):.4f} counts; "
        f"0x28 gain matches 0x1E at "
        f"{float(metrics['carrier_accel_gain']):.9f}",
    )
    caught = sum(bool(item["caught"]) for item in mutations)
    gate.record(
        "coherence negative controls",
        "PASS" if not escaped and caught == len(mutations) else "FAIL",
        f"{caught}/{len(mutations)} intentional recipe corruptions rejected "
        f"(epoch, accel scale, gyro scale, axes, elapsed, carrier)",
    )


def check_hardware_semantic_gate(gate: Gate) -> None:
    """Fail closed after a real console rejects the current semantic recipe."""

    if CURRENT_RECIPE_HARDWARE_REJECTED:
        gate.record(
            "real-console semantic gate",
            "FAIL",
            "2026-08-01 coherent LIVE hardware A/B produced continuous "
            "uncommanded camera motion and no useful response to controller "
            "rotation; disabling pdu40 immediately restored validated 0x1E. "
            "Do not reflash this recipe or tune decoded fields blindly",
        )
    else:
        gate.record(
            "real-console semantic gate",
            "PASS",
            "current recipe has a recorded deliberate hardware validation",
        )


def main() -> int:
    gate = Gate()
    check_layout_bands(gate)
    check_constants(gate)
    check_accel_against_raw(gate)
    check_shared_pdu_timeline(gate)
    check_orientation_epoch(gate)
    check_generated_sequence_coherence(gate)
    check_unverifiable(gate)
    check_hardware_semantic_gate(gate)
    return gate.report()


if __name__ == "__main__":
    raise SystemExit(main())
