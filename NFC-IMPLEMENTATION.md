# NFC / amiibo — Implementation Design

> Design and feasibility document for NFC (amiibo) support, toward full feature parity with genuine
> Switch Pro Controllers and Joy-Cons. **Documentation only — no code changed.** Builds on the RE
> evidence in [`docs/switch2/nfc-protocol-inventory.md`](docs/switch2/nfc-protocol-inventory.md)
> (protocol facts) rather than re-deriving it; this file is the *how-to-build-it* view.
>
> Status: ⬜ not implemented (beyond an init-handshake stub). **Cannot function on a real Switch
> over USB** (§2) — this is a completeness/realism feature and a foundation for a future wireless
> path (§8).

## 0. The honest reality up front

1. **The Switch does not process NFC from a USB-attached controller.** This is confirmed here, not
   assumed: the only genuine USB capture in the repo (`usbpcaptures/genuine_procon_2.pcapng`) is a
   **PC/Windows** session, and report `0x09` (which carries the NFC-state byte) **never appears** in
   a console context in any capture we have (`nfc-protocol-inventory.md` §2.5). Nintendo gates
   amiibo to the controller's own NFC radio, which the console reaches only wirelessly.
2. **The paired Bluetooth controller has no NFC reader.** A DualSense/Xbox/etc. cannot scan a
   physical amiibo. So NFC support here can only ever be **virtual amiibo** — the dongle *stores*
   NTAG215 dumps and *presents* them on command, exactly like a software amiibo emulator.
3. **Therefore this is not console-testable over USB today.** It is worth building anyway for (a)
   parity/realism of the emulated controller, (b) use with PC tools/games that read controller-NFC,
   and (c) a ready foundation if/when wireless pairing lands (§8).

## 1. Background — amiibo and NFC on Switch controllers

- An **amiibo** is an **NTAG215** NFC tag: **540 bytes**, 135 pages × 4 B. Layout (widely
  documented, e.g. amiitool): UID + lock bytes (pages 0–2), Capability Container (page 3), an
  **AES-128-CTR-encrypted** data section, the unencrypted **figure/model ID** (~bytes `0x54–0x5B`),
  two **HMAC-SHA256 signatures**, and the tag's factory **ECC originality signature**.
- Genuine Pro Controllers / right Joy-Cons carry a **PN7160/PN7161 NFC controller** (datasheet in
  `nso-gc-refs/switch2_controller_research/datasheets/PN7160_PN7161.pdf`). The Left Joy-Con has **no
  NFC** (report `0x07` NFC-state is "Always 0").
- Flow: game requests a scan → controller polls its field → tag detected → controller reads the
  NTAG215 pages → returns them to the console → console decrypts/validates with **its own** keys.

## 2. Genuine protocol surface (summary; full evidence in the inventory doc)

- **Command `0x01` = NFC**, with subcommands (ndeadly + Dycool/NS-PC-Control, confidence-tagged in
  `nfc-protocol-inventory.md` §4):

  | Sub | Meaning (best current understanding) |
  |---|---|
  | `0x03` | Enter NFC scan mode |
  | `0x04` | Leave scan mode (eject virtual tag) |
  | `0x05` | Get status — 61-byte payload: status byte, detail byte, 7-byte UID when a tag is present |
  | `0x06` | Begin read/write — `D0 07 …` request; **zero UID ⇒ read**, matching UID ⇒ write |
  | `0x14` | Write buffer — ~454-byte staging image (`D0 07` hdr + UID + lock + page records, pages 5–129), chunked |
  | `0x08` | Commit staged write; status → `0x05` afterward |
  | `0x15` | Read buffer — ~622-byte payload: 63-B metadata (UID + NTAG originality signature + 9 B echoed from `0x06`) + **540-B raw NTAG215 dump** + trailer |
  | `0x0C` | ✅ capture-confirmed on genuine hardware; response `61 12 50 10` (opaque 4 B, likely NFC-IC id/rev) |

- **NFC-state byte** in the console-native input report (Pro2 report `0x09` offset `0xC`; right
  JoyCon `0x08` offset `0xE`): `0x00`–`0x07`, `0x00 = Idle`. Drives the console's "tag present / read
  in progress" state.

