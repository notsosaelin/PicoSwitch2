# Controller Firmware Versioning — Implementation & Analysis

> Comprehensive breakdown of how the Switch 2 queries a Pro Controller 2's firmware version, how
> PicoSwitch2 answers those queries, why the console shows an **"update available → update failed"**
> prompt, whether the update image can be *read/captured* or *applied*, and exactly how to change the
> dongle's **reported** version to match the latest genuine controller firmware.
>
> **Documentation only — no code changed by this file.** It is the durable reference behind the
> `STATUS.md` P2 item *"Pro Controller 2 update prompt"* and the `usb-spec.md` §"Firmware-version
> compatibility" note. Evidence base: `src/switch_pro2/switch_pro2.c`, `docs/switch2/usb-spec.md`,
> and the NS-PC-Control / Dycool `PC2_Gyro_*.pcapng` reference (privately held → Strong Evidence,
> not byte-verified in this repo).

## 0. The honest reality up front

1. **We only ever report a version string; we never run Nintendo firmware.** The dongle is a
   RP2040/RP2350 emulating a controller. The genuine update image targets Nintendo's controller MCU
   and **physically cannot be flashed onto the Pico** (§5). So "firmware support" here means: answer
   the console's version queries convincingly, and — optionally, as a research feature — *capture*
   the encrypted update stream for offline analysis rather than pretend to apply it.
2. **The update prompt is a version comparison, not a health check.** The console offers an update
   whenever the version we report is **older than the newest Pro Controller 2 firmware the console
   knows about**. It is cosmetic/nagging, not a functional failure — the controller keeps working
   either way (confirmed by the user and consistent with the optional nature of the update).
3. **The prompt fails because we do not implement the `0x0D` update transport** (§4). The console
   starts the transfer, we bare-ACK it instead of running the multi-chunk CRC32/bank-switch protocol,
   and the update aborts. That is *expected* today — we deliberately do not accept Nintendo firmware
   writes (`usb-spec.md:167-170`).
4. **The clean fix for the nag is to raise the reported version to the current latest** (§6). That is
   a **one-block edit** to named constants that feed every version surface. The open question is only
   *what the current latest value is* — an evidence gap, not a code gap (§7).

## 1. Where firmware version is reported — the two surfaces

The console reads the controller version in **two** places during bring-up, and PicoSwitch2 drives
**both from one set of named constants** (`switch_pro2.c:209-217`) so they can never drift apart:

```c
// switch_pro2.c:209-217  — single source of truth for every version surface
#define NS2_PRO_FW_MAJOR 0x02   // controller firmware  major
#define NS2_PRO_FW_MINOR 0x00   //                      minor
#define NS2_PRO_FW_MICRO 0x11   //                      micro  (0x11 = 17 decimal)  -> "2.0.17"
#define NS2_PRO_BT_MAJOR 0x0C   // Bluetooth patch       major (0x0C = 12)          -> "12.0.0"
#define NS2_PRO_BT_MINOR 0x00
#define NS2_PRO_BT_MICRO 0x00
#define NS2_PRO_DSP_MAJOR 0x00  // DSP (haptics) firmware major                      -> "0.2.2"
#define NS2_PRO_DSP_MINOR 0x02
#define NS2_PRO_DSP_MICRO 0x02
```

### Surface A — EP0 vendor control request `0x02` (enumeration, 16 bytes)

Read over **endpoint 0** during USB enumeration, *before* the console will touch the bulk command
channel (`tud_vendor_control_xfer_cb`, `switch_pro2.c:1098`). Layout of `ns2_ctrl_info`
(`switch_pro2.c:228-230`):

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | **Controller firmware** major/minor/micro | `02 00 11` → 2.0.17 |
| 3 | 3 | reserved (0) | `00 00 00` |
| 6 | 1 | **Bluetooth patch major** | `0C` → 12 |
| 7 | 3 | reserved (0) | `00 00 00` |
| 10 | 6 | Per-unit **BD_ADDR** (controller Bluetooth address) | `9E 2B AB AB A9 3C` |

Note this block carries the FW triplet + BT *major only*, then the controller's own Bluetooth
address — the same 6 bytes returned by the `0x15/01` pairing exchange (`switch_pro2.c:667`), so the
advertised identity is self-consistent. **No DSP version here.**

### Surface B — bulk command `0x10/0x01` "firmware info" (init, 12 bytes)

