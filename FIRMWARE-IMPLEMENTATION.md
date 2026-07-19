# Controller Firmware Versioning — Implementation & Analysis

> Comprehensive breakdown of how the Switch 2 decides a Pro Controller 2 needs a firmware update, how
> PicoSwitch2 answers the relevant queries, **why raising the reported version did not stop the
> "update available → update failed" prompt**, whether the update image can be *read/captured* or
> *applied*, and how the reported version + firmware-region data are maintained.
>
> **Documentation only — no code changed by this file.** It is the durable reference behind the
> `STATUS.md` P2 item *"Pro Controller 2 update prompt"* and `usb-spec.md` §"Firmware-version
> compatibility".
>
> Evidence base in this repo: `src/switch_pro2/switch_pro2.c`, `docs/switch2/usb-spec.md`,
> `nso-gc-refs/switch2_controller_research/{commands,memory_layout,hid_reports}.md` (ndeadly), and
> **two genuine 2 MB PC2 SPI dumps** (`dumps/SPI/2069_spi_dump_*.bin`) that this revision reads
> directly. Confidence tags: **Confirmed** (byte-verified here), **Strong Evidence** (external
> capture, consistent), **Hypothesis**, **Unknown**.

## 0. The honest reality up front

1. **We only ever report a version and serve memory bytes; we never run Nintendo firmware.** The
   dongle is a RP2040/RP2350 emulating a controller. The genuine update image targets Nintendo's
   controller MCU and **physically cannot be flashed onto the Pico** (§6). "Firmware support" here
   means: answer the console's version/memory queries convincingly, and — optionally, as research —
   *capture* the encrypted update stream for offline analysis (§7).
2. **The update prompt is not driven by the version string alone.** This is the corrected core
   finding of this document (§3). The user raised the reported version *above retail* and the prompt
   **still appeared** — proving the console's decision consults **more than** the `0x10`/EP0 version
   triplet. The genuine SPI dumps show what that "more" is (§3).
3. **The prompt then fails because we do not implement the `0x0D` update transport** (§5). Even if
   the console starts an update, we bare-ACK the transfer instead of running the multi-subcommand
   protocol, so it aborts. That is *by design* — we never accept Nintendo firmware writes. The
   controller keeps working regardless; the nag is cosmetic.
4. **The reported version is maintained in-app by the maintainer**, not user-configurable (§6). A
   wrong/invalid version could plausibly break enumeration or trigger worse update behavior, so the
   version stays a compile-time constant updated in the app.

## 1. Where firmware version is reported — the surfaces the console reads

The console gathers controller-version information from **several** places during bring-up. Only the
first two are simple "version strings"; the rest are **memory reads** that turn out to matter more
(§3).

### Surface A — EP0 vendor control request `0x02` (enumeration, 16 bytes) — **Confirmed**

Read over endpoint 0 during USB enumeration, before the bulk command channel
(`switch_pro2.c:1098`). `ns2_ctrl_info` (`switch_pro2.c:228-230`):

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | Controller firmware major/minor/micro | `02 00 11` → 2.0.17 |
| 3 | 3 | reserved | `00 00 00` |
| 6 | 1 | Bluetooth patch major | `0C` → 12 |
| 7 | 3 | reserved | `00 00 00` |
| 10 | 6 | Per-unit BD_ADDR | `9E 2B AB AB A9 3C` |

### Surface B — bulk command `0x10/0x01` "Get Firmware Version Info" (12 bytes) — **Confirmed**

Dispatcher `case 0x10` (`switch_pro2.c:740-742`). `ns2_firmware_info` (`switch_pro2.c:234-237`),
layout per `commands.md` §"Command 0x10":

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | Controller firmware major/minor/micro | `02 00 11` → 2.0.17 |
| 3 | 1 | Controller type (`00`=JoyCon L, `01`=JoyCon R, `02`=Pro, `03`=GameCube) | `02` |
| 4 | 3 | Bluetooth patch major/minor/micro | `0C 00 00` → 12.0.0 |
| 7 | 1 | pad | `00` |
| 8 | 3 | **DSP firmware** major/minor/micro (*"Only present on Pro Controller with updated firmware"*) | `00 02 02` → 0.2.2 |
| 11 | 1 | pad | `00` |

