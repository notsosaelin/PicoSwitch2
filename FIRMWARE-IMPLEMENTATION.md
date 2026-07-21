# Controller Firmware Versioning — Implementation & Analysis

> Comprehensive breakdown of how the Switch 2 decides a Pro Controller 2 needs a firmware update, how
> PicoSwitch2 answers the relevant queries, **how the exact current retail tuple eliminated the
> "controller update available" prompt**, whether the update image can be *read/captured* or
> *applied*, and how the reported version + firmware-region data are maintained.
>
> This is the durable reference behind the resolved controller-update compatibility work and
> `usb-spec.md` §"Firmware-version compatibility".
>
> Evidence base in this repo: `src/switch_pro2/switch_pro2.c`, `docs/switch2/usb-spec.md`,
> `nso-gc-refs/switch2_controller_research/{commands,memory_layout,hid_reports}.md` (ndeadly), and
> **one genuine 2 MB PC2 SPI image stored under two byte-identical filenames**
> (`dumps/SPI/2069_spi_dump_*.bin`) that this revision reads directly. Confidence tags:
> **Confirmed** (byte-verified here), **Strong Evidence** (external
> capture, consistent), **Hypothesis**, **Unknown**.

## 0. The honest reality up front

1. **We only ever report a version and serve memory bytes; we never run Nintendo firmware.** The
   dongle is a RP2040/RP2350 emulating a controller. The genuine update image targets Nintendo's
   controller MCU and **physically cannot be flashed onto the Pico** (§6). "Firmware support" here
   means: answer the console's version/memory queries convincingly, and — optionally, as research —
   *capture* the encrypted update stream for offline analysis (§7).
2. **The prompt is suppressed by the exact current retail tuple, not an arbitrary high value.** A
   live genuine-controller query returned `2.1.4 / 12.0.0 / 0.2.3`; applying that tuple to both
   version surfaces made Settings → Update Controllers report all known controllers up to date.
   `255.255.255` still prompted, disproving a simple numeric minimum comparison (§3).
3. **An actual update attempt fails because we do not implement the `0x0D` transport** (§5). Even if
   the console starts an update, we bare-ACK the transfer instead of running the multi-subcommand
   protocol, so it aborts. That is *by design* — we never accept Nintendo firmware writes. The
   controller keeps working regardless; the nag is cosmetic.
4. **The reported version is maintained in-app by the maintainer**, not user-configurable (§6). A
   wrong/invalid version could plausibly break enumeration or trigger worse update behavior, so the
   version stays a compile-time constant updated in the app.

### Implemented and hardware-validated — 2026-07-21

The first deliberately narrow experiment is now implemented:

- `include/ns2_firmware_profile.h` and `src/ns2_firmware_profile.c` are the single source for the
  controller, Bluetooth and DSP triplets. EP0 request `0x02` and command `0x10/01` are constructed
  from that same profile, so they cannot drift apart.
- The default is the hardware-queried and console-validated tuple **2.1.4 / 12.0.0 / 0.2.3**.
- Maintainers can select an exact expected tuple at configure time with
  `NS2_PRO_FIRMWARE_VERSION`, `NS2_PRO_BLUETOOTH_VERSION`, and `NS2_PRO_DSP_VERSION`. `build.ps1`
  passes all three explicitly so stale CMake cache values cannot leak into a normal build.
- The observed post-update bytes at `0x1FD010-0x1FD013` are served as zero. This is controlled by
  `NS2_PRO_UPDATED_STATE`. **Hardware result:** the prompt still appeared, proving this state alone
  is not sufficient to suppress it.
- No firmware-image headers, encrypted images, DSP blob, flash writes, or update transport are
  fabricated. The console trace proved none of those regions were read before the prompt decision.
- A compact read-only diagnostic records how often the two version surfaces are queried and the
  unique command-`0x02` flash address/length pairs requested. It stores no returned memory bytes and
  no command-`0x0D` update payload. The dedicated GP0/GP1 UART channel exposes that trace while USB-C
  remains attached to the console: run `tools/read_uart_diag.ps1 -Port COMx -Command fwreads` after
  reproducing the prompt. Config mode retains the same formatter for PC-only inspection, but is not
  the console-live transport.
- The same UART channel has a RAM-only `profile` override plus soft USB re-enumeration for rapid A/B
  tests, and a `btversion` bridge that sends read-only command `0x10/01` to a genuine Switch 2
  controller paired upstream over BLE. These are maintainer diagnostics, not persisted user config.

