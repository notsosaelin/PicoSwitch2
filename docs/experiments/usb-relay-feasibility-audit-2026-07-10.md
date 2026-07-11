# Implementation-Level Feasibility Audit: `Dycool/Usb-relay-for-NS` for Genuine Console-Side Report-0x09 Capture (2026-07-10)

**Status:** 🟡 Audit complete from primary-source code inspection. **Go, with a required fix path**
— not a hard no-go, but the relay as committed will very likely fail at Switch 2's device
classification step before any report-0x09 data is ever exchanged with a real console. A smaller,
lower-risk, immediately-actionable alternative (§7) exists that doesn't depend on fixing that at
all, and is recommended as the actual first step.

**Why this pass exists**: BLE 40-byte-block semantic decoding (`sw2-v2-motion-block-discovery-2026-07-10.md`)
has been paused — every passive-statistical approach available from that dataset has been tried
and none converged (see that report's pause banner). Per explicit direction, this project's actual
target — reproducing genuine console-side USB report 0x09 — needs a better observation point, not
another indirect stimulus against the same opaque BLE data. This audit determines whether
`Dycool/Usb-relay-for-NS` (previously assessed only at a summary level in
`docs/switch2/ble-controller-protocol-inventory.md` §6) is that better observation point.

**Method**: direct inspection of the repository's actual source files (`README.md`,
`setup_pro_gadget.sh`, `pi_pro_proxy.cpp`, `relay_protocol.hpp`, `win_pro_relay_capture.cpp`,
`CMakeLists.txt`) via `github.com/Dycool/Usb-relay-for-NS` (main branch, fetched this pass) —
not a re-statement of the earlier summary-level assessment. Quotes below are from the actual
repository content unless marked otherwise.

---

## 1. Complete intended topology and data path

```
genuine Pro Controller 2 (USB cable)
        |  hidapi (direct HID read/write)
        v
  Windows PC — win_pro_relay_capture.exe
        |  UDP (peer-discovery, no fixed IP required)
        v
  Raspberry Pi — pi_pro_proxy
        |  /dev/hidg0 (Linux configfs USB HID gadget, OTG mode)
        v
  Nintendo Switch 2 console (USB cable, console is USB host)
```

Bidirectional: controller→console traffic (`ControllerToSwitch`, carries report 0x09 motion) flows
Windows→UDP→Pi→`/dev/hidg0`; console→controller traffic (`SwitchToController`, LED/rumble/feature
commands) flows the reverse path, `/dev/hidg0`→Pi→UDP→Windows→genuine controller via hidapi.

## 2. Required hardware, USB roles, gadget capabilities, Windows components, networking, connection mode

- **Raspberry Pi**: any model with USB OTG/gadget-mode capability — `libcomposite`/`configfs`
  support is required (`setup_pro_gadget.sh` explicitly does `modprobe libcomposite` and mounts
  `configfs`). A Pi Zero/Zero 2 W (native OTG on the USB data port) or a Pi 4B (USB-C port can run
  in device/gadget mode) both qualify. **No specialized USB analyzer hardware** (no Cynthion,
  Packetry, FaceDancer) is needed for this approach — a meaningfully lower hardware bar than the
  method that originally produced this project's only existing report-0x09 evidence.
- **USB roles**: Pi = USB *device* (gadget) to the console; console = USB *host*. Windows PC = USB
  *host* to the genuine controller (via hidapi, standard HID device access, no special driver
  beyond hidapi itself).
- **Gadget capability required**: a single HID function (`hid.usb0`) presenting VID `0x057e`, PID
  `0x2069`, `bcdDevice 0x0210` (all confirmed matching this project's own established genuine
  identity), with a **generic, hand-authored HID report descriptor is used, not a byte-exact copy
  of the real device's descriptor** — see §4 for why this matters.
- **Windows components**: `hidapi` library (linked via CMake `find_package`), Winsock2 (`ws2_32`)
  for UDP. No driver installation beyond whatever hidapi itself requires for HID access (typically
  none beyond what Windows already provides for HID-class devices).