> **Report `0x09` is a shared multi-field container, not an NFC-specific report.** The same Pro
> Controller 2 console-native report carries Counter (0x0), Power Info/battery (0x1), Buttons (0x2),
> sticks (0x5/0x8), **NFC state (0xC)**, Headset Audio State (0xD), and **Motion/gyro data (0xF,
> 0x28 bytes)**. So the NFC-state byte here and the console-side **motion/gyro** field the project's
> `0x09` motion RE targets live in the *same* report — different offsets. This matters for RE
> economy: one genuine console-side `0x09` capture would unblock **both** the NFC-state and the
> motion questions at once (both are gated on the same missing capture — `inventory` §2.5).
- **Timing:** in the (PC) capture, NFC status is queried **once at connection setup**, not polled;
  polling during an actual amiibo interaction is likely but uncaptured (`inventory` §2.4).

> Confidence: `0x0C` and the bare-ack `dir=0x04` shape are **capture-confirmed**; the read/write/
> status/buffer semantics are a **structured hypothesis** from Dycool/NS-PC-Control's private
> captures — solid to build against, but **unvalidated by this project** and impossible to validate
> on-console over USB. A genuine console-side capture of an amiibo tap remains the open RE gap.

## 3. Current PicoSwitch2 state

`switch_pro2.c` (`case 0x01`, ~L762) is an **init-handshake stub**: it replies to sub `0x0C` with
the capture-sourced `61 12 50 10`, and returns a **bare ack** (`dir=0x04`, no data) to other NFC
subcommands so the enumeration/init sequence doesn't stall. The report NFC-state byte is left `0x00`
(Idle). **Nothing else is implemented** — no tag storage, no scan/read/write state machine, no
`0x05/0x06/0x14/0x15` payloads, no state-byte transitions.

## 4. Virtual-amiibo architecture (proposed)

Since there is no physical reader, NFC = a **virtual-tag server**. Components:

1. **Amiibo store.** One or more **540-byte NTAG215 dumps** held in flash and/or uploaded via the
   config-mode web UI. A minimal design is a single "active tag" slot; a fuller design is a small
   library with a selected slot. Reuse the existing config/flash + web CDC plumbing.
2. **Virtual "tap" control.** The web UI (or a BOOTSEL gesture) marks a tag **present/removed** — the
   software equivalent of placing an amiibo on the reader.
3. **NFC command state machine** (drives the report NFC-state byte and the `0x01` responses):
   ```
   Idle(0x00) ──0x03 enter scan──▶ Polling ──virtual tag present──▶ TagPresent
     status 0x05 → UID + present   ──0x06 (zero UID)──▶ Reading
     ──0x15──▶ return 540-B dump + NTAG signature ──▶ Done
     write path: 0x06 (UID) → 0x14 staged image → 0x08 commit → persist dump
     ──0x04 leave / tag removed──▶ Idle
   ```
4. **Report NFC-state byte** transitions (offset `0xC`/`0xE`) mirrored to the state machine so the
   console/host UI reflects "scanning / tag found / reading".
5. **Persistence of writes.** A game writing amiibo save-data (`0x14`→`0x08`) updates the stored
   dump so it survives reconnect/power-cycle.

## 5. Cryptography — what keys are (and are not) needed

**Key point: serving/storing a virtual amiibo needs NO Nintendo keys.**

- **Read:** the console reads the **raw, still-encrypted** NTAG215 pages and performs decryption/
  HMAC validation with **its own** keys. The dongle only has to hand back the exact stored bytes
  (plus the NTAG **originality (ECC) signature**, which dumps include and the console may check via
  a read-signature step). No `key_retail.bin` on the Pico.
- **Write (save-back):** the console **encrypts** the new data and writes it to the tag pages; the
  dongle stores the raw bytes verbatim. Again no keys.
- **Out of scope (needs keys):** *generating*, *editing*, or *forging* amiibo, or regenerating
  signatures. That requires Nintendo's master keys and is explicitly not part of this
  (present-a-dump / store-a-write) design.

This makes a legally-cleaner, key-free virtual-amiibo server viable: the user supplies their own
NTAG215 dumps; the dongle only stores and presents them.

## 6. Input side — where does the tag come from?