## 1. Where firmware version is reported — the surfaces the console reads

The console can gather controller-version information from **several** places during bring-up. The
first two are confirmed version strings; memory reads are a candidate validation surface (§3).

### Surface A — EP0 vendor control request `0x02` (enumeration, 16 bytes) — **Confirmed**

Read over endpoint 0 during USB enumeration, before the bulk command channel
(`switch_pro2.c:1098`). `ns2_ctrl_info` (`switch_pro2.c:228-230`):

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | Controller firmware major/minor/micro | `02 01 04` → 2.1.4 |
| 3 | 3 | reserved | `00 00 00` |
| 6 | 1 | Bluetooth patch major | `0C` → 12 |
| 7 | 3 | reserved | `00 00 00` |
| 10 | 6 | Per-unit BD_ADDR | `9E 2B AB AB A9 3C` |

### Surface B — bulk command `0x10/0x01` "Get Firmware Version Info" (12 bytes) — **Confirmed**

Dispatcher `case 0x10` (`switch_pro2.c:740-742`). `ns2_firmware_info` (`switch_pro2.c:234-237`),
layout per `commands.md` §"Command 0x10":

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | Controller firmware major/minor/micro | `02 01 04` → 2.1.4 |
| 3 | 1 | Controller type (`00`=JoyCon L, `01`=JoyCon R, `02`=Pro, `03`=GameCube) | `02` |
| 4 | 3 | Bluetooth patch major/minor/micro | `0C 00 00` → 12.0.0 |
| 7 | 1 | pad | `00` |
| 8 | 3 | **DSP firmware** major/minor/micro (*"Only present on Pro Controller with updated firmware"*) | `00 02 03` → 0.2.3 |
| 11 | 1 | pad | `00` |

### Surface C — flash **memory reads** via command `0x02` (candidate validation surface) — **see §3**

Command `0x02/0x04` (memory read, ≤0x50 B) and `0x02/0x01` (0x40-byte block) let the console read
**any** address in the controller's 2 MB flash (`commands.md` §"Command 0x02"; our handler routes
these through `ns2_mem_read`). Captures prove that the protocol can read firmware/DSP regions, but
we do not yet have console-side evidence identifying which addresses are consulted when the update
prompt is decided. This is nevertheless where PicoSwitch2 diverges most visibly from genuine
hardware (§3).

### Byte-encoding rule

Each component byte's **hex value equals the decimal version number.** `0x04` = **4** → "2.1.4";
`0x0C` = **12**. To report 3.0.0 set `03 00 00`; version `.20` = `0x14`.

## 2. What we report today, and where it came from

| Surface | Value | Source |
|---|---|---|
| Ctrl firmware (A+B) | **2.1.4** | Live genuine PC2 `0x10/01` via UART↔BLE bridge, raw `02 01 04` |
| Type | 0x02 (Pro) | Constant |
| BT patch | **12.0.0** | Stable across captures |
| DSP firmware (B) | **0.2.3** | Same live reply, raw `00 02 03`; agrees with genuine SPI dump |
| Memory region C | Factory identity at `0x13000`, pairing count at `0x1FA000`, post-update state `00 00 00 00` at `0x1FD010`; otherwise erased `0xFF` | `ns2_mem_read` + `ns2_firmware_profile_flash_byte` |

The repo's bundled USB capture is an **older** genuine PC2 (**1.1.5 / no DSP**). A later private
capture supplied **2.0.17 / 12.0.0 / 0.2.2**, which was also stale. The live bridge removed that
dependency and queried the user's current genuine controller directly.

## 3. Root cause and hardware proof

The console uses a known exact/coherent version identity rather than accepting any numerically high
tuple. The hardware sequence on 2026-07-21 established this directly:

1. UART tracing showed the console queried EP0 firmware info once and command `0x10/01` once.
2. Its only command-`0x02` reads were `0x13080`, `0x130C0`, `0x1FC040`, `0x13040`, `0x13100`, and
   `0x13060`—calibration/user-data regions. It did **not** read firmware banks, the DSP image, or
   `0x1FD010` before offering the update.
3. A RAM-only tuple of `255.255.255 / 255.255.255 / 255.255.255` still triggered the update.
4. A genuine Pro Controller 2 paired to the dongle returned raw command-`0x10/01` payload
   `02 01 04 02 0C 00 00 00 00 02 03 00`.
