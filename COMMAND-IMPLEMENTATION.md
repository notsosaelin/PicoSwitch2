# Console Command Surface — Implementation Audit

> A command-by-command audit of the Switch 2 console-native **vendor bulk command channel**
> (commands `0x01`–`0x18`): which commands genuinely exist, which PicoSwitch2 answers with real /
> structured data, which it replays opaquely, which it stubs with a bare ACK, and where memory reads
> return `0xFF`. Includes theories for the unknown commands and concrete tests, cross-referenced to
> every dump and capture in the repo.
>
> **Documentation only — no code changed.** Companion to
> [`FIRMWARE-IMPLEMENTATION.md`](FIRMWARE-IMPLEMENTATION.md) (command `0x0D`),
> [`NFC-IMPLEMENTATION.md`](NFC-IMPLEMENTATION.md) (command `0x01`), and
> [`BATTERY-OPTIMIZATION.md`](BATTERY-OPTIMIZATION.md) (command `0x0B`).
>
> Evidence base: `src/switch_pro2/switch_pro2.c` (`ns2_dispatch`, the authoritative dispatcher),
> `src/switch_gc/switch_gc.c` + `src/switch_joycon2/switch_joycon2.c` (family mirrors),
> `nso-gc-refs/switch2_controller_research/commands.md` (ndeadly), and the dumps/captures cataloged
> in §7. Confidence: **Confirmed** (byte-verified here) · **Strong Evidence** (external capture) ·
> **Hypothesis** · **Unknown**.

## 0. Headline numbers

- **Command IDs documented to exist (`commands.md`): 24** (`0x01`–`0x18`), of which ndeadly marks
  `0x04`, `0x05`, `0x0E`, `0x12` "unused/possibly unused" → **~20 live** command IDs, each with 1–7
  subcommands.
- **PicoSwitch2 (Pro Controller 2 dispatcher) gives a structured reply to 12** command IDs
  (`0x01 0x02 0x03 0x07 0x09 0x0B 0x0C 0x10 0x11 0x15 0x16 0x18`) and **bare-ACKs the other 12** via
  `default:`.
- Of those 12: **~8 are evidence-backed real** (`0x02 0x03 0x07 0x09 0x0B 0x0C 0x10 0x15`) and
  **~4 are opaque replays** of captured bytes whose meaning we don't know (`0x11 0x16 0x18`, plus the
  single real `0x01/0x0C` reply).
- **The single biggest unhandled command is `0x0D` (firmware update)** — see the dedicated doc.
- **Highest-value unknowns:** `0x17` (request decodes to **48000** — almost certainly an audio
  sample-rate config) and `0x18` — both plausibly the headset-audio subsystem (§6).

## 1. The command protocol (shared by all console-native personalities)

8-byte header + optional data (`commands.md` §"Command Structure"):

| Off | Size | Field | Notes |
|---|---|---|---|
| 0x0 | 1 | Command ID | `0x01`–`0x18` |
| 0x1 | 1 | Direction | `0x91` request (host→dev), `0x01` response (dev→host) |
| 0x2 | 1 | Transport | `0x00` USB, `0x01` Bluetooth |
| 0x3 | 1 | Subcommand ID | |
| 0x4 | 1 | Unknown | |
| 0x5 | 1 | Length (req) / ACK (resp) | |
| 0x6 | 2 | Reserved | `0x0000` |

**ACK form — a resolved detail.** Our dispatchers always emit `r[4]=0x00, r[5]=0xF8` (`00 f8`).
`commands.md` examples mostly show `10 78` because they are **Bluetooth** sniffs (transport `0x01`);
the **USB** capture (`usb-spec.md` table) shows `00 f8`. So **`00 f8` is correct for our USB path**;
`10 78` is the BT-transport ACK. (If a future BT-direct command path is added, it must switch to
`10 78` — currently moot.) — **Strong Evidence.**

**Direction byte (`header[1]`) has *two* bare-ACK forms.** The A/B diff
([`docs/experiments/2026-07-19-usb-command-ab-diff.md`](docs/experiments/2026-07-19-usb-command-ab-diff.md))
confirmed the genuine controller answers some bare ACKs with **dir `0x04`** (NFC `0x01`, **and grip
`0x08/02`**) rather than the usual `0x01`. We handle this for NFC but **not** for `0x08` (it hits the
`default:` dir `0x01`) — a confirmed, still-live shape divergence. The set of commands that use
dir `0x04` is not fully mapped; a console-side capture would complete it.