### Surface C — flash **memory reads** via command `0x02` (the decisive surface) — **see §3**

Command `0x02/0x04` (memory read, ≤0x50 B) and `0x02/0x01` (0x40-byte block) let the console read
**any** address in the controller's 2 MB flash (`commands.md` §"Command 0x02"; our handler
`switch_pro2.c:717-738` → `ns2_mem_read` `:300-310`). The console uses this to inspect the actual
firmware images, the DSP blob, and update-state flags — **not** just the version strings. This is
where PicoSwitch2 diverges hardest from a genuine controller (§3).

### Byte-encoding rule

Each component byte's **hex value equals the decimal version number.** `0x11` = **17** → "2.0.17";
`0x0C` = **12**. To report 3.0.0 set `03 00 00`; version `.20` = `0x14`.

## 2. What we report today, and where it came from

| Surface | Value | Source |
|---|---|---|
| Ctrl firmware (A+B) | **2.0.17** | Dycool `PC2_Gyro_*.pcapng` (updated retail unit) |
| Type | 0x02 (Pro) | Constant |
| BT patch | **12.0.0** | Stable across captures |
| DSP firmware (B) | **0.2.2** | From the same capture |
| Memory region C | **all `0xFF`** except the 0x160-byte factory identity window at `0x13000` | `ns2_mem_read` (`switch_pro2.c:300-310`) |

The repo's bundled USB capture is an **older** genuine PC2 (**1.1.5 / no DSP**); the constants were
advanced to 2.0.17 to try to out-rank the console's known-latest. **That did not stop the prompt**
— §3 explains why.

## 3. Root cause — why a higher version still nagged (**corrected**)

**The update decision is memory-content–driven, not version-string–driven.** A genuine controller
carries real firmware **images** and **update-state flags** in flash; PicoSwitch2 returns `0xFF`
(blank) for every one of those regions because `ns2_mem_read` only backs the 0x160-byte identity
window at `0x13000` and answers `0xFF` everywhere else (`switch_pro2.c:303-308`). So no matter what
triplet Surfaces A/B report, the console's **Surface C memory reads** see a controller that looks
**un-imaged and never-updated** → it offers/forces an update.

This is now **byte-verified** against `dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` (a genuine,
already-updated PC2). Genuine bytes vs. what we serve:

| Region (memory_layout.md) | Genuine dump | PicoSwitch2 `ns2_mem_read` | Divergent? |
|---|---|---|---|
| `0x0` Initial FW | `01 00 64 AA "SYS " …` (magic **0xAA640001**, real image) | `0xFF …` | **YES** |
| `0x11000` failsafe FW addr | `FF …` (unused) | `0xFF …` | no |
| `0x12000` failsafe magic | `FF …` (unset → default bank) | `0xFF …` | no |
| `0x15000` FW bank #1 | `01 00 64 AA "SYS " …` (real image, size `00 03 E7 10`) | `0xFF …` | **YES** |
| `0x75000` FW bank #2 | `01 00 64 AA "SYS " …` (real image, size `00 03 D5 70`) | `0xFF …` | **YES** (⇒ this unit *was* updated: bank #2 is blank on factory firmware) |
| `0x175000` DSP firmware | `DSPH … 00 02 03 …` (**DSP 0.2.3**, `MT3616A0`) | `0xFF …` | **YES** |
| `0x1FD000` shipment flag | `FF …` (cleared after first console pairing) | `0xFF …` | no |
| `0x1FD010` "updated" flag | `00 00 00 00` (*set after a firmware update*) | `0xFF …` | **YES** (⇒ we read as **never updated**) |

**Ranked candidate triggers** (each is a concrete, testable hypothesis; the console-side capture to
confirm *which* it checks does not exist yet):

