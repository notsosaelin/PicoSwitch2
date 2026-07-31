# Switch 2 controller Safe Mode

Status: 🔵 Partial — third-party evidence, not reproduced on this project's hardware.
Scope: genuine Nintendo Switch 2 controllers. PicoSwitch2 does **not** implement or emulate this.
Related: [`controller-nvm-map.md`](controller-nvm-map.md), [`usb-spec.md`](usb-spec.md),
[`firmware-versioning.md`](firmware-versioning.md).

## What it is

Every Switch 2 controller family has a **Safe Mode**: a recovery state entered by a button
combination that replaces the normal HID personality with a *vendor-specific USB interface*
carrying a simple byte-oriented command channel. It is Nintendo's own service/recovery path,
almost certainly the transport behind a failed-firmware-update recovery.

Two things are distinctive:

1. The controller enumerates under a **different product ID** and a different product string
   (`Nintendo Safe Mode Device`), so a host sees a genuinely different device, not a mode flag.
2. The interface is a **serial command channel**, not HID. Commands are single bytes; multiple
   bytes in one transfer are processed sequentially. An unrecognised command answers `15`.

Player LEDs 1 and 4 light while in Safe Mode. `SYNC` exits.

## Entry and identity

| Controller | Normal PID | Safe Mode PID | Key combination |
|---|---|---|---|
| Joy-Con 2 (R) | `0x2066` | `0x2070` | `ZR` + `PLUS` + `SYNC` |
| Joy-Con 2 (L) | `0x2067` | `0x2071` | `ZL` + `MINUS` + `SYNC` |
| Pro Controller 2 | `0x2069` | `0x2072` | `ZR` + `PLUS` + `SYNC` |
| NSO GameCube Controller | `0x2073` | `0x2074` | `Z` + `START` + `SYNC` |

Hold all three, then release them individually in the order listed.

Normal PIDs are this project's own confirmed values
(`src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.h`); Safe Mode PIDs are ndeadly's.

Two internal consistency checks support the Safe Mode table even though we have not reproduced it:

- The right/left ordering is preserved across both blocks — R before L in the normal pairs
  (`0x2066`/`0x2067`) and in the Safe Mode pairs (`0x2070`/`0x2071`).
- Safe Mode command `0x05` returns the device's *normal* product ID; the documented example
  response is `69 20`, little-endian `0x2069`, which is exactly this project's confirmed Pro
  Controller 2 PID.

Note that `0x2073` (NSO GameCube, normal) sits inside the `0x2070`–`0x2074` span, so the Safe Mode
identifiers are not a clean contiguous block. Do not infer a numbering rule from them.

## Command surface

| ID | Meaning | Example request → response | Confidence |
|---|---|---|---|
| `0x01` | Unknown; possibly a Safe Mode firmware version | `01` → `0D 01` | Hypothesis |
| `0x02` | Unknown | `02` → `01` | Unknown |
| `0x03` | Unknown | `03` → `06` | Unknown |
| `0x04` | Unknown | `04` → `06` | Unknown |
| `0x05` | Get the normal (non-Safe-Mode) product ID | `05` → `69 20` | Strong |
| `0xAA` | Unknown; read times out, so more data is expected | `AA` → (timeout) | Unknown |
| `0xAB` | Unknown; read times out, so more data is expected | `AB` → (timeout) | Unknown |
| other | Invalid | → `15` | Strong |

**These commands were found by fuzzing the output endpoint**, not from any documented interface.
That matters twice over: the "Usage" column is inference from observed responses, and the set is
not known to be complete.

`0xAA`/`0xAB` timing out on read is the interesting shape: a command that expects further argument
bytes before it answers is what a flash read/write or an update transfer looks like. That is
consistent with Safe Mode existing to recover a bad firmware update, but it is **unverified** and
this project has no reason to establish it.

## What this means for PicoSwitch2

Honestly: **little, directly.** The project emulates controllers toward a console; Safe Mode is a
host-facing service interface on genuine hardware. It changes nothing about how the console talks
to us. Recording it is still worthwhile for three reasons.

### 1. Identity-space hygiene ✅ actionable

`0x2070`–`0x2074` are now known to be occupied by Nintendo. Any future personality, probe, or test
identity this project invents must avoid them. Nothing currently does — the four emulated
personalities use `0x2066`, `0x2067`, `0x2069`, and `0x2073`.

### 2. A support/troubleshooting signature ✅ actionable

A genuine controller that has fallen into Safe Mode will not pair and will not enumerate as a
controller. `ZR`+`PLUS`+`SYNC` is not a hard combination to hit while fumbling a sync, and
`ZL`+`MINUS`+`SYNC` even less so. The recognisable signature is **player LEDs 1 and 4 lit** plus a
USB product string of `Nintendo Safe Mode Device`. The fix is to press `SYNC`.

If a user ever reports "my genuine Pro Controller 2 stopped being detected by the dongle", this is
worth ruling out before any protocol debugging.