## 2. The three dispatchers

All three console-native USB personalities carry their own dispatcher; the protocol is identical,
the payloads differ per controller identity:

| Personality | Function | Status |
|---|---|---|
| Pro Controller 2 | `switch_pro2.c:618` `ns2_dispatch` | **Authoritative & hardware-validated in full.** Richest set (real NFC `0x0C`, report `0x05`/`0x09` select, IMU gating via `0x0C/0x04`, `0x15` pairing with wake staging). |
| NSO GameCube | `switch_gc.c:393` `switch_gc_vendor_dispatch` | Structural mirror; hardware-validated for input/rumble. `0x10` reports **1.1.5 / no DSP** (`ff ff ff ff`); NFC bare-ACK only (no `0x0C` reply). |
| Joy-Con 2 (L/R) | `switch_joycon2.c:381` `switch_joycon2_vendor_dispatch` | Structural mirror; report select accepts `0x05` or `0x07`(L)/`0x08`(R). |

Design principle visible in all three: **never answer silently** — every command gets at least an
8-byte envelope (the `default:` bare-ACK), because a genuine controller always responds and a silent
drop stalled the console handshake in early GC testing (`switch_gc.c:588-592`).

## 3. Command-by-command audit (Pro Controller 2 dispatcher)

Legend: ✅ **Real** (structured, evidence-backed) · 🟦 **Opaque replay** (captured bytes, semantics
unknown) · 🟨 **Partial** (some subcommands real, rest stubbed) · ⬜ **Stub** (bare 8-byte ACK, no
data) · 🟥 **Divergent stub** (genuine returns data; we bare-ACK — shape mismatch) · `FF` memory
fill.

| Cmd | Genuine purpose | Our handling | Code | Conf |
|---|---|---|---|---|
| `0x01` | NFC | 🟨 `0x0C`→real `61 12 50 10`; all other subs → bare ACK `dir=0x04` (no tag state machine) | `:762-772` | Confirmed (`0x0C`) / see NFC doc |
| `0x02` | Flash memory read/write/erase | 🟨 `0x04`/`0x01` read → factory window `0x13000-0x13160`, `0x1FA000`→`00`, **`FF` elsewhere**; `0x05` write → ACK, **never persisted**; **`0x03` erase → unhandled (bare ACK)** | `:717-738`, `ns2_mem_read :300` | Confirmed |
| `0x03` | Initialisation | 🟨 `0x0D` Init USB→`01`, `0x03` Enable HID→`01`, `0x0A` Select report (`05`/`09`)→arm stream; **`0x01/02` BT wake/cancel, `0x07/08/09` pairing-info, `0x0C`, `0x0F` → bare ACK** | `:647-656` | Confirmed (handled subs) |
| `0x07` | First-init (first cmd of init) | ✅ `00`, 1 byte | `:657-659` | Strong |
| `0x09` | Player LEDs | ✅ drives physical player LEDs from bitmask | `:690-692` | Confirmed |
| `0x0B` | Battery | 🟨 `0x03` voltage→**fixed** `A5 0E`, `0x04` charge→**fixed** `34 00 83 00`; **`0x06/0x07` → bare ACK** | `:744-747` | Confirmed shape / see Battery doc |
| `0x0C` | Feature select | 🟨 `0x01` get-info (per-bit levels), `0x06` configure (echo id), `0x04` enable→**gates IMU** (`c[8]!=0`); `0x02/0x03/0x05` mask ops → generic 4-byte ACK | `:694-716` | Confirmed |
| `0x10` | Firmware info | ✅ `ns2_firmware_info` (2.0.17 / BT 12.0.0 / DSP 0.2.2) | `:740-742` | see Firmware doc |
| `0x11` | **Unknown** | 🟦 `0x01`→`03`, `0x03`→29-byte captured blob replayed verbatim | `:749-761` | Unknown |
| `0x15` | Bluetooth pairing (over USB) | ✅ `0x01`-`0x04` real AES-128 LTK derivation + wake-pairing staging/commit | `:663-689` | Confirmed |
| `0x16` | **Unknown** | 🟦 24 zero bytes | `:660-662` | Unknown |
| `0x18` | **Unknown** | 🟦 `0x01`→`00 00 40 f0 00 00 60 00`, `0x03`→echo byte | `:773-777` | Unknown |
| `0x04` | Unused (ndeadly) | ⬜ bare ACK | `default` | — |
| `0x05` | Unknown/unused | ⬜ bare ACK | `default` | — |
| `0x06` | Shutdown (`0x02`) / Reboot (`0x03`) | ⬜ bare ACK | `default` | Strong (purpose known) |
| `0x08` | Charging Grip info / **`0x02` enable GL/GR** | 🟥 bare ACK — **and `0x02` uses wrong dir byte**: genuine `08 04 …`, we send `08 01 …` (§ verified by A/B diff) | `default` | Confirmed divergence |
| `0x0A` | Vibration (`0x02` sample, `0x08` data) | ⬜ bare ACK (**rumble uses HID OUT reports, not this command**) | `default` | Strong |
| `0x0D` | **Firmware update** | 🟥 bare ACK (no transport) — **the big gap** | `default` | see Firmware doc |
| `0x0E` | Unused | ⬜ bare ACK | `default` | — |
| `0x0F` | Unknown (**observed real**) | ⬜ bare ACK (sufficient — Joy-Con session proceeds normally) | `default` | Strong (`0x0F/00` seen in mouse capture) |
| `0x12` | Unused | ⬜ bare ACK | `default` | — |
| `0x13` | **Unknown (Joy-Con only)** | ⬜ bare ACK | `default` | Unknown |
| `0x14` | **Unknown** | ⬜ bare ACK | `default` | Unknown |
| `0x17` | **Unknown (audio?)** | ⬜ bare ACK | `default` | Hypothesis (§6) |

