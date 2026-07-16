# Dycool/NS-PC-Control audit (2026-07-12)

> Status: 🔵 Partial. One fix applied and hardware-validated by cross-reference (not yet by our
> own console test). Everything else in this doc is **Strong Evidence** at best — NS-PC-Control's
> own claims are traced to captures the author holds privately (`PC2_Gyro_*.pcapng`,
> `PC2_Write_Amiibo.pcapng`) that are **not bundled in their repository** and could not be
> independently re-verified byte-for-byte the way `ndeadly`'s raw capture was in the previous
> pass. Treat every NS-PC-Control-sourced claim below as "another implementation believes this,"
> not "confirmed on the wire by this project."

## 0. Provenance

- **Repository:** `https://github.com/Dycool/NS-PC-Control`
- **Freshly cloned:** 2026-07-12, ~19:59 UTC, into a scratch location outside the tracked tree
  (not committed anywhere in this repo).
- **Commit:** `a422f4bcf3b0f79568b7583e9eb04db266be0ed0` — "Refactor audio handling to use ALSA
  for UAC1 support", 2026-07-12 02:20:04 +0100. Branch `main`; the `joycon-usb-experiments`
  branch this project's memory previously referenced (PR #9) is now merged into `main` — there is
  no separate branch left to check.
- **Scope:** a full remote-play system (Raspberry Pi USB-gadget server + PC/mobile clients + web
  app + cloud streaming), not a narrow relay. The directly comparable component to this repo is
  `server/src/switch2_native.cpp` (+ `s2_nfc_codec.cpp`, `s2_uac1_audio.cpp`, `bluetooth_manager.cpp`).
  No bundled `.pcap`/capture files were found anywhere in the tree — all protocol claims trace to
  the author's own private captures, cited only by filename in code comments.

## 1. Command-handshake diff — `switch2_native.cpp` vs. `switch_pro2.c`

Both implementations use the identical 8-byte header convention this project already documented
(`[id][dir][transport][sub][unk][len_lo][len_hi][pad]`, `dir=0x91` request / `0x01` response,
ACK `00 F8`) — independent convergent implementation, not surprising since both derive from the
same reverse-engineered wire protocol, but a reassuring structural cross-check.