5. Applying the decoded `2.1.4 / 12.0.0 / 0.2.3` tuple live, with no other response change, made
   Settings → Update Controllers report all known controllers up to date.

This isolates the stale reported tuple as the root cause for this console firmware. Firmware-image
headers and sparse update-state bytes remain genuine/Pico differences, but they do not participate
in the observed prompt decision.

The following **byte differences** are verified against
`dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` (a genuine, already-updated PC2). The differences are
confirmed; their participation in the update decision is not:

| Region (memory_layout.md) | Genuine dump | PicoSwitch2 `ns2_mem_read` | Divergent? |
|---|---|---|---|
| `0x0` Initial FW | `01 00 64 AA "SYS " …` (magic **0xAA640001**, real image) | `0xFF …` | **YES** |
| `0x11000` failsafe FW addr | `FF …` (unused) | `0xFF …` | no |
| `0x12000` failsafe magic | `FF …` (unset → default bank) | `0xFF …` | no |
| `0x15000` FW bank #1 | `01 00 64 AA "SYS " …` (real image, size `00 03 E7 10`) | `0xFF …` | **YES** |
| `0x75000` FW bank #2 | `01 00 64 AA "SYS " …` (real image, size `00 03 D5 70`) | `0xFF …` | **YES** (⇒ this unit *was* updated: bank #2 is blank on factory firmware) |
| `0x175000` DSP firmware | `DSPH … 00 02 03 …` (**DSP 0.2.3**, `MT3616A0`) | `0xFF …` | **YES** |
| `0x1FD000` shipment flag | `FF …` (cleared after first console pairing) | `0xFF …` | no |
| `0x1FD010` post-update state | `00 00 00 00` (*appears after a firmware update*) | `00 00 00 00` in the 2026-07-21 experiment | no |

**Former candidate triggers, now resolved for this prompt path:**

1. **`0x1FD010` post-update state — not consulted.** The 2026-07-21 build returned the four observed
   zero bytes, but the console did not read this address during the prompt decision.
2. **DSP blob mismatch — not consulted.** The console never read `0x175000`; changing the reported
   DSP component alone to 0.2.3 did not suppress the prompt.
3. **Missing FW image headers — not consulted.** The console never read `0x0`, `0x15000`, or
   `0x75000` during the decision path.

> **Why raising the version failed:** the console does not treat the tuple as a simple monotonic
> minimum. The exact current genuine tuple succeeded where an all-255 ceiling failed.

## 4. Evaluation of ndeadly's `commands.md` (what it tells us)

`nso-gc-refs/switch2_controller_research/commands.md` (evaluated in full) clarifies the whole
version/update surface:

- **`0x10/01` Get Firmware Version Info** — confirms the exact 12-byte layout in §1B, including that
  **DSP is only present on updated Pro Controllers**. Their JoyCon example
  (`01 00 0e 01 0c 00 00 00 ff ff ff ff`) shows `ff ff ff ff` where a Pro's DSP triplet sits — i.e.
  "no DSP" is encoded as `0xFF` padding, matching the older 1.1.5 PC2.
- **`0x02` Flash Memory** — read/write/erase of the 2 MB space; `0x02/01` example literally reads
  `0x175000` and returns the `DSPH … 00 02 02` DSP header. **This is the proof that the DSP version
  is a memory value the console can read** (Surface C), not just a string.
- **`0x0D` Firmware Update** — the transport we don't implement, now fully mapped (§5): subcommands
  `01` init, `02` set failsafe address (`0x15000`/`0x75000`), `03` set image size, `04` transfer
  data (≤`0x4C` B/chunk over USB), `05` end transfer, `06` verify (CRC-32), `07` finalise+reboot.
  memory_layout.md adds the bank/magic mechanics (`0x11000` address, `0x12000` `0xBEEF` magic,
  alternating banks). A **CAUTION** notes a bad `0x0D/02` address can brick real hardware — reinforcing
  that we must never *act* on these, only (optionally) *capture* them (§7).
- **`0x06/03` Reboot Controller** — *"called following a controller firmware update … reboots the
  controller and/or reloads the firmware."* Relevant to how a capture path should *end* (§7-3).
- Not a version surface, but confirmed alignment: `0x0B` battery, `0x0C` feature flags
  (`0x27` = buttons+sticks+IMU+rumble), `0x09` player LEDs, `0x15`/`0x03-07/09` pairing — all match
  our dispatcher, so nothing else in `commands.md` implicates the update prompt.

