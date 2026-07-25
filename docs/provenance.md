# Provenance Ledger — What This Project Contributed vs. Inherited

> An honest attribution of where the project's knowledge came from. It exists so a future
> contributor can tell, at a glance, what PicoSwitch2 **discovered**, what it **independently
> confirmed**, and what it **inherited** — and so the work is legible as more than "a fork of
> ndeadly's notes."
>
> Two upstreams underpin almost everything:
> - **ndeadly / `switch2_controller_research`** — controller-side protocol documentation (descriptors,
>   commands `0x01–0x18`, memory layout, report layouts), gathered by observing *genuine* controllers.
>   Mirrored here at `nso-gc-refs/switch2_controller_research/`.
> - **Dycool / `NS-PC-Control` & `Usb-relay-for-NS`** — emulation/relay of genuine Switch 2 controllers
>   from a Linux USB gadget (Raspberry Pi).
>
> **The core distinction this ledger draws:** ndeadly documented the **vocabulary** a genuine Switch 2
> controller speaks; Dycool proved a genuine speaker can be **relayed**. PicoSwitch2 is the one
> building a controller **from scratch** and thereby discovering the console's **acceptance
> criteria** — what the console *demands* to believe a device is native. That acceptance layer is
> where our original ground is; the vocabulary layer is mostly inherited or cross-validated.
>
> Follows the repo [authority order](README.md#authority-order): claims cite captures/code first,
> archive last. Confidence: **Confirmed · Strong · Hypothesis**.

---

## Tier A — Original (not present in ndeadly or Dycool)

These are contributions that come *only* from being the impostor talking to a live, unhacked console
— a vantage neither upstream occupies (ndeadly observes sealed controllers; Dycool relays a genuine
one).

| # | Contribution | Why it's original | Evidence | Confidence |
|---|---|---|---|---|
| A1 | **The console requires a byte-exact EP0 vendor identity handshake (`bRequest` 0x02/03/04) to classify a device — invisible to PC hosts.** | The project's central acceptance fact. Concretely *ahead of Dycool*: the `Usb-relay-for-NS` audit found its Pi gadget has **no EP0/control-transfer relay at all** — Dycool's implementation doesn't even account for what we found the console gates on. | `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`; `docs/switch2-joycon2/protocol.md` "What this does NOT tell us"; code: the per-personality identity handlers | **Confirmed** (hardware: Pro2/GC enumerate on a real console) |
| A2 | **`bcdDevice` is not gated by the console, but *is* by the Windows WinUSB driver cache** — reusing the real value verbatim causes Code 28 collisions with genuine hardware. | Pure emulation-integration knowledge; a Windows-host dev artifact neither reference repo would ever contain. | `src/switch_gc/switch_gc.c:57-76` (the deliberate 1.11 vs real 1.01 deviation + reasoning); mirrored in Pro2/Joy-Con2 descriptors | **Confirmed** (first hardware test) |
| A3 | **The console actively *reads* factory address `0x13100` (motion cal, `0x02/04` len `0x18`) during init and expects non-zero data.** | "The console reads this address at init" is an acceptance fact, not a controller fact — caught by decoding a genuine console-side session while our firmware returned zero-fill. | `src/switch_pro2/switch_pro2.c:283-289` | **Confirmed** (independent console-side capture) |
| A4 | **Joy-Con 2's report ID `7` is the console-facing *extended* report** (the role Pro2/GC fill with `0x0A`). | Pinned from the project's *own* live HID-Report-descriptor replug capture; the repo flags it "new, previously-unknown information," not cross-validation. | `docs/switch2-joycon2/protocol.md:40,162-164` | **Confirmed** (own capture) |
| A5 | **A body of *refuted* protocol hypotheses** (GC rumble `data[0]` is not a linear amplitude; "any nonzero rumble byte = on" is false; several motion models rejected). | Negative results are original contributions; neither upstream published these, and some were cross-checked against the real Linux `HID: nintendo` driver source. | `docs/experiments/refuted-hypotheses.md` | **Confirmed** (refutations) |
| A6 | **Serial-number *structure* decode** — `0x13002` model code = chars 1–2 (`HB/HC/HE/HH`), per-unit tail; `0x13E00` classified as a lot/batch code. | ndeadly's `memory_layout.md` labels `0x13002` "Serial number" with raw bytes only; the structural analysis (from six genuine units) is ours. | `docs/switch2/serial-generation.md` | **Strong** (chars 1–2 Confirmed; field split Strong) |
| A7 | **Wake-a-sleeping-console behavior** — identity learned from the `0x15` pairing exchange, persisted, first non-neutral report arms wake. | An *emulator* behavior (make the console wake), not protocol documentation either upstream produces. | `docs/bluetooth/wake-from-sleep-design.md` | **Confirmed** (hardware, Pro2) |
| A8 | **Mouse-mode enable is feature bit 4 (`0x0C` mask `0x37`), refuting an earlier "command `0x13` = mouse" idea; relative report `0x07/08` deltas byte-verified.** | Resolved a wrong hypothesis and byte-verified against a decrypted BLE capture decoded in-repo. | `MOUSE-MODE.md`; `docs/experiments/2026-07-19-usb-command-ab-diff.md` (Exp 2) | **Confirmed** (capture) |

## Tier B — Independent confirmation (raised single-source → multi-source)

Not novel facts, but real evidentiary value: turning "one person captured this once" into
multi-unit / cross-project **Confirmed**.

- **Second-physical-unit confirmations.** Factory/identity bytes and the `bRequest=3` identity block
  cross-validated byte-for-byte against the owner's *own* genuine units — a second independent unit
  behind claims ndeadly recorded from one. (Archive: `docs/archive/status-through-2026-07-15.archived.md`
  ~1602,1610.)
- **Dycool cross-validation.** `Usb-relay-for-NS`'s `0x0C` feature-enable bytes (configure/enable,
  mask `0x27`) match this project's own documented bytes exactly — genuine cross-validation, not a
  guess. (`docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`.)