Sent on the vendor bulk channel during controller init (dispatcher `case 0x10`,
`switch_pro2.c:740-742`). Layout of `ns2_firmware_info` (`switch_pro2.c:234-237`):

| Offset | Bytes | Field | Current value |
|---|---|---|---|
| 0 | 3 | **Controller firmware** major/minor/micro | `02 00 11` → 2.0.17 |
| 3 | 1 | **Controller type** (`0x02` = Pro Controller) | `02` |
| 4 | 3 | **Bluetooth patch** major/minor/micro | `0C 00 00` → 12.0.0 |
| 7 | 1 | pad | `00` |
| 8 | 3 | **DSP firmware** major/minor/micro | `00 02 02` → 0.2.2 |
| 11 | 1 | pad | `00` |

This is the **full** version surface: firmware + type + BT patch + DSP. The on-wire capture form is
`10 01 00 01 00 f8 00 00` + these 12 bytes (`usb-spec.md:161-162`).

### Byte-encoding rule (important when editing)

Each component byte's **hex value equals the decimal version number.** `0x11` = decimal **17**, so
`02 00 11` reads as **"2.0.17"**, not "2.0.0x11". `0x0C` = **12** → "12.x". To report, say, **3.0.0**
you set `03 00 00`; to report **2.1.0** you set `02 01 00`. A component ≥ 10 needs its hex form
(e.g. version `.20` = `0x14`). Get this wrong and you will silently report a different number than
you intend.

## 2. What we report today, and where it came from

| Surface | Value | Source of the bytes |
|---|---|---|
| Controller firmware | **2.0.17** | Dycool `PC2_Gyro_*.pcapng` — an **updated retail** PC2 |
| Controller type | 0x02 (Pro) | Constant across all PC2 captures |
| Bluetooth patch | **12.0.0** | Same across old + new captures |
| DSP firmware | **0.2.2** | Present in the updated capture; **absent** (no DSP) in the old one |

The repo's own *bundled* USB capture is an **older** genuine PC2 running **1.1.5 / BT 12.0.0 / no
DSP** (`usb-spec.md:152-153`, `133`). Reporting those older bytes reliably triggered the console's
update offer, which is precisely why the constants were advanced to the later `2.0.17` set — an
attempt to sit at-or-above the console's known-latest and suppress the prompt.

## 3. Why the update prompt still appears (root cause)

The console keeps a notion of the **newest** Pro Controller 2 firmware (baked into its system
firmware and/or fetched from Nintendo). On connect it compares that against **Surface A/B**:

```
reported_version  <  console_known_latest   →  "Update available"  (nag shown)
reported_version  >= console_known_latest    →  no prompt
```

The user still sees the prompt while we report **2.0.17**. That means **2.0.17 is now behind the
current console-known latest** — Nintendo has shipped newer Pro Controller 2 firmware since that
capture was taken. This is **not a bug in our handshake**; it is a stale version number. The fix is
§6 (raise the number). Nothing about the *format* is wrong — enumeration and init both complete, the
controller streams input normally, which is why everything works despite the nag.

> **Confidence:** the comparison-drives-the-prompt model is Strong Evidence (behavioral + the
> documented 1.1.5→prompt / 2.0.17→intended-suppression history). The **exact current-latest value**
> that would fully suppress it is **Unknown** and is the one missing datum (§7).

## 4. The `0x0D` update transport — and why "update failed"

When the user accepts (or the console auto-attempts) the update, the console runs its firmware-update
protocol. From `usb-spec.md:167-170`:

- Transfers an **≈240 KiB** controller image.
- Verifies it with **CRC32**.
- Switches **failsafe banks** (A/B) and **reboots** the controller into the new image.

The command id the genuine flow uses for this transfer is distinct from the version *query*
(`0x10`). **PicoSwitch2 has no dedicated handler for the update-transfer command** — the dispatcher's
`switch (id)` (`switch_pro2.c:646-781`) has cases for `0x01/0x02/0x03/0x07/0x09/0x0B/0x0C/0x10/0x11/
0x15/0x16/0x18` and a `default:` that returns a **bare ACK** (`switch_pro2.c:778-780`). So the update
command hits `default`, we ACK a header with no payload, the console never receives the chunked
transfer/CRC handshake it expects, and the operation **times out / aborts → "update failed."**

> Do not confuse this with dispatcher `case 0x03` **sub** `0x0D` ("Init USB", `switch_pro2.c:648`) —
> that is subcommand `0x0D` of the init command, unrelated to the top-level update transport.