### GC / Joy-Con 2 deltas

- **GC** additionally does *not* serve a real NFC `0x0C` reply (bare `dir=0x04`), reports firmware
  **1.1.5** with `ff ff ff ff` DSP, and caps memory reads at `0x28` bytes (buffer size). Same
  unhandled set as Pro2.
- **Joy-Con 2** mirrors GC; report select accepts `0x05`/`0x07`/`0x08`. `0x13` (documented
  "JoyCon-only") is still bare-ACKed — a **candidate real gap** specifically for the Joy-Con
  personalities (§6).

## 4. What "returns `0xFF`" vs "stubbed" actually means

Three distinct behaviors get conflated as "not implemented" — they are different:

1. **`0xFF` memory fill (command `0x02` reads).** `ns2_mem_read` (`switch_pro2.c:303-308`) backs only
   the 0x160-byte factory window at `0x13000` (+ `0x1FA000`→`00`) and returns **`0xFF` for every
   other address**. This is *deliberate* — genuine uninitialised flash reads `0xFF` — and is correct
   for most of the 2 MB map. It is **only a problem for the firmware/DSP regions** the console reads
   to make its update decision (see `FIRMWARE-IMPLEMENTATION.md` §3). Not a "stub"; a faithful blank.
2. **Bare-ACK stub (`default:` and unhandled subcommands).** An 8-byte envelope with **no data**.
   Correct for commands the console tolerates an empty answer to (it kept the handshake alive in
   testing). **Divergent** only where genuine returns a specific payload (`0x08` grip, `0x0D`
   update) — flagged 🟥 above.
3. **Opaque replay.** Real captured bytes emitted verbatim (`0x11`, `0x16`, `0x18`, EP0 info middle
   bytes) without understanding them. Safe as long as the bytes are unit-invariant (verified for the
   `0x11/03` tail and the EP0 per-unit id, per `switch_pro2.c:225,749-750`).

## 5. Subcommand-level gaps inside "handled" commands

Even the ✅/🟨 commands have unhandled subcommands worth cataloguing:

- `0x02/0x03` **erase sector** — unhandled; genuine returns a 4-byte reply. Harmless unless the
  console erases during an interaction we don't currently trigger.