- **Networking**: plain UDP between Pi and Windows PC. The protocol includes a `Hello` message
  type for peer discovery — **exact network topology requirements (same LAN? same subnet? Wi-Fi
  vs. wired?) are not stated in the repo and are unverified** — flagged as needing confirmation
  during setup, not a known blocker.
- **Controller connection mode**: the genuine Pro Controller 2 must be connected via **USB cable**
  directly to the Windows PC (hidapi opens it as a USB HID device) — **not BLE**. This is a
  meaningful, easily-overlooked prerequisite: it requires physical USB access to the genuine
  controller, separate from (and simpler than) this project's own BLE-based capture work.

## 3. What remains unimplemented or unverified in the repository

- **No FunctionFS / low-level USB device implementation** — confirmed by an explicit in-code
  comment: `// - This forwards HID reports. It does not implement a lower-level FunctionFS USB
  device.` No code anywhere in `pi_pro_proxy.cpp` references FunctionFS, `ep0`, control transfers,
  SETUP packets, USB descriptors, or `/dev/bus/usb`. The Pi side operates *exclusively* at the
  HID-report level via the kernel's own generic gadget HID function.
- **No committed sample captures** exist in the repository.
- **No decode script** is present despite being referenced in this project's earlier summary-level
  notes (`decode_relay_capture.py`) — confirmed absent from the actual file listing this pass.
- **The Raspberry Pi side is not part of the CMake build** — `CMakeLists.txt` only builds
  `win_pro_relay_capture` (Windows-only, gated `if(WIN32)`). `pi_pro_proxy.cpp` must be compiled
  manually on the Pi via the documented `g++ -O2 -std=c++17 -pthread -o pi_pro_proxy
  pi_pro_proxy.cpp` command — a real but minor setup-procedure gap, not a functional defect.
- **Two of the five USB init commands are unverified against this project's own documentation**:
  `USB_INIT_COMMAND` (`03 91 00 0D 00 08 00 00 01 00 FF FF FF FF FF FF`) and
  `USB_SELECT_COMMON_REPORT_COMMAND` (`03 91 00 0A 00 04 00 00 05 00 00 00`) use command ID `0x03`
  with subcommands `0x0D`/`0x0A` that don't appear anywhere in this project's own documented USB
  command set (`report-0x09-motion.md`, `switch_pro2.c`). Not necessarily wrong — this project's
  own documentation has never needed to characterize the *full* init handshake, only the
  motion-relevant part — but genuinely unverified, worth checking against `switch_pro2.c`'s own
  working init sequence before assuming these are correct.
- **`WaitForInitReply()` waits up to 1000ms for HID report ID `0x05`** as the init-acknowledgment
  signal — not cross-checked against this project's own report-ID conventions this pass.

## 4. Does it preserve control, bulk, interrupt, and vendor-specific transactions byte-for-byte?

**Partially, and the gap is the single most important finding of this audit.**

- **Interrupt-endpoint HID report payload**: ✅ relayed byte-for-byte at the application layer.
  Both `Switch2Protocol::ProcessSwitchPacket()` and `ProcessControllerPacket()` are confirmed pure
  `memcpy` pass-through with **no translation applied** — the in-code `// TODO: Switch 2 protocol
  translation if needed` comment is present but the pass-through path is what actually executes
  (the TODO was never acted on, which for this project's purposes is *correct* — no translation is
  wanted, raw genuine bytes are).