## 5. The `0x0D` update transport — and why "update failed"

When an update is accepted/attempted, the console runs `0x0D` (`commands.md` §"Command 0x0D"):

```
0x0D/01 Initialise update
0x0D/02 Set failsafe address   (0x02, addr ∈ {0x15000, 0x75000})
0x0D/03 Set image size         (region id + size, e.g. ~0x03B060 ≈ 240 KiB)
0x0D/04 Transfer update data   (repeated; ≤0x4C B/chunk over USB;
                                over BT via char 4147423d-… handle 0x0018, 0xB5C-B blocks)
0x0D/05 End data transfer
0x0D/06 Verify update          (region id + size + CRC-32)
0x0D/07 Finalise → controller reboots (then 0x06/03 reboot)
```

**PicoSwitch2 has no `case 0x0D`.** The dispatcher (`switch_pro2.c:646-781`) handles
`0x01/02/03/07/09/0B/0C/10/11/15/16/18` and a `default:` bare-ACK (`:778-780`). So every `0x0D`
subcommand hits `default`, we ACK an empty header, the console never gets the chunk/CRC handshake it
expects, and the update **times out / aborts → "update failed."** This is intentional
(`usb-spec.md:167-170`): we report a version instead of pretending to accept Nintendo firmware
writes. (Don't confuse this with `case 0x03` **sub** `0x0D` "Init USB" at `switch_pro2.c:648` — a
different command.)

## 6. Applying / changing the reported version

### 6a. Apply a real update — **No (permanent hardware limit).**

The image targets Nintendo's controller MCU; it cannot execute or be flashed on RP2040/RP2350
(`usb-spec.md:168-169`, and the encrypted `0xAA640001`/"SYS " images seen in the dump at `0x0`/
`0x15000`/`0x75000`). We must never write it into our own flash.

### 6b. Change the *reported* version — **maintainer-selected release profile.**

Per project decision, the version is **not** exposed for end users to set (an invalid value could
break enumeration or provoke worse update behavior). It stays a compile-time profile the maintainer
selects and releases:

1. Select the complete tuple through CMake (`NS2_PRO_FIRMWARE_VERSION`,
   `NS2_PRO_BLUETOOTH_VERSION`, `NS2_PRO_DSP_VERSION`) or the matching `build.ps1` parameters. Each
   component must be decimal `0..255`.
2. **Clean rebuild** (`build.ps1 <board> -Clean` — clean-build-after-revert lesson). The build script
   passes all three values explicitly even without `-Clean`, preventing a stale experimental cache.

The UART `profile` command is an explicitly diagnostic, RAM-only exception. It updates both version
surfaces together and soft re-enumerates USB for rapid maintainer A/B testing; it never writes config
flash and disappears on power cycle. The successful `2.1.4 / 12.0.0 / 0.2.3` result was promoted to
the compiled default, so end users do not need or receive a version setting.

## 7. Capture-analysis design sketch (research; **not implemented**)

We can't *apply* the image, but we can turn the dongle into a **firmware-image tap** that accepts the
`0x0D` transfer purely to **capture the encrypted bytes** for offline RE — the first open capture of
a genuine PC2 update image. Fully documented here; **no code written.**

### 7.1 Goal & scope

- **Capture** the complete `0x0D` update stream (all `04` chunks reassembled in order), plus the
  surrounding parameters (`02` failsafe addr, `03` size, `06` size+CRC-32), to reconstruct the image
  a console would have written to a failsafe bank.
- **Never** persist it as executable firmware; never act on `0x0D/02` addresses against our own
  flash. Capture only.
- Deliverable: a byte-exact image file + a metadata sidecar, analyzed against the known header
  formats (`0xAA640001` "SYS " at `0x0`/`0x15000`; `DSPH`/`MT3616A0` at `0x175000`).

### 7.1a Interaction model — the commit is **autonomous** (no dongle interaction in Phase 1)

While the dongle is on the console it is a black box: no BOOTSEL gesture, no config mode, no button
is reachable. **There is therefore no manual "commit" step.** The capture firmware commits *itself*,
driven entirely by the `0x0D` packets the console sends:

- `0x0D/01` (init) → **arms** the sink.
- each `0x0D/04` chunk → buffered and **auto-flushed to flash a sector at a time** as 4 KiB fills.
  This *is* the commit — continuous, protocol-triggered, zero input.