1. **`0x1FD010` never-updated flag — Strong Hypothesis.** memory_layout.md: bytes `0x1FD010-13`
   are "set after updating controller firmware." Genuine updated unit = `00 00 00 00`; we return
   `0xFF` = "never updated." A console that reads this byte to decide "does this controller have the
   current firmware?" would nag every PicoSwitch2 forever, **independent of the version string** —
   exactly the observed symptom.
2. **DSP version mismatch (`0x175000+0xC`) — Strong Hypothesis.** We *claim* DSP 0.2.2 in Surface B
   but serve `0xFF` at `0x175000`. Our own dump has **0.2.3**. If the console validates the DSP blob
   (or its version) via memory read and finds blank / a mismatch with its known-latest, it prompts.
3. **Missing FW image headers (`0x0`, `0x15000`) — Hypothesis.** If the console reads the image
   header (magic/size/hash) to gauge the installed version rather than trusting the `0x10` string, a
   blank (`0xFF`) region reads as "no valid firmware."

> **Why raising the version "obviously missed something": Confirmed.** Surfaces A/B were the *only*
> thing changed, but the console's verdict is dominated by Surface C, which we don't populate. This
> is the corrected model; the earlier "reported < latest → prompt" note in v1 of this doc was
> incomplete.

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

### 6b. Change the *reported* version — **maintainer-updated constants, not user-configurable.**

Per project decision, the version is **not** exposed for end users to set (an invalid value could
break enumeration or provoke worse update behavior). It stays a compile-time constant block the
maintainer bumps and re-releases:

1. Edit `src/switch_pro2/switch_pro2.c:209-217` (`NS2_PRO_FW_*`, `NS2_PRO_BT_*`, `NS2_PRO_DSP_*`),
   honoring the decimal-in-hex rule (§1). One block feeds both Surfaces A and B, so they can't desync.
2. **Clean rebuild** (`build.ps1 <board> -Clean` — clean-build-after-revert lesson).

**Important caveat given §3:** bumping these constants alone will **not** clear the prompt, because
the console's decision is dominated by Surface C (blank firmware/DSP regions + the `0x1FD010`
never-updated flag). Fully suppressing the nag requires *also* teaching `ns2_mem_read` to serve
plausible firmware-region bytes — most promisingly `0x1FD010 = 00 00 00 00` and a valid `DSPH`
header at `0x175000` matching the reported DSP version — which is **not done** and needs on-console
experiments to confirm which region the console actually gates on (§8). This file does **not** change
that code; it records the design so a future, deliberate increment can.

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
| **Which regions the console *checks* to decide on an update** | **No** | Requires a **console-side command capture** (the sequence of `0x02` memory reads the console issues during init). We have only a PC session — this is the standing gap. |
| **The controller-firmware `major.minor.micro` string (e.g. "2.0.17")** | **Not reliably** | memory_layout.md exposes **no** plain "controller firmware version" field. The triplet reported by `0x10`/EP0 lives in the MCU program image (the encrypted `0x0`/`0x15000` blobs) or is computed by firmware — **not** trivially extractable from the SPI dump. Get it from a **live `0x10/01` command capture** instead. |

**Recommended method to characterize a real controller fully (no code changes):**

1. **SPI dump** (we already have two: `dumps/SPI/2069_spi_dump_*.bin`) → extract DSP version
   (`0x175000+0xC`), FW image sizes (`0x15000`/`0x75000`), and update flags (`0x1FD010`). Done in §3
   here.
2. **Live command capture** of `0x10/01` (+ EP0 `0x02`) from the genuine controller → the exact
   reported controller-FW / BT / DSP triplets on the wire.
3. **Console-side capture** (the missing piece) → which `0x02` memory addresses the console reads
   before prompting, and the full `0x0D` update exchange. This single capture would unblock §3's
   ranking, §6b's suppression work, and §7's capture path simultaneously.