This failure is **by design today**: we intentionally "report the compatible retail version instead
of pretending to accept or persist Nintendo firmware writes" (`usb-spec.md:169-170`). The cost of
that choice is the visible nag.

## 5. Can we read / apply firmware updates?

### 5a. Apply — **No (hard hardware limit).**

The image is compiled for **Nintendo's controller MCU**, not an RP2040/RP2350. It cannot be executed
or flashed on the Pico under any circumstance (`usb-spec.md:168-169`). There is no path to "really
update," and we must never write it into our own flash banks. This is a permanent constraint, not a
missing feature.

### 5b. Capture for offline analysis — **Feasible, not implemented (research opportunity).**

We cannot *run* the image, but we **could accept the transfer purely to capture the encrypted
bytes** for later reverse engineering — turning the dongle into a firmware-image tap. This directly
serves the project's RE mission and is currently **⬜ not implemented**. Design sketch:

1. **Add a `case 0x0D`** (or whichever top-level id the transfer uses — confirm from a genuine
   console capture first, §7) that implements the transfer *state machine* enough to keep the console
   sending chunks: ACK the "begin update" with the shape the console expects, accept each data chunk,
   acknowledge offsets/CRC progress.
2. **Sink the chunks** to (a) a reserved flash region, and/or (b) stream them out over the **config
   CDC channel** to the host PC (reuse the existing web/CDC plumbing) so a full ≈240 KiB image can be
   reassembled off-device without consuming scarce flash.
3. **Terminate cleanly** — either report success (controller "reboots", i.e. we re-enumerate at the
   *new* version we now advertise) or report a benign failure, whichever avoids a retry storm. Which
   ending the console tolerates without re-nagging is itself an experiment.
4. **Analyze offline:** the captured image is almost certainly **signed and/or encrypted** for the
   Nintendo MCU. Capturing it enables studying header/bank/CRC structure and versioning, but decrypt/
   forge is out of scope and needs keys we do not have.

> **Value vs. cost:** this is a genuine reverse-engineering deliverable (first open capture of the
> PC2 update image) but it is a **multi-stage protocol RE task** gated on a **console-side capture**
> of a real update attempt (we have none — the bundled capture is a PC/Windows session). Recommended
> as a **future experiment** under `/docs/experiments`, not a quick fix.

### 5c. The pragmatic answer to the user's actual annoyance

For *"make the pop-up stop"*, capturing/applying the image is the wrong tool. The right tool is §6:
**report a version ≥ the console's current latest.** That removes the offer entirely, so no transfer
is ever attempted and there is nothing to fail.

## 6. How to change the reported version (the actionable fix)

**One edit, one block, both surfaces.** Because Surfaces A and B both read the `NS2_PRO_*` constants,
changing them updates enumeration *and* init together — they cannot desync.

**Steps:**