- `0x03/0x01,0x02` **BT wake/cancel**, `0x03/0x07,0x08,0x09` **send/clear/store pairing-info**,
  `0x03/0x0C`, `0x03/0x0F` — bare-ACKed. `0x03/0x07+0x09` is an *alternate* pairing path (bypasses
  `0x15`); we implement `0x15`, so this is redundant over USB but relevant to a future BT-direct path.
- `0x0B/0x06` (`always 0x00000011`), `0x0B/0x07` — bare-ACKed; low value.
- `0x0C/0x02,0x03,0x05` mask set/clear/disable — generic 4-byte ACK rather than the exact genuine
  reply; feature negotiation still works because `0x04` enable is the one we gate on.
- `0x01/*` all NFC subs except `0x0C` — the entire tag state machine (see NFC doc).

## 6. Unknown commands — theories & tests

Each is a concrete, testable hypothesis. "Test" names the specific capture/dump or experiment that
would resolve it.

### `0x17` — **audio sample-rate config (strong hypothesis, corroborated)**
Request example `17 91 01 02 00 07 00 00` + `80 bb 00 00 02 f0 00`. **`80 bb 00 00` (LE) = 0x0000BB80
= 48000** — a textbook 48 kHz sample rate. **Corroborated** by the live headset capture
(`docs/experiments/2026-07-19-usb-command-ab-diff.md` Exp 3): the genuine UAC audio descriptor's
`FORMAT_TYPE.tSamFreq` is **the exact same bytes `80 bb 00` = 48000**, for both the headphones and mic
streams. So `0x17/02` almost certainly configures the audio path sample rate. **Still not directly
captured** — a PC host issues no vendor commands; obtaining the command itself needs the console-init
tool run *with* a headset, or a console interposer. Ties into `DS5-NS2_AUDIO.md`.

### `0x18` — **audio/DSP or analog config (hypothesis)**
`0x18/01` → `00 00 40 f0 00 00 60 00`; `0x18/03` echoes a byte (`07`). The `40 f0` / `60 00` groups
look like little-endian 16-bit values. **Hypothesis:** a config/threshold block, possibly the same
audio/DSP subsystem as `0x17` (they are numerically adjacent and both opaque), or analog
deadzone/threshold parameters. **Test:** capture with/without headset and with different analog
states; mutate the `0x18/03` argument and watch for a streaming behavior change; correlate with
`0x1FB000` (the "unique per controller type" block in `memory_layout.md`).

### `0x11` — **calibration/parameter block (hypothesis)**
`0x11/03` → `01 20 03 00 00 0a e8 1c 3b 79 …`. The repeating `0a e8 9c 4X 58 a0 0b 4X` groups
resemble the packed factory blocks at `0x13080`/`0x130C0` (stick calibration neighborhood).
**Hypothesis:** `0x11` returns a derived stick/motion calibration or device-parameter summary.
**Test:** diff the `0x11/03` bytes against the `0x13080`-region bytes in `dumps/SPI/2069_spi_dump_*`;
capture `0x11` across two physically different controllers to see which bytes are per-unit vs constant.

### `0x16` — **status/reserved (hypothesis)**
Always 24 zero bytes in every capture. **Hypothesis:** a reserved/status query that is non-zero only
in a state we haven't captured (error flags?). **Test:** grep all captures for a non-zero `0x16`
response; if none, treating it as constant-zero is safe.

### `0x13` — Joy-Con-only feature (**mouse-mode hypothesis refuted; still Unknown**)
`commands.md`: "Seems to only be used on JoyCon controllers." **Tested** against all eight decrypted
BLE captures (`docs/experiments/2026-07-19-usb-command-ab-diff.md` Exp 2): **`0x13` appears in none of
them** — not even the mouse-mode session. **Mouse mode is instead enabled by the `0x0C` feature mask
`0x37`** (= standard `0x27` + bit `0x10` "Mouse data"), a declarative feature toggle, *not* `0x13`. So
`0x13`'s purpose is still **Unknown**; it is unobserved in pairing/reconnect/wake/OTA/mouse. **Next
test:** a first-time Joy-Con↔console setup or charging-grip attach capture. Bare-ACK remains correct.