| Command | NS-PC-Control | PicoSwitch2 | Discrepancy | Confidence | Action |
|---|---|---|---|---|---|
| `0x02/0x04` memory read (general) | Same framing; caps at `0x50` per read like this repo | Identical | None | — | — |
| `0x02/0x04` addr `0x13060` (32 B) | Explicit comment: "reads back erased (0xFF) on the real unit... Captured read: addr=0x13060 len=0x20" — global `0xFF` fill, no explicit override | Was hardcoded `4C 09 00 00` (4 B, no source annotation) | Real | **Strong** (2nd independent source, agrees with `ndeadly`'s raw capture from the previous pass) | **✅ Fixed** — `switch_pro2.c` now explicitly fills this 32-byte span with `0xFF`. See §3 below. |
| `0x02/0x04` addr `0x13100` (24 B, mag+accel bias) | Explicitly populated: 12 zero bytes + 3×float32 `(0.0816, 0.01106, 10.01)` on their reference unit | Was entirely unpopulated (silent zero-fill) until the previous pass added it from this repo's own SPI dump `(0.160, -0.0687, 10.38)` | Per-unit calibration variance (three sources now: this repo's SPI dump, `ndeadly`'s unit ≈`(0.046,-0.085,9.84)`, NS-PC-Control's reference unit ≈`(0.082,0.011,10.01)` — all near-gravity Z, all different exact values) | **Confirmed as a real, previously-missing region** (this repo's own fix, independently corroborated a third time); **not confirmed as any specific value** (expected per-unit spread) | No further action — already fixed; do not chase matching any one unit's exact bytes. |
| `0x02/0x04` addr `0x13040` (16 B, temp+gyro bias) | `16 F4 D3 41 48 CE 85 BA F1 05 71 BA 1F 27 CB 3B` | This repo: `3B E0 D3 41 C6 60 6A BC 4D D7 A2 BB 71 1E DD 37` | Per-unit variance (yet another different value; this repo's own SPI dump gave a *third* different value, `08 C0 D8 41 EF 3F A8 BC EA E6 18 BA 5A A2 56 3B`, per the previous pass) | Expected — do not act | No action; document only. |
| `0x10/0x01` firmware/version info | `02 00 11 02 0C 00 00 00 00 02 02 00` | Originally `01 01 05 02 0C 00 00 00 FF FF FF FF` (confirmed **twice** independently in this repo's capture and `ndeadly`'s raw capture) | Confirmed firmware-generation difference: Pico's captured unit was FW 1.1.5 with no DSP; NS-PC-Control's reference was FW 2.0.17 with DSP 0.2.2 | **Strong Evidence** for the later bytes; their cited `PC2_Gyro_*.pcapng` remains private. Real-console testing later showed that repeating 1.1.5 causes the console to offer an update, then fail because PicoSwitch2 does not implement the controller updater. | **Adopted 2026-07-16 for Pro2 only.** Both Pico version surfaces now report 2.0.17; command `0x10` also reports BT 12.0.0 and DSP 0.2.2. Hardware validation must confirm the update prompt disappears. Do not copy NS-PC-Control's later artificial `2.9.99 / 12.9.9 / 0.9.9` ceiling values. |
| `0x11/0x03` opaque 29-byte block | `01 20 03 00 00 0A E8 1C 3B 79 7D 8B 3A` + **identical 16-byte tail** to this repo's value | `01 C0 03 00 00 E7 D0 1C 3B 79 22 A0 3A` + same 16-byte tail | Only 5 of 29 bytes differ (positions 1,5,6,10,11); the trailing 16 bytes are byte-identical across **three** sources now (this repo, `ndeadly`, NS-PC-Control) | **Confirms this repo's existing comment** ("last 16 B are a constant shared across units") was already correctly scoped — the leading ~13 bytes vary, the tail doesn't. No new finding, just independent confirmation the existing caveat was right. | No action — existing code and comment already correct. |
| `0x03/0x0C`, `0x0A/0x02`, `0x0A/0x08` | Not specially handled — falls to the same generic default-ACK the rest of unknown commands get | Same (confirmed with real bytes from `ndeadly`'s capture in the previous pass) | None — independent convergent design | — | — |
| Memory-write ack style (`0x02/0x05`) | Comment: "The real Pro Controller 2 acks every 0x02 read/write with the ordinary 00 F8 status... including the data-bearing flash replies. The earlier 10 78 override did not match the captured hardware behaviour." | This repo already uses the plain `00 F8` ack uniformly (never implemented a `10 78` form) | None — NS-PC-Control's comment describes *their own* prior bug, already avoided here | — | Confirms this repo never had that mistake; informational only. |

## 2. NFC / Amiibo — the single biggest capability gap found

NS-PC-Control has a **complete, working amiibo read/write emulation** over the native Switch 2
vendor command channel (`server/src/s2_nfc_codec.cpp` + the `fill_nfc_response_payload()` state
machine in `virtual_controller.cpp`, ~250 lines). This repo currently only answers subcommand
`0x0C` (confirmed real, matches `61 12 50 10`) and bare-acks everything else — this project has
**zero real tag-transaction evidence of its own**, a gap already explicitly documented in
`docs/switch2/nfc-protocol-inventory.md`.

**What NS-PC-Control implements (Strong Evidence — their own code, unverifiable captures cited by
filename only: `PC2_Write_Amiibo.pcapng`, `PC2_Gyro_*.pcapng`):**

- **Subcommand `0x03`** — enter NFC scan mode. If a virtual tag is "placed," sets an internal
  status/detail pair (`0x09`/`0x00`) and schedules an HID-state-advance event **~40 ms later**
  (their comment: "the HID NFC state advances about 40 ms after this scan command" in their own
  capture).
- **Subcommand `0x04`** — leave scan mode; conditionally ejects the virtual tag depending on
  whether a write was just committed (keeps the tag "present" across a read-then-write sequence,
  per their comment about the captured write flow's exact command ordering).
- **Subcommand `0x05`** — get status. Returns a 61-byte payload (`STATUS_PAYLOAD_SIZE`): status
  byte + detail byte + the tag's 7-byte UID (extracted from a raw NTAG215 dump per ISO14443
  cascade rules) when a tag is present; `0x07`/`0x41` when absent.
- **Subcommand `0x06`** — begin a read/write operation. Request is validated as a `D0 07 ...`
  descriptor; a **zero UID selects read mode**, a **UID matching the placed tag selects write
  mode**; sets status `0x04`/`0x00` on success, `0x07`/`0x41` on failure.
- **Subcommand `0x15`** — read buffer. Full response is **622 bytes** (`8 header + 622` = 630-byte
  USB transfer, matching their comment "0x15 is a 630-byte transfer"): 63 bytes of metadata
  (including the UID, an NTAG originality signature, and 9 bytes echoed from the preceding `0x06`
  request) + the full 540-byte raw NTAG215 dump + a 19-byte trailer.
- **Subcommand `0x14`** — write buffer, received as six chunks of a 454-byte staging image
  (`D0 07` header + UID + lock-byte state + a page-record count + repeated `(page, length, data)`
  records covering NTAG pages 5-129 only — UID/manufacturer/config pages are rejected as
  out-of-range).
- **Subcommand `0x08`** — commits the staged write image into the persistent tag; status becomes
  `0x05` afterward (per their comment).

**Assessment against DATA.md's evidence discipline:** this is detailed, internally consistent,
and shaped like something built against real captures (specific byte offsets, specific timing
deltas, specific validation rules that would be pointless to invent) — but it is **unverifiable
by this project** without either (a) NS-PC-Control publishing the actual capture files, or
(b) this project independently capturing a real amiibo read/write transaction. Per DATA.md's
explicit instruction ("Do not promote its constants or comments to facts merely because another
implementation uses them"), **none of this has been ported into `switch_pro2.c`.** It is recorded
here as a **structured hypothesis** — a detailed target to test against if this project ever
captures its own real amiibo transaction (the still-open blocker documented in
`nfc-protocol-inventory.md` §7/§8: no console-side capture, no physical amiibo tag exercised in
any capture this project holds).

**Confidence-qualified subcommand table addition** (for `nfc-protocol-inventory.md` §4, values
sourced from NS-PC-Control only, all 🔵 Hypothesis unless independently verified):

| Sub | NS-PC-Control's role | This repo's prior confidence | Now |
|---|---|---|---|
| `0x03` | Enter scan mode | ⬜ Unknown | 🔵 Hypothesis (NS-PC-Control only) |
| `0x04` | Leave scan mode | ⬜ Unknown | 🔵 Hypothesis |
| `0x05` | Get status (61 B) | ⬜ Unknown ("Get status" name only, per `ndeadly`) | 🔵 Hypothesis, with a concrete byte layout now |
| `0x06` | Begin read/write op | ⬜ Unknown ("Unknown", per `ndeadly`) | 🔵 Hypothesis |
| `0x08` | Commit write | ⬜ Unknown ("Write device" name only) | 🔵 Hypothesis |
| `0x14` | Write buffer (454 B staging, 6 chunks) | ⬜ Unknown (name only) | 🔵 Hypothesis, with a concrete framing now |
| `0x15` | Read buffer (622 B) | ⬜ Unknown (fields undocumented per `ndeadly`) | 🔵 Hypothesis, with a concrete framing now |

## 3. Implemented this pass

**`0x13060` fixed to `0xFF`-fill (32 bytes), replacing the unannotated `4C 09 00 00`.** Now backed
by two independent real-capture sources (`ndeadly`'s raw USB capture, decoded in the previous
pass; NS-PC-Control's own factory-table comment). This crosses the evidence bar DATA.md set
("seek another independent unit/capture... before deciding") — the previous pass deliberately left
this unchanged with exactly one source. See `src/switch_pro2/switch_pro2.c` `ns2_factory_init()`.
Both boards build clean.

## 4. Deliberately not changed

- **NFC/amiibo emulation** — not ported (§2). Real feature-sized work with unverifiable source
  evidence; the right next step is *this project's own* capture, not adopting unverified bytes.
- **`0x10/0x01` firmware info, `0x13040`/`0x13100` calibration bytes** — NS-PC-Control's values
  differ from this repo's already-doubly-or-triply-confirmed values; treated as per-unit/per-
  firmware variance, not errors on either side.
- **BlueZ reconnect policy specifics** (§5) — BTstack has no equivalent config-file mechanism;
  porting the *concept* (not the BlueZ-specific keys) needs BTstack-API-level research, out of
  scope for a same-pass low-risk change.
- **UAC1 audio via ALSA** (`s2_uac1_audio.cpp`) — architecturally different use case from this
  project's DualSense-passthrough research (`docs/switch2/audio-passthrough-research.md`):
  NS-PC-Control routes console audio Pi→ALSA→UDP→PC speakers (a real Linux kernel `usb_f_uac1`
  gadget function bridged to a PC client), not console-audio→BT-gamepad's-own-speaker. Confirms
  UAC1 (not UAC2) is the right class version (matches this repo's own audio-stub work), but isn't
  a direct implementation precedent for the DualSense-passthrough feature. Not actionable.

## 5. BT reconnect policy — refined evidence, still not implemented

`server/src/bluetooth_manager.cpp` (Linux, BlueZ) sets, when run as root:

```
[General]   FastConnectable = true
[BR]        PageScanType = 1
            PageScanInterval = 128
            PageScanWindow = 48
[Policy]    ReconnectUUIDs = 00001124-0000-1000-8000-00805f9b34fb,00001812-...
            ReconnectAttempts = 15
            ReconnectIntervals = 1,1,1,2,2,2,4,4,8,8,16,16,32
            AutoEnable = true
```

Plus an application-level **per-device proactive-reconnect cooldown of exactly 5 seconds**
(`RECONNECT_COOLDOWN = std::chrono::seconds(5)`) between retry attempts for a trusted device that
has dropped. This is more precise than this repo's existing summary in
`docs/bluetooth/btstack-implementation.md` (which had the cooldown right at "5s" but not the exact
`main.conf` keys or the backoff schedule). **Updated that doc with the precise values.** None of
this is directly portable — BlueZ's `main.conf` mechanism doesn't exist in BTstack (this project's
stack); the equivalent would need to be built from BTstack's own page-scan/connection-parameter
APIs (`gap_set_scan_parameters`, raw `hci_send_cmd` for page-scan type, BTstack's own reconnect/
bonding callbacks). **Still queued, not started** — this refines the target, it doesn't reduce the
work.

## 6. Wake-from-sleep — refined, still out of scope

`docs/wakeup.md` (NS-PC-Control) describes their *shipped* Switch 2 wake feature precisely: a
setup wizard captures a **real, already-paired Joy-Con 2's own HOME-button BLE advertisement**
(MAC + the full `adv=` payload) into a config file, then replays that exact advertisement via raw
HCI later. Notably **simpler than this project's prior characterization**: no mention of stopping
`bluetoothd`, disabling BlueZ, or using `btmgmt public-addr` to spoof the adapter's own identity —
just a captured MAC + payload replayed via `hci0`. Refines (simplifies) but does not change the
"out of scope" conclusion: the mechanism still fundamentally requires **impersonating a specific,
already-bonded Joy-Con 2's exact identity**, and this project's dongle is not a bonded Joy-Con 2 —
it would need a real Joy-Con 2 to capture from first, same structural blocker as before. Updated
`docs/bluetooth/btstack-implementation.md`'s wake section with the more accurate/simpler
description.

## 7. Recommended next action

Per DATA.md's decision tree: the NS-PC-Control audit **did** materially inform BT reconnect
(refined evidence, §5) but did not itself implement anything portable to BTstack, and its highest-
value new material (amiibo NFC, §2) is unverifiable without primary evidence this project doesn't
have. **The single highest-value next action is the queued BT pairing reliability audit** — trace
this project's own `ns2_bt_host.c`/BTstack reconnect path end-to-end against BTstack's actual
connection/bonding API (not against BlueZ's config keys, which don't apply), identify what a
BTstack-native equivalent of "fast-connectable + per-device cooldown + backoff" would look like,
and implement only what's low-risk (a pure cooldown timer, analogous to this repo's own
`CONTROL_TICK_MS`-driven state machine in `ns2_bt_host.c`, is likely the safest first piece — it
doesn't touch BTstack's actual radio/connection parameters, just when this project's own code
re-attempts a connection).