1. Open `src/switch_pro2/switch_pro2.c`, lines **209-217**.
2. Set the controller-firmware triplet to the target version (remember §1's decimal-in-hex rule):
   ```c
   #define NS2_PRO_FW_MAJOR 0x02   // e.g. bump to 0x03 for a 3.x line
   #define NS2_PRO_FW_MINOR 0x00
   #define NS2_PRO_FW_MICRO 0x11   // 0x11 = 17; raise to the current latest micro
   ```
   Adjust `NS2_PRO_BT_*` and `NS2_PRO_DSP_*` too **only if** the target genuine version also advanced
   those (the BT patch and DSP have been stable at 12.0.0 / 0.2.2 in captures so far).
3. **Clean rebuild** (mandatory after any change like this — see the clean-build-after-revert
   lesson): `./build.ps1 pico2_w -Clean` (and `pico_w` if shipping both).
4. Flash and reconnect a controller on the Switch 2. If the prompt is gone, the reported version now
   meets/exceeds the console's latest. If it persists, the true latest is **higher** than what you
   set — raise it further (§7 on how to find the real number).

**Design note (optional improvement, not done):** these are compile-time constants. If retuning the
version to chase Nintendo releases becomes frequent, a higher-value change is to source the triplet
from **config/flash** (like `body_color` at `switch_pro2.c:257-258`) and expose it in the **config
web UI**, so the reported version is editable **without recompiling**. That matches the user's
stated desire to "update and change the dongle's reported versioning." Recommended as the next
concrete code step if the nag recurs across Nintendo updates.

## 7. The evidence gap — what "latest" actually is

We can *set* any version trivially; we do **not** authoritatively know the **current** genuine Pro
Controller 2 firmware number, because:

- The repo's captures top out at **2.0.17** (privately-held Dycool capture; not byte-verified here).
- The console's known-latest advances whenever Nintendo ships controller firmware, independent of
  this repo.

**Ways to obtain the real target number (in rough order of reliability):**

1. **Read it off a genuine, fully-updated Pro Controller 2** over USB with a capture tool — the
   `0x10/0x01` reply and EP0 `0x02` block contain it verbatim. This is the gold standard and would
   also unblock the `0x0D` capture work (§5b).
2. **Empirical bisection on-console:** bump `NS2_PRO_FW_MICRO`/`MAJOR` upward, clean-build, reconnect,
   and observe when the prompt disappears. The lowest value that suppresses it is at/above the
   console's latest. Cheap; needs only the dongle + a Switch 2.
3. **Cross-reference** NS-PC-Control / community captures for any newer `PC2_Gyro`-style dump.

Until (1) or (2) is done, treat the exact suppressing value as **Unknown**; everything about the
*mechanism* (§1–§4) is Strong Evidence or Confirmed.

## 8. Risks & open questions

- **Over-reporting has no known downside but is unverified.** Reporting a version *higher* than any
  real controller should still read as "up to date" (comparison is `reported >= latest`), but a
  console that special-cases unknown-future versions is an untested edge. Prefer matching the real
  latest once known (§7-1) over an arbitrarily huge number.
- **DSP/BT desync.** If a future genuine firmware bumps the DSP or BT patch alongside the controller
  triplet, reporting the new controller version but the *old* DSP/BT could itself trigger a prompt.
  Advance all three from the same capture, not just the headline number.
- **`0x0D` transport shape is uncaptured here.** The ≈240 KiB/CRC32/bank-switch description is from
  `usb-spec.md` (Strong Evidence); the exact command id, chunk framing, and ACK shapes needed for the
  §5b capture path require a genuine console-side update capture we do not yet have.
- **No console-side capture at all.** The bundled USB capture is a PC/Windows session; the update
  offer and its transport have never been recorded in this repo. This is the same standing gap that
  blocks NFC (`nfc-protocol-inventory.md` §2.5) and report-`0x09` motion — a reproducible
  console-side capture path (`STATUS.md` next-steps #5) would unblock all three.

## 9. Summary

| Question | Answer |
|---|---|
| How does the Switch 2 query firmware? | EP0 vendor req `0x02` (16 B, FW+BT-major+BD_ADDR) **and** bulk cmd `0x10/01` (12 B, FW+type+BT+DSP). |
| How do we report it? | Both surfaces driven from `NS2_PRO_*` constants (`switch_pro2.c:209-217`); currently **2.0.17 / BT 12.0.0 / DSP 0.2.2**. |
| Why differ / why the prompt? | Our reported version is **older than the console's current latest**; the console offers an update. |
| Why does the update *fail*? | We have **no `0x0D` update-transport handler** — it bare-ACKs and aborts (by design; we never accept Nintendo firmware writes). |
| Can we apply an update? | **No** — image targets Nintendo's MCU, unflashable on RP2040/RP2350. |
| Can we *read/capture* one? | **Feasible but unimplemented** — a `0x0D` sink that logs chunks to flash/CDC for offline RE; gated on a console-side capture. |
| Can I change the reported version? | **Yes, trivially** — edit the constants + clean rebuild (§6); or (better) make it config/web-editable. Only the *target latest value* is an evidence gap (§7). |

## 10. References

- `src/switch_pro2/switch_pro2.c:204-237` — version constants + both report blocks.
- `src/switch_pro2/switch_pro2.c:740-742` (cmd `0x10`), `:1098-1103` (EP0 req `0x02`), `:646-781`
  (dispatcher + `default:` bare-ACK that swallows the update command).
- `docs/switch2/usb-spec.md:150-170` — firmware-version compatibility + `0x0D` update protocol.
- `STATUS.md` — P2 item "Pro Controller 2 update prompt" (🟡, re-test / isolate firmware gating).
- `PLAN.md` — "Re-test the Switch 2 'Update this controller' prompt after USB audio is healthy."
- Dycool / NS-PC-Control `PC2_Gyro_*.pcapng` — the 2.0.17/DSP-0.2.2 source (private; Strong Evidence).
- The clean-build-after-revert lesson — always `-Clean` after changing these constants.