### `0x06` — **shutdown/reboot (purpose known, reply unconfirmed)**
`0x06/02` = shutdown (sent when console sleeps **docked**); `0x06/03` = reboot (after a firmware
update). We bare-ACK both. **Test:** monitor for `0x06/02` on dock-sleep — relevant to
`controller-sleep-research.md`; `0x06/03` is exercised by the `0x0D` capture flow (Firmware doc §7).

### `0x08` — **charging grip (divergent shape)**
Genuine returns 0x20/0x40 factory-like bytes; we bare-ACK. **Hypothesis:** the console queries grip
data during init; for a standalone Pro2 (not in a grip) an empty answer is likely tolerated (we
enumerate fine), but a wrong *shape* could matter in a grip context. **Test:** check
`genuine_procon_2.pcapng` for `0x08`; if absent for a bare Pro2, deprioritise.

### `0x0A` — **vibration command family**
`0x0A/02` plays a predefined sample (e.g. the "search for controllers" tone), `0x0A/08` sends raw
vibration data. We bare-ACK because our rumble arrives via **HID OUT reports**. **Test:** capture
whether the console ever sends `0x0A/02` (connection/low-battery tones); if so, a real controller
would buzz — a realism gap we currently ignore silently.

### `0x04`, `0x05`, `0x0E`, `0x12`, `0x14`, `0x0F`
Marked unused or single-observation. **Test:** grep all captures; only invest if one appears in a
genuine console session.

## 7. Evidence catalog — dumps & captures (what each is good for)

**SPI / flash dumps** (`0x02` memory-read ground truth):
- `dumps/SPI/2069_spi_dump_2026-07-06_2107.bin`, `…_2026-07-10_1422.bin` — genuine **Pro Controller 2**
  (PID 0x2069) full 2 MB. Ground truth for `0x02` reads, factory `0x13000`, firmware/DSP regions
  (used in Firmware doc §3). **Both `0x11`-vs-factory and `0x08` grip theories test against these.**
- `dumps/SWITCH2_JOYCON_L_1.bin`, `…_R_1.bin` — Joy-Con 2 factory data for the Joy-Con dispatcher.
- `dumps/SPI/NSO_GC_SPI_DUMP_1.bin`, `…_2.bin` — NSO GameCube factory data.

**USB command captures** (the authoritative command-channel evidence):
- `usbpcaptures/genuine_procon_2.pcapng` — genuine Pro2 over USB (**PC/Windows** session). The
  primary source for real command shapes and the `00 f8` USB ACK. **Every "does the console send X"
  test starts here** — but note it lacks a console-side amiibo/update interaction (the standing gap).
- `usbpcaptures/picoswitch_2_dongle.pcapng` — **our own dongle's** USB output. **A/B diff against the
  genuine capture is the single most useful validation** — it shows exactly where our replies deviate
  in shape/length/timing. (Done: Exp 1.)
- `usbpcaptures/genuine_procon2_headset_2026-07-19.pcap` — genuine Pro2 **with headset**: full config
  descriptor (UAC audio function, endpoints). `usbpcaptures/genuine_procon2_headset_audio_2026-07-19.pcap`
  — genuine headphones **isochronous OUT** stream (192 B/frame, 48 kHz/16-bit/stereo). (Exp 3.)

**BLE captures** (`nso-gc-refs/switch2_controller_research/captures/`):
- `nrf52840/btle_joycon2_ota_update_{decrypted,encrypted}.pcapng` — **OTA firmware update** over BLE.
  Directly informs command `0x0D` semantics (the `FIRMWARE-IMPLEMENTATION.md` §7 capture design) —
  the only genuine update exchange we hold.
- `nrf52840/btle_joycon2_mouse_mode_{decrypted,encrypted}.pcapng` — resolves the **`0x13`** theory.
- `nrf52840/btle_procon2_pairing_{decrypted,encrypted}.pcapng`, `btle_joycon2_pairing_*` — validate
  `0x15` pairing against decrypted ground truth.
- `nrf52840/btle_procon2_motion_0x000A.pcapng`, `…_0x000E.pcapng` — report `0x09` motion over the two
  GATT handles (motion RE, not command channel).