- `0x0D/05`/`07` (end/finalise) → write the metadata sidecar + completeness flag, close the sink.

Runtime flash writes during normal operation are already proven safe in this codebase — `config.c`
does exactly that at runtime (`config.c:277-281`, multicore lockout + interrupts disabled + the flash
routine running from SRAM). No human trigger is involved there either.

**The only interaction the operator has is on the *Switch*, not the dongle:** tapping **"Update"** on
the console's own controller-update prompt (the same prompt that fails today) is what makes the
console emit the `0x0D` stream. That is fully available to the operator; the dongle merely reacts.

**Feedback without input:** the one output visible while on the console is the **LED**. The firmware
already blinks it as a diagnostic tracer (`g_ns2_stage`, driven by the platform layer). A capture
build reuses it — a distinct pattern for *armed → capturing → committed* — so the operator can see
the capture finished before unplugging, despite having no input path.

**End-to-end operator workflow (no "reach into the dongle" moment):**

1. **PC, beforehand:** flash a **capture-enabled build** via the normal BOOTSEL/UF2 route.
2. **Switch:** plug in, connect a controller, **accept the update prompt on-screen**.
3. **Dongle (autonomous):** captures + flushes to flash by itself; LED confirms completion.
4. **Unplug**, plug into PC, enter config mode, **read the blob + sidecar out over CDC** (Phase 3).

### 7.2 State machine (proposed `case 0x0D`)

```
IDLE ──0x0D/01──▶ INIT            (allocate/rewind capture sink; ACK)
INIT ──0x0D/02──▶ ADDR   {region_id, failsafe_addr}   record, ACK   (do NOT write our flash)
ADDR ──0x0D/03──▶ SIZED  {region_id, image_size}      record, ACK
SIZED─0x0D/04──▶ XFER    append chunk[≤0x4C] at running offset; ACK each
XFER ─0x0D/04──▶ XFER    (repeat until image_size bytes seen)
XFER ─0x0D/05──▶ ENDED   mark stream complete; ACK
ENDED─0x0D/06──▶ VERIFY  {size, CRC-32}: recompute CRC-32 over capture; log match/mismatch; ACK
VERIFY0x0D/07──▶ DONE    finalise; flush capture + metadata; ACK
DONE ─(0x06/03)─▶ reboot: choose ending (§7.4)
```

Each response must mirror the genuine ACK shape (`0d 01 01 0X 10 78 00 00`, `dir=0x01`,
`ack=0x10/0x78` — note `0x0D` uses the `10 78` ack form, not the `00 f8` form used by init commands;
verify against a real capture before trusting the exact ACK bytes — **Unknown** until captured).

### 7.3 Where the bytes go — this is a **two-phase, capture-on-console / read-out-on-PC** flow

**Critical constraint (drives the whole design): the capture and the read-out cannot overlap.** The
update `0x0D` stream arrives only while the dongle is plugged into the **console**; config mode / the
CDC channel is only reachable while plugged into the **PC**. The Pico has **one** USB port, so it is
never connected to both at once. And unplugging from the console is a **full power loss** — the Pico
is bus-powered with no battery/RTC-backup domain, so **SRAM does not survive the move to the PC.**

Consequently:

- ❌ **"Stream chunks to the PC over CDC as they arrive" is impossible here** (an earlier draft's
  "preferred" option). At capture time there is no PC attached. Discard this path for
  console-sourced updates.
- ✅ **The capture MUST be committed to internal flash on-device**, because flash is the *only*
  storage that persists across the unplug-from-console → replug-into-PC power cycle. **Yes — a blob
  written to flash persists across that unplug/replug.** This is exactly how `config.c` already
  survives power cycles: it writes to the last 4 flash sectors
  (`CONFIG_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - 4*FLASH_SECTOR_SIZE`) via
  `multicore_lockout_start_blocking()` → `save_and_disable_interrupts()` → `flash_range_erase()` →
  `flash_range_program()` (`config.c:277-281`). A capture sink reuses the same primitives with a
  larger reserved partition.

**The flow:**

```
Phase 1 — on the CONSOLE:  capture 0x0D chunks  →  commit to a reserved flash region
Phase 2 — unplug (image persists in flash across the power loss)
Phase 3 — plug into the PC, enter config mode  →  read the blob + sidecar out over CDC
```

**Flash-write engineering (two real constraints):**