No supported *input* controller can source amiibo data live: only genuine Switch Pro/Joy-Cons have
NFC readers, and this project consumes their **button/motion input**, not their NFC subsystem.
Relaying NFC from a paired genuine Switch pad would be exotic and pointless. So the amiibo data
comes **entirely from stored user dumps**, never a live read. (A stretch idea — a real NFC reader IC
wired to the Pico — is possible but far outside scope and defeats the wireless-bridge design.)

## 7. Feature-parity value even without console function

Even un-testable on-console over USB, completing the NFC **command surface** (all `0x01`
subcommands answered with correctly-shaped responses, capability advertised, state byte wired) makes
the emulated controller *claim and behave like* a genuine NFC-capable Pro Controller 2 during
enumeration and status queries. That is real parity/realism, and PC tools/games that read
controller-NFC (the domain Dycool/NS-PC-Control targets) could actually use it.

## 8. The wireless future (the only path to on-console amiibo)

Because the console reaches amiibo only via the controller's **wireless** NFC path, real on-console
amiibo requires the dongle to **pair to the console over BLE as a genuine Switch 2 controller** —
the transport ndeadly documents (headset/NFC data flow over the controller-specific GATT
characteristics; `bluetooth_interface.md`). That wireless-pairing capability **does not exist in
this project today** and is a large piece of work in its own right. The virtual-amiibo core (§4–§5)
is transport-agnostic, so building it now means only the *transport shim* remains when/if wireless
lands.

## 9. Phased plan (design order; only Phase 0 exists)

- **Phase 0 — done:** init-handshake stub (`0x0C` reply + bare acks); state byte Idle.
- **Phase 1 — command surface / realism:** implement all `0x01` subcommand responses with correct
  shapes (per §2 / NS-PC-Control), advertise NFC capability, wire the report NFC-state byte. No real
  tag yet. Testable for *shape/enumeration*, not amiibo behavior.
- **Phase 2 — virtual-tag read:** amiibo store + web upload + present/remove control + the read state
  machine (`0x03/0x05/0x06/0x15`) returning a stored 540-B dump. Validate against a **PC** tool that
  reads controller-NFC, not the console.
- **Phase 3 — write-back:** `0x14/0x08` staging + commit + persistence for game amiibo saves.
- **Phase 4 — wireless (blocked):** BLE controller-pairing transport so the real console actually
  drives the NFC state machine. Gated on the wireless-pairing feature (§8).

## 10. Risks & open questions

- **No on-console validation over USB** — the whole feature is unfalsifiable on real hardware until
  wireless (§8); every read/write detail is a hypothesis until a genuine console-side amiibo capture
  exists (the standing RE gap, `inventory` §2.5/§4).
- **Subcommand semantics** (`0x01/0x02/0x03/0x04/0x05/0x06/0x08/0x14/0x15`) are mostly
  hypothesis-grade (Dycool/NS-PC-Control, private captures). Multi-chunk framing, checksums, and
  operation timing are unconfirmed.
- **NTAG originality signature / read-signature check** — whether the console verifies it and how it
  must be presented is untested here.
- **Write-back re-encryption** is owned by the console; storing raw bytes should suffice but is
  unvalidated.
- **Effort vs. payoff** — a bonus feature with no on-console function until a separate, large
  wireless project exists; worth staging the transport-agnostic core, not rushing the whole chain.

## 11. References

- `docs/switch2/nfc-protocol-inventory.md` — this project's capture-derived NFC RE (evidence base).
- `nso-gc-refs/switch2_controller_research/{commands,hid_reports,bluetooth_interface}.md` @ `d1c5a7f`
  — Command `0x01` table, NFC-state byte, wireless GATT NFC characteristics.
- `datasheets/PN7160_PN7161.pdf` — the controllers' NFC controller IC.
- Dycool/NS-PC-Control — external virtual-amiibo reference for the `0x01` subcommand semantics
  (private captures; treat as structured hypothesis — `inventory` §4).
- amiitool / NTAG215 amiibo layout — the 540-byte tag format and encryption model (general
  amiibo documentation; keys **not** needed for serve/store per §5).
- `src/switch_pro2/switch_pro2.c` (`case 0x01`, ~L762) — the current stub.