### 3. A boundary worth stating explicitly ⛔ out of scope

Safe Mode plus the NVM map in [`controller-nvm-map.md`](controller-nvm-map.md) is, in principle, a
path to reading and writing a genuine controller's flash — including its firmware banks.

**This project will not go there, and neither should a contributor acting on this document.**

- PicoSwitch2's mission is emulation. Nothing in it requires modifying genuine hardware.
- The failsafe bank structure exists precisely because a bad write bricks the device — and it is
  not the safety net it looks like. Bank #2 stays erased until a controller firmware update has
  been applied, so a never-updated unit has exactly one populated firmware bank. Our own NSO
  GameCube dump is in that state; see
  [`controller-nvm-map.md`](controller-nvm-map.md).
- Every piece of factory data this project actually needs — calibration, colours, firmware version,
  identity — is already obtainable non-destructively over the normal Bluetooth/USB command surface,
  which is what `docs/switch2/command-surface.md` documents and what the firmware already uses.

Document the mechanism; do not build a writer.

## Where the two research efforts stand relative to each other

ndeadly's repository and this one overlap and each is ahead in places. Worth knowing so effort is
not duplicated:

| Area | Ahead | Detail |
|---|---|---|
| Safe Mode | ndeadly | Entirely absent from this project until now |
| Controller NVM firmware/update banks | ndeadly | Header magic, failsafe bank ranges, bank selector |
| NFC command surface | **PicoSwitch2** | See below |
| `0x1FB000` region | **PicoSwitch2** | Identified as a per-unit battery discharge curve; listed as "unknown" upstream |
| Figure-v3 / NTAG I2C 2K amiibo | **PicoSwitch2** | Read *and* write lifecycle implemented and hardware-validated |

On NFC specifically, the upstream table lists command `0x01` subcommands `0x01`–`0x15` with most
marked Unknown. This project has hardware-confirmed semantics for more of that surface, including
three subcommands the upstream table does not list at all:

| Sub | Upstream | This project |
|---|---|---|
| `0x03` | Unknown | Poll / start scan |
| `0x04` | Unknown | Stop |
| `0x05` | Get status | Get status (payload layout documented) |
| `0x06` | Read device | Begin read; full descriptor layout decoded |
| `0x08` | Write device | Commit mutable-data write |
| `0x14` | Write buffer | Stage buffer; three envelope families classified |
| `0x15` | Read buffer | Read buffer; chunk framing documented |
| `0x1E` | *not listed* | Sector-aware reuse read |
| `0x20` | *not listed* | Complete extended (Air Riders) operation |
| `0x21` | *not listed* | Execute staged device command |

See [`../Amiibo-v3.md`](../Amiibo-v3.md) and
[`../re-methodology/nfc-investigation-workflow.md`](../re-methodology/nfc-investigation-workflow.md).
Contributing this back upstream would be a genuinely useful, low-effort exercise.

## Remaining unknowns and suggested experiments

None of these are on this project's critical path. They are recorded so the question does not have
to be re-derived.

| Question | Smallest useful experiment | Risk |
|---|---|---|
| Are the Safe Mode PIDs correct for our units? | Enter Safe Mode on a genuine Pro2, read the USB descriptor, press `SYNC` to exit | Low — read-only, documented exit |
| Does `0x01` return a version? | Compare its response across two controllers on different firmware | Low |
| What do `0xAA`/`0xAB` expect? | Do **not** probe blindly. A command that accepts trailing bytes may write | **High — do not attempt** |
| Does Safe Mode survive a battery pull? | Observe, do not induce | Low |

The first one is the only one worth a maintainer's time, and only if a genuine controller is
already on the bench for another reason.

## Provenance

- Safe Mode behaviour, PIDs, key combinations, and command table: ndeadly, `safe_mode.md`,
  mirrored in this repository at `nso-gc-refs/switch2_controller_research/safe_mode.md` and
  re-checked against
  [upstream](https://github.com/ndeadly/switch2_controller_research/blob/master/safe_mode.md) on
  2026-07-29 (identical). Confidence **Strong** (specific, checkable, named source) — *not*
  Confirmed, because nothing here has been reproduced on this project's hardware.

  Process note: that mirror has been in this repository since 2026-07-15, so Safe Mode was
  available to read for two weeks before anyone looked. Mirroring a reference is not the same as
  having read it. When a question arises about genuine controller behaviour, check
  `nso-gc-refs/` before searching the web — as of this writing it also holds `commands.md`,
  `descriptors.md`, `hid_reports.md`, `bluetooth_interface.md`, `memory_layout.md`, plus `captures/`
  and `datasheets/`.
- Normal PIDs and all NFC comparisons: this project's own captures and hardware tests.
  Confidence **Confirmed**.

Per [`../re-methodology/evidence-standards.md`](../re-methodology/evidence-standards.md) and
`AGENTS.md`, third-party projects are not treated as definitive Switch 2 protocol truth. Nothing in
this document should be promoted to Confirmed without a capture in `dumps/`.