- `nrf52840/btle_*_reconnect_*`, `btle_*_wake_console_*`, `ubertooth-one/*` — reconnect/wake flows
  (relevant to `0x03` BT-wake subs and `0x06` sleep).
- `dumps/BLE CAPTURE/sw2_capture_2026-07-10_*.ndjson` — this project's own BLE GATT captures (report
  `0x05`, motion variants); genuine battery voltage reference for `0x0B`.

**Note:** the decrypted BLE captures are the richest untapped resource for the command audit — most
`commands.md` examples with `10 78` ACKs came from exactly this kind of BT sniff.

## 8. Recommended experiments (ranked, all documentation/analysis — no firmware change)

1. ✅ **DONE — A/B-diff `picoswitch_2_dongle.pcapng` vs `genuine_procon_2.pcapng`**
   ([`docs/experiments/2026-07-19-usb-command-ab-diff.md`](docs/experiments/2026-07-19-usb-command-ab-diff.md)).
   Result: 8/12 exercised replies byte-identical; found the live `0x08/02` dir-byte divergence and the
   factory `0x00`-vs-`0xFF` padding bug; confirmed the capture is a PC-tool init session that does
   **not** exercise `0x10/0x15/0x0D/0x16/0x17/0x18` or EP0 vendor requests.
2. **Resolve `0x17`/`0x18` from a headset-present capture.** If `genuine_procon_2.pcapng` lacks a
   headset session, this needs one new genuine capture; the payoff is unblocking realistic headset
   audio config (ties into `DS5-NS2_AUDIO.md`).
3. ✅ **DONE — tested `0x13` against the mouse-mode (and 7 other) BLE captures** (Exp 2, same doc).
   Result: `0x13` absent everywhere; mouse mode is the `0x0C` mask `0x37` (`0x10` bit), not `0x13`.
   Also confirmed command `0x0F` is genuinely issued. `0x13` remains Unknown pending a new capture.
4. **Grep all captures for `0x11`, `0x16`, non-zero unknowns**; diff `0x11/03` against the SPI factory
   region to test the calibration hypothesis.
5. **Console-side capture path** (the standing infrastructure gap, `STATUS.md` #5) — would deliver
   genuine `0x0D`, `0x06`, `0x08`, and amiibo `0x01` exchanges in one shot.

## 9. Summary

| Question | Answer |
|---|---|
| How many commands exist? | 24 IDs (`0x01`–`0x18`); ~20 live after removing ndeadly's "unused". |
| How many do we answer with structure? | **12** (Pro2 dispatcher); the other 12 get a bare ACK. |
| How many are evidence-backed real? | **~8** (`0x02 0x03 0x07 0x09 0x0B 0x0C 0x10 0x15`). |
| How many are opaque replays? | **~4** (`0x11 0x16 0x18` + `0x01/0x0C`). |
| What returns `0xFF`? | `0x02` memory reads outside the 0x160-byte factory window — faithful blank-flash, only problematic for firmware/DSP regions. |
| What's stubbed with a bare ACK? | `0x04 0x05 0x06 0x08 0x0A 0x0D 0x0E 0x0F 0x12 0x13 0x14 0x17` + unhandled subs; `0x08`/`0x0D` are the divergent ones. |
| Highest-value unknowns? | `0x17` (48000 → audio sample rate), `0x18` (adjacent, likely audio/DSP), `0x13` (Joy-Con — resolvable from in-repo mouse-mode captures), `0x0D` (firmware, own doc). |

## 10. References

- `src/switch_pro2/switch_pro2.c:618-783` (`ns2_dispatch`), `:300-310` (`ns2_mem_read`).
- `src/switch_gc/switch_gc.c:393-612`, `src/switch_joycon2/switch_joycon2.c:381-545`.
- `nso-gc-refs/switch2_controller_research/commands.md` — the full command reference.
- `nso-gc-refs/switch2_controller_research/memory_layout.md` — `0x02` address map.
- Dumps/captures per §7.
- Companions: `FIRMWARE-IMPLEMENTATION.md` (`0x0D`/`0x10`), `NFC-IMPLEMENTATION.md` (`0x01`),
  `BATTERY-OPTIMIZATION.md` (`0x0B`), `docs/switch2/usb-spec.md` (init command sequence).