1. **Flash ops stall both cores.** `flash_range_erase`/`program` require locking out the other core
   and disabling interrupts (XIP can't run during a flash op), so you cannot block for a sector erase
   in the middle of a high-rate transfer. Exploit the **ACK-gated** `0x0D/04` protocol: buffer one
   sector (4 KiB) in SRAM, and **defer the per-chunk ACK until the flush completes** — the console
   waits, which paces the transfer around the flash writes automatically.
2. **Board-dependent buffering.**
   - **Pico 2 W (RP2350, 520 KiB SRAM, 4 MB flash):** could hold the whole ~240 KiB image in SRAM and
     do a **single** flash commit at `0x0D/07` finalise (while still on console power), then persist
     across the unplug.
   - **Pico W (RP2040, 264 KiB SRAM, 2 MB flash):** too little SRAM for the whole image → must
     **flush progressively**, sector by sector, during the transfer.
   - **Progressive sector-flushing is the more robust choice on both boards:** a partial capture
     survives even if the transfer aborts or the user unplugs early. The image needs a **reserved
     ≥256 KiB (~60 sector) partition** clear of both the firmware and the `config.c` region — trivial
     on Pico 2 W's 4 MB, tighter but feasible on Pico W's 2 MB.

**Commit-before-unplug is mandatory.** Anything still only in SRAM when VBUS drops is lost. Progressive
flushing (or committing at finalise, which happens while still console-powered) guarantees the persisted
image is complete before you can pull the plug.

Emit a **metadata sidecar** (also in the reserved flash region) with: timestamp, `region_id`, target
`failsafe_addr`, declared `image_size`, declared CRC-32, observed byte count, computed CRC-32, capture
completeness flag, and transport (USB vs BT — BT uses the `4147423d-…` characteristic, handle
`0x0018`, 0xB5C-byte blocks of 30×0x64 chunks, so a BT capture path must hook that characteristic, not
just the command channel).

### 7.4 Ending the transaction

The `0x06/03` "reboot controller" is a **logical** command we answer however we like — it is *not* a
real power cycle, so it never threatens the flash commit (that only happens when the user physically
unplugs, by which point Phase 1 is already persisted). The captured image is read out later in a
separate **PC** session (Phase 3), independent of how Phase 1 ends. Three candidate endings; which
avoids a retry storm is itself an experiment (**Unknown**):

- **Report success** (`0x0D/07` + ACK `0x06/03` reboot) then **re-enumerate advertising the new
  version** we now know from the captured header — cleanest if the console then stops nagging.
- **Report benign failure** at `0x06`/`0x07` — simplest, but may re-prompt on next boot.
- **Silently ACK and drop** — captures bytes but likely leaves the console believing the update
  pending.

### 7.5 Analysis, offline

- Parse the reassembled image against `memory_layout.md`: `0xAA640001` header (name, size u32 BE,
  IV/tag block at `0x10`), or `DSPH` (`MT3616A0 DSP`, version at +0xC — the one **unencrypted** blob).
- The main firmware images are **signed/encrypted for Nintendo's MCU**; capture enables studying
  header/bank/CRC/versioning structure and diffing across console firmware releases. **Decrypt/forge
  is out of scope** (needs keys we don't have). The DSP blob is unencrypted and immediately useful
  (we already hold 0.2.3 in the SPI dump).

### 7.6 Prerequisites & risk

- **Gated on a genuine console-side capture** of a real update attempt to learn the exact `0x0D`
  ACK shapes, chunk cadence, and the `06`/`07` semantics. We have **none** (the bundled USB capture
  is a PC/Windows session). Belongs in `/docs/experiments` as a staged experiment.
- **Zero risk to our hardware** if we only capture (never write our own flash from `0x0D/02`
  addresses). The `commands.md` brick warning applies to *real* controllers, not to a capture sink.

## 8. Answering: "what do you need to check a real controller's version — is an SPI dump enough?"

**Short answer: an SPI dump is necessary but not sufficient.** It depends which "version" you mean.

| You want… | SPI dump gives it? | How / why |
|---|---|---|
| **DSP firmware version** | **Yes** | `0x175000+0xC` in the `DSPH` blob. Our dump = `00 02 03` → **0.2.3** (Confirmed, byte-read here). |
| **Firmware image presence / size / update state** | **Yes** | Headers at `0x0`/`0x15000`/`0x75000` (`0xAA640001` "SYS ", size fields); "updated" flag at `0x1FD010`; shipment flag `0x1FD000`. All read directly from our dump. |
| **Which regions the console *checks* to decide on an update** | **Not from SPI; now captured live** | UART tracing recorded the complete pre-prompt read set; it contained calibration/user regions only, not firmware/DSP/update-state regions. |
| **The controller-firmware `major.minor.micro` string (e.g. "2.1.4")** | **Not reliably from SPI; now queryable live** | `btversion` asks a genuine controller paired to the dongle for native `0x10/01`; the validated unit returned `2.1.4`. |

**Recommended method to characterize a real controller fully (no code changes):**

1. **SPI dump** (one image stored under two byte-identical filenames:
   `dumps/SPI/2069_spi_dump_*.bin`) → extract DSP version
   (`0x175000+0xC`), FW image sizes (`0x15000`/`0x75000`), and update flags (`0x1FD010`). Done in §3
   here.
2. **Live `btversion` bridge** → exact genuine controller-FW / BT / DSP triplets on the wire. Done.
3. **Console-side UART trace** → exact `0x02` memory addresses read before prompting. Done for normal
   enumeration. The full `0x0D` update exchange remains deliberately uncaptured (§7).

So: SPI remains useful for image layout and persistent state, while UART now supplies both sides of
the live decision—the console's queries and the genuine controller's native version response.

## 9. Risks & open questions

- **Future console updates can change the exact accepted tuple.** Re-run `btversion` against an
  updated genuine controller, then validate a RAM-only `profile` override before changing defaults.
- **The DSP image remains blank in emulation.** This is currently harmless because the console does
  not read it on the validated prompt path; do not fabricate a blob unless a future trace requires it.
- **`0x0D` ACK/timing shapes unconfirmed here.** The subcommand map is Strong Evidence (ndeadly); the
  exact ACK bytes, chunk cadence, and `06`/`07` semantics need a genuine console-side update capture.
- **The general console-side tracer is still incomplete.** Firmware-query summaries are live, but
  NFC, report-`0x09` motion, and arbitrary command timing still need the structured Tier-1 stream.

## 10. Summary

| Question | Answer |
|---|---|
| How does the Switch 2 read firmware version? | EP0 req `0x02` + bulk `0x10/01`; UART confirmed one query of each before the prompt decision. |
| Why did a *higher* reported version still nag? | The console expects a known coherent tuple, not a numeric minimum. Genuine `2.1.4 / 12.0.0 / 0.2.3` passed; all-255 failed. |
| Why does the update *fail*? | No `0x0D` update-transport handler — it bare-ACKs and aborts (by design). |
| Can we apply an update? | **No** — image targets Nintendo's MCU, unflashable on RP2040/RP2350. |
| Can we *capture* one? | **Feasible, unimplemented** — a `0x0D` capture sink streaming chunks to the PC over CDC (§7); gated on a console-side capture. |
| Is an SPI dump enough to check version? | For DSP image version + update state, yes; for the live wire tuple, use the UART `btversion` bridge. |
| How is our version changed? | Maintainer-selected CMake/build profile. UART offers a non-persistent RAM-only A/B override, not an end-user setting. |

## 11. References

- `include/ns2_firmware_profile.h` + `src/ns2_firmware_profile.c` (coherent version profile and
  sparse post-update state); `src/switch_pro2/switch_pro2.c` (`ns2_mem_read`, EP0 and command
  `0x10` integration, plus the dispatcher `default:` that swallows `0x0D`).
- `nso-gc-refs/switch2_controller_research/commands.md` — `0x02` flash memory, `0x0D` firmware
  update (full subcommand map), `0x10` firmware info, `0x06/03` reboot.
- `nso-gc-refs/switch2_controller_research/memory_layout.md` — the 2 MB map: FW banks
  `0x15000`/`0x75000`, DSP `0x175000`, update flags `0x1FD000`/`0x1FD010`, failsafe `0x11000`/`0x12000`.
- `dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` — genuine updated PC2; DSP **0.2.3**, both FW banks
  imaged, `0x1FD010 = 00 00 00 00` (verified in §3).
- `docs/switch2/usb-spec.md:150-170` — firmware-version compatibility + `0x0D` overview.
- `STATUS.md` (hardware-confirmed firmware identity/update status), `PLAN.md` (completed controller
  update compatibility work).
- Dycool / NS-PC-Control `PC2_Gyro_*.pcapng` — the 2.0.17 / DSP-0.2.2 source (private; Strong
  Evidence).
