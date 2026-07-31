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
| A9 | **The NFC command surface beyond `0x15`** — subcommands `0x1E` (sector-aware reuse read), `0x20` (complete extended operation), and `0x21` (execute staged device command) are absent from ndeadly's `commands.md` table entirely; `0x03`/`0x04` are listed there as Unknown and are decoded here as poll and stop. The `0x14` staging surface is further split into three capture-derived envelope families. | Only a device *serving* a tag to a live console sees the console drive these; a sealed-controller observer never elicits them. | [`../docs/Amiibo-v3.md`](Amiibo-v3.md); [`switch2/controller-safe-mode.md`](switch2/controller-safe-mode.md) has the side-by-side table | **Confirmed** (hardware read + write) |
| A10 | **Figure-v3 (NTAG I2C Plus 2K) amiibo read *and* write**, including the allocation-relative Air Riders slot geometry — slot *n* at sector-0 page `0x92 + 8n` and sector-1 page `25n`, ten slots per tag. | No upstream covers 2048-byte v3 amiibo at all. | [`Amiibo-v3.md`](Amiibo-v3.md) §8 | **Confirmed** (read/write on hardware); slot formula **Strong** |
| A11 | **`0x1FB000` identified as a per-unit battery discharge curve.** ndeadly's `memory_layout.md` lists this region as "Unknown". | Came from comparing multiple genuine units' dumps in-repo. | [`experiments/spi-dump-analysis-2026-07-10.md`](experiments/spi-dump-analysis-2026-07-10.md) §3.5 | **Confirmed** (multi-unit) |
| A8 | **Mouse-mode enable is feature bit 4 (`0x0C` mask `0x37`), refuting an earlier "command `0x13` = mouse" idea; relative report `0x07/08` deltas byte-verified.** | Resolved a wrong hypothesis and byte-verified against a decrypted BLE capture decoded in-repo. | `docs/switch2-joycon2/mouse-mode.md`; `docs/experiments/2026-07-19-usb-command-ab-diff.md` (Exp 2) | **Confirmed** (capture + hardware) |

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
- **NVM map verification (2026-07-29).** ndeadly's `memory_layout.md` checked against this project's
  own 2 MB dumps: firmware header magic `0xAA640001` + `" SYS"` at all three firmware offsets, the
  `0x60000` bank spacing, `DSPH` and the `MT3616A0 DSP` string, factory `VID 0x057E` /
  `PID 0x2069`/`0x2073` at `0x13012`/`0x13014`, and the `0x1FD010` post-firmware-update flag. That
  last one resolved why our two units differ — one had been firmware-updated and the other had not —
  and corrected a wrong first inference that the GameCube controller lacked a DSP blob because it
  lacks audio. Consolidated in [`switch2/controller-nvm-map.md`](switch2/controller-nvm-map.md).

## Tier C — Inherited foundation (credit where due)

The load-bearing majority of the raw protocol came from upstream and should be cited as such:

- **From ndeadly:** the command set `0x01–0x18` and subcommands, factory memory map (`0x13000`
  region), input/output report field layouts, feature-flag table (`0x0C`), the initial descriptor
  reference, the firmware/failsafe-bank structure and its header magic, and **Safe Mode** — the
  recovery interface documented in [`switch2/controller-safe-mode.md`](switch2/controller-safe-mode.md),
  which this project has neither reproduced nor needs. Mirrored at
  `nso-gc-refs/switch2_controller_research/`.
- **From Dycool:** confirmation that from-a-gadget emulation/relay of a genuine Pro2 is viable, and a
  reference point for report cadence and feature negotiation.

Where this repo re-states an upstream fact, it is expected to carry a confidence tag and, ideally, a
second source — see [`re-methodology/evidence-standards.md`](re-methodology/evidence-standards.md).

## The shared, still-unsolved frontier

Direct UART evidence has moved the motion boundary beyond the older static-capture state. A genuine
Pro Controller 2 now provides repeatable interleaved length-`0x1E`/`0x28` native motion, native
passthrough is hardware-validated, and DualSense translation uses the decoded length-`0x1E`
carrier. Those are current repository results, not claims inherited from a third-party static
capture.

The remaining frontier is narrower: exact carrier projection/rounding and coherent software
generation for controllers without
the source hardware. The length-`0x28` cadence-dependent accel/gyro/temperature layouts are decoded,
and the former G6/G7/G8 aliases are known to cross packed gyro/accel fields rather than identify
independent reference lanes. Reciprocal zero-drop chart captures directly establish the local
state-0/state-3 boundary projection and refute strict smallest-three as an exact genuine model.
A held-out zero-drop `3 → 1 → 0` gameplay capture further refutes composition into one global
unsigned permutation per state while validating the cyclic omitted-component paired-sign branch
across state 1. A reciprocal `3 → 2 → 3` capture then closes all four chart states under the same
stateful topology, with no per-edge tuning; its interleaved prefix selects the
pre-transition chart 3. A
controlled 2026-07-29 no-magnet/polarity/distance matrix found no resolved external-field response;
it rejects a simple raw magnetic-field interpretation without claiming that the genuine controller
lacks or never consumes an internal magnetometer. See
[`switch2/uart-magprobe.md`](switch2/uart-magprobe.md) and
[`experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md`](experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md),
and
[`experiments/pro2-carrier-chart-transition-2026-07-29.md`](experiments/pro2-carrier-chart-transition-2026-07-29.md).

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