- **Live descriptor re-capture.** Device/config/HID-Report descriptors re-captured from the owner's
  own hardware, byte-for-byte matching ndeadly's `descriptors.md` — good confidence, not new fact.

## Tier C — Inherited foundation (credit where due)

The load-bearing majority of the raw protocol came from upstream and should be cited as such:

- **From ndeadly:** the command set `0x01–0x18` and subcommands, factory memory map (`0x13000`
  region), input/output report field layouts, feature-flag table (`0x0C`), and the initial descriptor
  reference. Mirrored at `nso-gc-refs/switch2_controller_research/`.
- **From Dycool:** confirmation that from-a-gadget emulation/relay of a genuine Pro2 is viable, and a
  reference point for report cadence and feature negotiation.

Where this repo re-states an upstream fact, it is expected to carry a confidence tag and, ideally, a
second source — see [`re-methodology/evidence-standards.md`](re-methodology/evidence-standards.md).

## The shared, still-unsolved frontier

Honesty check: the biggest open problem is **not** something we (or anyone) cracked.

- **Console-native motion / gyro (`0x09`)** remains unresolved by *everyone*, Dycool included. Every
  `0x09` semantic fact still traces to one static, unrepeatable third-party capture; multiple models
  were tested and refuted without a positive replacement. See PLAN.md "Console-native motion" and
  `docs/switch2/ble-controller-protocol-inventory.md`. This is the clearest evidence that the
  *vocabulary* frontier is still closed for the whole field — our new ground is on the acceptance
  layer, not here.

## Why this distinction matters

Nearly every hard blocker (motion, NFC, opaque identity bytes) is gated on the same missing
capability — **seeing/probing what the console does** — which is exactly the acceptance layer Tier A
lives in and the reason [`uart-trace-tooling.md`](switch2/uart-trace-tooling.md) prioritizes an on-device tracer + fault
injection. Fault injection in particular is *structurally impossible* on the sealed genuine hardware
ndeadly and Dycool observe, and is the project's clearest unique future advantage.

## References

- Upstream mirror: `nso-gc-refs/switch2_controller_research/` (ndeadly).
- Dycool audit: `docs/experiments/usb-relay-feasibility-audit-2026-07-10.md`.
- Acceptance-layer evidence: `src/switch_gc/switch_gc.c:57-76` (bcdDevice/WinUSB),
  `src/switch_pro2/switch_pro2.c:283-289` (`0x13100` read at init),
  `docs/switch2-joycon2/protocol.md:40,162-164` (report ID 7).
- Refutations: `docs/experiments/refuted-hypotheses.md`.
- Project analyses: `switch2/serial-generation.md`, `switch2-joycon2/mapping.md`, and the
  Bluetooth mouse implementation documented in the compatibility matrix.
- Evidence rules: [`re-methodology/evidence-standards.md`](re-methodology/evidence-standards.md).