- **Report length/padding**: 🟡 **potential fidelity issue, unverified against real report
  sizes.** `pi_pro_proxy.cpp` zero-pads every outgoing report to a fixed `NSPR_MAX_PAYLOAD = 128`
  bytes before writing to `/dev/hidg0` (confirmed: `uint8_t out[NSPR_MAX_PAYLOAD]{};
  std::memcpy(out, p.payload, p.len); write(g_hid, out, sizeof(out));` — always writes the full
  128-byte buffer regardless of the original report's actual length). The gadget's own
  `report_length` is also hard-coded to 128 (`setup_pro_gadget.sh`: `echo 128 >
  functions/hid.usb0/report_length`). **Whether the genuine Switch 2 Pro Controller's actual USB
  interrupt report size is exactly 128 bytes is not verified in this pass** — if it differs, this
  padding could alter wire-level packet length even though payload *content* up to the original
  length is preserved untouched.
- **Control transfers (EP0) and vendor-specific handshake**: ❌ **not relayed at all.** The Linux
  kernel's generic `configfs`/`libcomposite` HID gadget framework answers all EP0 control requests
  (`GET_DESCRIPTOR`, class-specific HID requests, and any vendor-specific control transfers the
  real console might issue) using its own **static, kernel-generated responses** based on the
  descriptors `setup_pro_gadget.sh` configures — it does **not** forward EP0 traffic to the genuine
  controller or replay the genuine controller's own actual EP0 responses. This project's own
  established, hard-won knowledge (`STATUS.md`: *"EP0 vendor handshake byte-exact vs a real PC2;
  console accepts it"*) is that the Switch 2 console performs a **vendor-specific EP0 handshake**
  as part of device classification, and this project's own working PicoSwitch2 emulation needed to
  replicate that handshake byte-exact to be accepted. **The Dycool gadget, as configured, has no
  mechanism to do this** — it can only ever present whatever *static* descriptor responses were
  written into `setup_pro_gadget.sh`, never a live, correct vendor handshake.
- **Bulk transfers**: not applicable — HID-class devices of this kind don't typically use bulk
  endpoints; not investigated further since no evidence in the code suggests any are used.

## 5. Can it capture both directions with timestamps, without changing content/timing materially?

- **Timestamps**: ✅ present and reasonably good, but asymmetric in quality by direction. The
  Windows-side capture file (`CaptureRecord`, `NPRCAP03` format) timestamps every record with
  `QueryPerformanceCounter` (`qpc`), a high-resolution Windows timer, alongside a sequence number
  and direction flag. Critically: **the `ControllerToSwitch` direction — the one that matters for
  report 0x09 — is timestamped essentially at the point of direct `hidapi` read from the genuine
  controller**, *before* any UDP relay hop, meaning its timing fidelity relative to the genuine
  controller's own output rate is good and not meaningfully corrupted by the relay's later stages.
  The `SwitchToController` direction (console commands, LED/rumble/feature-enable) is only
  observed *after* traversing Pi→UDP→Windows, so its recorded timing reflects UDP arrival, not the
  console's actual USB-bus transaction time — real but bounded network jitter is introduced there.
- **Content**: preserved byte-for-byte at the payload level (§4), modulo the unverified padding
  question.
- **The `RelayPacket` UDP wire format itself carries no timestamp** (`relay_protocol.hpp`: fields
  are `magic, type, reserved, len, seq, payload[128]` — no time field) — any timing information
  only exists because the Windows-side capture tool separately stamps records as it writes them,
  not because the protocol was designed to preserve bus-level timing end-to-end.

## 6. Can it relay PID `0x2069` through the Switch 2's classification and vendor-handshake sequence?

**Likely not, as currently implemented — this is the audit's central concrete concern.** Per §4,
the gadget presents only static descriptors and generic kernel-driven EP0 responses. If the
console's classification logic depends on the vendor handshake this project's own emulation work
had to replicate byte-exact (which the evidence strongly suggests it does), the Dycool gadget will
most likely either fail to enumerate as an accepted Pro Controller 2, or enumerate as *some* HID
device but never receive the command traffic a genuinely-classified controller would — meaning the
whole relay chain downstream of that point (including the feature-enable commands correctly
implemented in §8) would never actually run against a real console session. **This is exactly the
risk the repository's own code comment already flags**: *"If the Switch classification issue is
inside configfs/UDC control behavior, this v0 may still fail."* This audit adds a specific,
evidence-backed reason *why* that's likely (the missing EP0 vendor-handshake relay), not just a
restatement of the repo's own uncertainty.

## 7. Hard blocker, and the recommended smallest isolated path

**The hard blocker, if it materializes, is at USB enumeration/classification — before any
report-0x09 traffic would ever reach a real console.** Confirming or refuting this requires
plugging real hardware together (§9, Phase 1) — it cannot be fully resolved by further code
reading alone.

**However, a materially smaller, lower-risk, immediately-actionable alternative exists that
sidesteps this blocker entirely, and is the recommended first step, not the full relay:**

Run `win_pro_relay_capture.cpp`'s existing `hidapi` + `SendSwitch2Init()` + capture-file logic
**directly against the genuine controller connected via USB to a Windows PC — no Raspberry Pi, no
console, no relay involved at all.** This is not new code to write; the relevant logic (open by
VID/PID, send the confirmed-correct feature-enable command pair, capture with real timestamps) is
already present in that one file and could be extracted or run largely as-is (the UDP relay
threads would simply have nothing to talk to and can be left idle/disabled).

**What this tests, cheaply**: whether report-0x09 motion data becomes observable directly from the
genuine controller once the documented feature-enable sequence is sent, entirely independent of
whether the console-relay classification problem can ever be solved. Two outcomes, both valuable:

- **Motion data appears** → a genuine, directly-captured, byte-exact report-0x09 stream, usable
  for real semantic decoding — the actual evidence gap this whole investigation has lacked from
  the start — obtained without needing the harder Pi/console relay to work at all.
- **Motion data does not appear** → suggests the genuine controller requires the *console's own*
  ongoing session context (not just the documented enable command) before it streams motion, which
  is itself an important, previously-unknown constraint on any future relay or emulation approach.

This satisfies the task's explicit request for "the smallest isolated path to make it operational"
better than attempting to fix the full three-node relay first, since it requires only a Windows PC
and the genuine controller (hardware already partially available to this project), defers the
Pi/gadget/classification risk to a later phase, and — via the command-byte cross-check in §8 —
is already known to be sending the *correct* feature-enable bytes.

---

## 8. Cross-validation: the relay's feature-enable commands match this project's own documentation

`SendSwitch2Init()`'s third and fourth commands:

```
USB_SET_FEATURE_MASK_COMMAND: 0C 91 00 02 00 04 00 00 27 00 00 00
USB_ENABLE_FEATURES_COMMAND:  0C 91 00 04 00 04 00 00 27 00 00 00
```

match this project's own independently-documented, captured-and-confirmed USB feature-enable
command **exactly**, byte for byte, including the `0x27` mask (`report-0x09-motion.md` line 83:
`0c 91 00 04 00 04 00 00 27 00 00 00 # feature mask 0x27 (IMU bit set, no magnetometer) -> motion
ON`). This is a genuine positive finding: **whatever else is wrong with the relay, its motion-enable
step is very unlikely to be the problem** — it is either independently correct or sourced from the
same underlying evidence this project already trusts, either way reducing risk in that specific
part of the chain. (It does not by itself resolve whether commands #1/#5, using the unverified
`0x03` command family, are correct — see §3.)

---

## 9. Go/no-go report

### Confirmed working components
- Feature-enable command bytes (`0x0C` configure/enable, mask `0x27`) — cross-validated exactly
  against this project's own documentation (§8).
- HID payload relay logic is genuine byte-for-byte pass-through, no unwanted translation (§4).
- Gadget identity fields (VID/PID/bcdDevice) match this project's own confirmed genuine values.
- Bidirectional data flow is structurally present (both directions implemented, not a one-way
  stub).
- Windows-side capture format includes real, reasonably precise timestamps for the critical
  `ControllerToSwitch` direction (§5).

### Unverified components
- Whether the Pi gadget will pass Switch 2's USB classification at all (§6) — the single largest
  open question, resolvable only with real hardware.
- Command family `0x03` (subcommands `0x0D`, `0x0A`) used in init steps #1 and #5 — not
  cross-checked against this project's own working init sequence.
- Whether 128 bytes matches the genuine controller's actual USB report size (§4) — padding could
  be silently altering wire-level length.
- Exact network topology requirements for the Pi↔Windows UDP link (§2).

### Concrete defects
- **No EP0/control-transfer relay of any kind** — the single most important defect, directly
  threatens the console-classification step (§4, §6).
- Fixed 128-byte zero-padding regardless of actual report length (§4).
- No timestamp in the UDP wire protocol itself — only added downstream on the Windows side (§5).
- Pi-side binary isn't part of the repo's build system (§3) — a process gap, not a code defect.

### Required hardware
A Raspberry Pi with USB OTG/gadget capability (Pi Zero/Zero 2 W or a Pi 4B in USB-C device mode);
a Windows PC; a genuine Pro Controller 2 connectable via USB cable; a Switch 2 console (only
needed from Phase 1 onward — **not needed at all** for the recommended first step, §7).

### Exact setup procedure (as documented in the repository, unverified end-to-end by this audit)
1. On the Pi: `sudo bash setup_pro_gadget.sh [UDC_NAME]` (auto-detects the UDC if omitted).
2. On the Pi: `g++ -O2 -std=c++17 -pthread -o pi_pro_proxy pi_pro_proxy.cpp`, then
   `sudo ./pi_pro_proxy --hid /dev/hidg0 --port 7441`.
3. On Windows: build `win_pro_relay_capture` via CMake (requires `hidapi` discoverable by
   `find_package`), run it with the genuine controller connected via USB.
4. Connect the Pi to the Switch 2 console via USB (Pi acts as device/gadget; console is host).
5. **Stop `ns-backend`/`ns-backend-pro` first** if applicable (explicit note in `pi_pro_proxy.cpp`'s
   own header comment — likely a reference to some other Switch-relay-adjacent background service,
   unverified what specifically this refers to on a stock Raspberry Pi OS install).

### Minimal fixes required (if pursuing the full relay, ranked by leverage)
1. **Determine and, if needed, implement genuine EP0 vendor-handshake relay** — the highest-risk,
   highest-effort item, and the one most likely to actually block console acceptance. Options
   range from "descriptors alone turn out to be sufficient" (best case, no fix needed) to "requires
   a FunctionFS-based gadget replacing the current `configfs` HID function entirely" (substantial
   rewrite). **Cannot be sized precisely without Phase 1 hardware testing (§9's ladder).**
2. Verify genuine report length and remove/correct the fixed 128-byte padding if it's wrong.
3. Verify the `0x03`-family init commands against `switch_pro2.c`'s own working sequence.
4. Add a timestamp field to the UDP wire protocol itself, or at minimum log Pi-side receive/send
   times, to properly characterize relay-introduced latency for the `SwitchToController` direction.

### Validation ladder (enumeration and buttons before motion)

| Phase | Test | Requires | Validates |
|---|---|---|---|
| **0** | Windows-only `hidapi` capture with feature-enable, no Pi/console (§7) | Windows PC + genuine controller | Whether motion streams from a direct PC connection once documented commands are sent — **recommended actual first step** |
| **1** | Pi gadget enumeration alone (dummy/static HID payload, no Windows/genuine-controller relay yet) | Pi + console | Whether the console accepts/classifies the gadget as a Pro Controller 2 at all — resolves §6's central open question cheaply, without needing the full chain running |
| **2** | Full 3-node relay, buttons only | Pi + Windows + genuine controller + console | Whether basic button presses on the genuine controller are correctly observed by the console through the complete relay chain |
| **3** | Full 3-node relay, motion capture during real gameplay | Same, plus a motion-capable game/context | Genuine report-0x09 bytes, captured with real console-driven session context — the actual evidence this whole investigation needs |

Phase 1 and Phase 0 can run in parallel (independent hardware requirements) and should both
complete before investing effort in Phase 2/3.

### First controlled capture matrix (once relay fidelity is established — Phase 3 only)

Per the task's specification, for whenever Phase 2 succeeds and Phase 3 becomes viable:
stationary flat; supported fixed tilt; isolated positive/negative pitch; isolated positive/negative
yaw; isolated positive/negative roll; known-angle rotations; feature enable/disable transitions;
connection and motion initialization. **Not analyzed or further designed this pass** — per explicit
instruction, the priority is determining whether the relay can obtain such captures at all, not
planning their analysis in advance of having any.

---

## 10. Explicit non-claims

Per this task's constraints: no changes were made to `report 0x09`, `ns2_build_report()`,
`ns2_motion_tick()`, any filter, or any BLE initialization code. The BLE 40-byte block was not
reinterpreted or re-analyzed. No claim is made that BLE and USB motion formats are equivalent —
this audit is entirely about a *separate, USB-only* observation path, orthogonal to the paused BLE
work. No hardware test was performed or requested to be performed as part of this audit — it is a
code-level feasibility assessment only, ending with a recommended next step (§7/§9 Phase 0) rather
than a request to run it immediately.