So: to read the **DSP version and update state**, the SPI dumps we already hold are enough (and this
doc extracts them). To read the **wire version string**, use a `0x10` capture. To understand the
**console's update decision** (the thing that actually controls the prompt), you need a console-side
capture we do not yet have.

## 9. Risks & open questions

- **Which Surface-C region gates the prompt is Unknown.** §3 ranks `0x1FD010` and the DSP blob first,
  but only an on-console experiment (serve one region at a time; observe the prompt) or a console
  capture can confirm. Do not tune blindly.
- **DSP/version desync.** We report DSP 0.2.2 (Surface B) but the genuine dump is DSP 0.2.3, and we
  serve blank at `0x175000`. Any future suppression work must keep the reported DSP triplet, the
  `0x175000` `DSPH` header, and the console's known-latest mutually consistent.
- **`0x0D` ACK/timing shapes unconfirmed here.** The subcommand map is Strong Evidence (ndeadly); the
  exact ACK bytes, chunk cadence, and `06`/`07` semantics need a genuine console-side update capture.
- **No console-side capture at all** — the same standing gap that blocks NFC
  (`nfc-protocol-inventory.md` §2.5) and report-`0x09` motion. A reproducible console-side capture
  path (`STATUS.md` next-steps #5) unblocks all of them.

## 10. Summary

| Question | Answer |
|---|---|
| How does the Switch 2 read firmware version? | EP0 req `0x02` + bulk `0x10/01` (version strings) **and** `0x02` memory reads of the firmware/DSP regions + update flags (the decisive surface). |
| Why did a *higher* reported version still nag? | The decision is **memory-content–driven**. We serve `0xFF` for every firmware/DSP region and the `0x1FD010` never-updated flag, so the console sees an un-imaged, never-updated controller regardless of the string. **Byte-verified against a genuine dump.** |
| Why does the update *fail*? | No `0x0D` update-transport handler — it bare-ACKs and aborts (by design). |
| Can we apply an update? | **No** — image targets Nintendo's MCU, unflashable on RP2040/RP2350. |
| Can we *capture* one? | **Feasible, unimplemented** — a `0x0D` capture sink streaming chunks to the PC over CDC (§7); gated on a console-side capture. |
| Is an SPI dump enough to check version? | **For DSP version + update state, yes** (extracted in §3/§8). **For the wire version string, no** — needs a `0x10` capture. **For the console's decision logic, no** — needs a console-side capture. |
| How is our version changed? | Maintainer-edited constants (`switch_pro2.c:209-217`) + clean rebuild; **not** user-configurable. Note this alone won't clear the prompt (§6b). |

## 11. References

- `src/switch_pro2/switch_pro2.c:204-237` (version constants + Surfaces A/B), `:300-310`
  (`ns2_mem_read`, Surface C — the `0xFF` fill), `:717-742` (`0x02`/`0x10` handlers), `:646-781`
  (dispatcher + `default:` that swallows `0x0D`).
- `nso-gc-refs/switch2_controller_research/commands.md` — `0x02` flash memory, `0x0D` firmware
  update (full subcommand map), `0x10` firmware info, `0x06/03` reboot.
- `nso-gc-refs/switch2_controller_research/memory_layout.md` — the 2 MB map: FW banks
  `0x15000`/`0x75000`, DSP `0x175000`, update flags `0x1FD000`/`0x1FD010`, failsafe `0x11000`/`0x12000`.
- `dumps/SPI/2069_spi_dump_2026-07-10_1422.bin` — genuine updated PC2; DSP **0.2.3**, both FW banks
  imaged, `0x1FD010 = 00 00 00 00` (verified in §3).
- `docs/switch2/usb-spec.md:150-170` — firmware-version compatibility + `0x0D` overview.
- `STATUS.md` (P2 "Pro Controller 2 update prompt"), `PLAN.md` ("Re-test the 'Update this
  controller' prompt").
- Dycool / NS-PC-Control `PC2_Gyro_*.pcapng` — the 2.0.17 / DSP-0.2.2 source (private; Strong
  Evidence).
