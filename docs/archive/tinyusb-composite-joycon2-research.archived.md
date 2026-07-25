# TinyUSB Composite Devices, Multi-HID, and the Two-Joy-Con Question (Archived)

> Research into what TinyUSB's composite-device support actually enables, whether it can present
> **two Joy-Con 2 identities from one Pico**, and where the real boundary lies. Prompted by a fair
> correction: *TinyUSB does support combining multiple functions into one USB device* — true, and
> this repo already relies on it. This document draws the precise line between what composite solves
> and what it cannot, and lays out the paths that could.
>
> **Documentation only — no code changed.** Confidence: **Confirmed** (verified in TinyUSB, the
> RP2040/RP2350 datasheet, and this repo's own descriptors/handlers) · **Strong Evidence** ·
> **Hypothesis** · **Unknown**.

## 0. Summary up front

- **The correction is right, and then some: TinyUSB composite works, and this repo already uses it.**
  The generic Pro path declares up to **four HID interfaces on one USB device**
  (`SWITCH_PRO_MAX_CONTROLLERS = 4`, `bNumInterfaces = 4`, `CFG_TUD_HID = 4`), and the config/audio
  paths combine CDC/vendor/UAC via IAD. Composite is not the blocker. (Confirmed.)
- **But "composite" and "two Joy-Cons" are different problems.** A **composite device** = **one
  device, one address, one identity, many interfaces**. Two Joy-Cons = **two devices, two addresses,
  two identities** — that is a **hub**, not a composite. (Confirmed.)
- **Two hard reasons composite can't be two *native* Joy-Con 2s:**
  1. **One device descriptor = one VID/PID.** JC2 Left is PID **`0x2067`**, Right is **`0x2066`** —
     distinct devices. A composite device has a single PID; it cannot *be* both.
  2. **One control endpoint (EP0) = one vendor identity handshake.** The Switch 2 classifies each
     native controller via an **EP0 vendor handshake** (`bRequest 0x02/0x03/0x04`) that returns
     **one** 64-byte identity block **per device**. The genuine Charging Grip proves the console does
     this **per device** (it is a real hub with two independently-addressed children). One composite
     device answers with one identity → the console sees **one** controller. (Confirmed from
     `switch_joycon2.c:688-714` + the grip capture.)
- **The real limit is a hardware one, not a TinyUSB-composite one:** two addresses need a hub, and
  the RP2040/RP2350 device controller holds **one** bus address (`ADDR_ENDP.ADDRESS`); TinyUSB's
  device stack has **no hub class**. (Confirmed.)
- **One open hypothesis remains testable** (§6) and two real hub paths exist (§7).

## 1. Composite device vs hub — the distinction that matters

```
COMPOSITE DEVICE (what TinyUSB does, what this repo uses)
  one USB address ──┬── Interface 0 (HID gamepad)
                    ├── Interface 1 (HID gamepad)      ← many INTERFACES
                    ├── Interface 2 (CDC / vendor / audio)
                    └── shared EP0 → ONE device descriptor, ONE VID/PID, ONE identity

HUB (what the genuine Joy-Con 2 Charging Grip is)
  hub (addr 1) ──┬── Joy-Con L  (addr 2, PID 0x2067, its own EP0 identity)   ← many DEVICES
                 └── Joy-Con R  (addr 3, PID 0x2066, its own EP0 identity)
```

A composite device multiplies **interfaces** behind one identity. A hub multiplies **devices**, each
with its own identity. The Switch 2 pairs *two Joy-Cons* — two identities — which is the hub shape.

## 2. What TinyUSB composite is — and that this repo already uses it (Confirmed)

TinyUSB fully supports composite devices:
- **Multiple HID interfaces:** `CFG_TUD_HID` = number of HID instances. This repo sets it to
  `SWITCH_PRO_MAX_CONTROLLERS` (=4) and emits four HID interface blocks — *"one interface = one Pro
  Controller = one player on the console"* (`usb_descriptors.c:160-201`). Each interface gets its own
  IN+OUT interrupt endpoint pair (`0x81/0x01 … 0x84/0x04`).
- **Report-ID multiplexing:** alternatively, one HID interface can carry many report types by report
  ID (TinyUSB's `hid_composite` example does keyboard+mouse+gamepad+… on one interface).
- **Mixed-class composite via IAD:** the config-mode device is CDC + interfaces
  (`bDeviceClass 0xEF / 0x02 / 0x01` = Misc/IAD, `usb_descriptors.c:231,220`), and the Pro
  Controller 2 audio personality combines HID + vendor + UAC1 audio the same way.
- **Endpoint budget:** RP2040/RP2350 provide **16 USB endpoints** and 4 KB DPRAM — ample for 4 HID
  interfaces (8 endpoints) + EP0. Not the constraint.

So the "four controllers from one Pico" capability the project was built on **is** TinyUSB composite
multi-HID. The user's premise is correct.

## 3. Why composite ≠ two native Joy-Con 2 identities (Confirmed)

The multi-HID trick works for **generic HID gamepads** because a host distinguishes those players by
**interface** — no per-device identity is involved. Native Switch 2 controllers are different: the
console identifies each by **device identity**, established two ways a composite device cannot
multiply:

### 3.1 One device descriptor → one PID
Each Joy-Con is a distinct USB **device** with its own product ID (Left `0x2067`, Right `0x2066`;
`switch2_ble.c:25-26`, mirrored in the USB personalities). A composite device has exactly **one**
device descriptor and therefore **one** VID/PID. It cannot simultaneously be PID `0x2067` *and*
`0x2066`. There is no "combined pair" PID — the genuine grip does not define one; it just exposes two
real devices (`docs/switch2-joycon2/protocol.md` "Why not simultaneous L+R").

### 3.2 One EP0 → one identity handshake
The console's native-controller classification is the **EP0 vendor handshake** (`bRequest 0x03` →
64-byte identity, `0x02` → info, `0x04` → ACK). In this repo's handler
(`switch_joycon2.c:688-714`) that request is answered **device-wide with a single identity block**;
it is *not* selected per interface (only the MS-OS descriptor requests branch on `wIndex`). A
composite device has **one** EP0, so it can complete **one** identity handshake and present **one**
native identity. The console, doing this handshake **per device** (as the hub-based grip requires),
sees one controller.

**Net:** multi-HID gives you many *interfaces under one identity*; two Joy-Cons need *two
identities*. Composite structurally cannot supply the second identity.

## 4. What the genuine hardware confirms

The project's own live USB capture of the real Joy-Con 2 Charging Grip: a **standards-compliant USB
hub** (`bDeviceClass 0x09`, zero vendor interfaces) with **two independently-addressed children**
(Joy-Con L and R), each running its own EP0 identity handshake. There is no "combined" wire
identity to emulate — the hardware's answer to "two Joy-Cons, one cable" is **a hub with two real
devices** (`docs/switch2-joycon2/protocol.md`; `docs/provenance.md` A1 — the EP0-handshake
acceptance criterion).

## 5. The actual limit: two addresses need a hub (hardware) (Confirmed)

- **RP2040/RP2350 hold one device address.** The USB device controller has a single
  `ADDR_ENDP.ADDRESS` field; the SIE matches incoming tokens against that one value. There is no
  second address to answer as a second device.
- **TinyUSB's device stack has no hub class.** Hub support exists only in the **host** stack. There
  is no device-mode "be a hub with children" path in TinyUSB.
- Therefore emulating the grip's three-device topology (hub + L + R) from one Pico's native USB
  controller is **not possible** with TinyUSB + the RP2040/RP2350 device peripheral. This is the real
  blocker — a **hardware/stack** limit, distinct from (and not solved by) composite support.

## 6. The one open hypothesis (testable now)

**Could a composite device return *different* identities per interface if the console asked per
interface?** The EP0 vendor handshake *could* in principle be **interface-recipient**
(`wIndex = interface number`), which would let one device answer two identities. Evidence says no —
the grip is a hub, and this repo's handler is device-level — but it has **not** been proven that the
console *never* issues an interface-targeted identity request.

- **Test (cheap, now possible):** the on-device **protocol tracer is implemented**
  (`ns2_protocol_trace_record`, `switch_joycon2.c:692`). Log the `bmRequestType`/`wIndex` of every
  EP0 vendor request from a real console during native enumeration. If every identity request is
  **device-recipient with `wIndex = 0`**, the hypothesis is dead — composite cannot help, full stop.
  If any identity request is interface-targeted, a composite two-interface device *might* present two
  identities (then fault-inject two identity blocks and watch the console). **Expected outcome:
  device-recipient → composite ruled out.** Confidence: **Hypothesis**, low probability, but a
  30-minute capture settles it definitively.

## 7. Paths that *do* provide two addresses

| Path | How | Verdict |
|---|---|---|
| **External USB hub + two Picos** | One Pico flashed JOYCON2_L, one JOYCON2_R, both behind an off-the-shelf hub → the console sees a valid L and R and offers to pair them (host-side merge). Composite not involved. | ✅ **Works today** (topology matches the genuine grip). One controller can drive both halves if its inputs are split across the two dongles. |
| **PIO-USB device-mode hub** | Bit-bang USB in PIO (RP2040/RP2350) with a stack that implements hub-class + multi-address device behaviour. `Pico-PIO-USB` exists but its device mode does not implement hub/multi-address; TinyUSB's PIO-USB integration is host-mode. | 🔬 **Unproven research** — no known implementation; genuine, uncertain-odds work. |
| **Second USB controller** | RP2350 exposes a single native USB device controller; there is no second bus to answer as a second address. | ❌ Not available. |

The **external hub + two Picos** path is the practical answer for a true Joy-Con 2 *pair*, and it
sidesteps this entire question because each dongle is a normal single-identity composite device.

## 8. TinyUSB implementation reference (for the multi-HID that *does* apply)

For presenting multiple **generic** HID controllers (the mode this repo already ships), the levers:

- `CFG_TUD_HID = N` in `tusb_config.h` → N HID class instances (`include/tusb_config.h:106-108`).
- Config descriptor: `bNumInterfaces = N`, then N × (Interface + HID + IN EP + OUT EP) blocks with
  distinct interface numbers and endpoint addresses (`usb_descriptors.c` `SWITCH_PRO_HID_INTERFACE`).
- Per-instance callbacks: `tud_hid_descriptor_report_cb(instance)`,
  `tud_hid_report(instance, …)` / `tud_hid_n_report(instance, …)`.
- Endpoint budget (RP2040/RP2350): 16 endpoints, so up to ~7 HID IN+OUT pairs alongside EP0 — the
  4-controller cap here is a project choice, not a hardware ceiling.
- For mixed classes (CDC/vendor/audio + HID), use **IAD** (`bDeviceClass 0xEF/0x02/0x01`) as the
  config-mode and audio personalities already do.

None of this changes the §3 conclusion for *native* controllers — it is the toolkit for the generic
multi-controller case, which is identity-by-interface, not identity-by-device.

## 9. Verdict & recommended next steps

- **Composite is not the limiter, and it is already exploited** to its useful extent here. The
  earlier "TinyUSB/composite" framing was imprecise: the blocker for a native Joy-Con 2 *pair* is the
  need for **two device identities** (two PIDs + two EP0 handshakes) → a **hub** → **two USB
  addresses**, which the RP2040/RP2350 device controller and TinyUSB device stack cannot provide.
- **Highest-value next step:** run the §6 tracer capture during genuine native enumeration to record
  whether identity requests are device- or interface-recipient. It either **kills** the composite
  hypothesis for good (device-recipient — most likely) or **opens** a genuinely novel single-device
  two-Joy-Con path (interface-recipient). Either way it converts a long-standing assumption into a
  measured fact — and the tracer to do it already exists.
- **For a working pair now:** external hub + two Picos (§7).

## 10. References

- TinyUSB composite/HID: <https://docs.tinyusb.org/en/latest/examples/device/hid_composite.html> ·
  HID device class `CFG_TUD_HID`: <https://github.com/hathach/tinyusb/blob/master/src/class/hid/hid_device.h> ·
  USB concepts (device/config/interface model): <https://docs.tinyusb.org/en/latest/reference/usb_concepts.html>
- RP2040 USB controller (16 endpoints, single address), TinyUSB port:
  <https://deepwiki.com/hathach/tinyusb/4.3-rp2040-usb-controller>
- Pico-PIO-USB (PIO-based USB host/device, the only multi-address research avenue):
  <https://github.com/sekigon-gonnoc/Pico-PIO-USB>
- This repo: `src/usb_descriptors.c:160-231` (4-HID composite + IAD),
  `include/tusb_config.h:106-134` (`CFG_TUD_HID`, CDC, endpoints),
  `src/switch_joycon2/switch_joycon2.c:688-714` (device-level EP0 identity handshake),
  `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c:25-26` (JC2 L/R PIDs),
  `docs/switch2-joycon2/protocol.md` "Why not simultaneous L+R", `docs/provenance.md` (A1: the
  EP0-handshake acceptance criterion). DS5Dongle (vendored, single-controller composite reference):
  `nso-gc-refs/DS5Dongle/src/tusb_config.h`.
