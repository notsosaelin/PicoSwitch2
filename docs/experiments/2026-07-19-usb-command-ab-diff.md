# Experiment — capture analyses: USB command A/B diff, and Joy-Con mouse mode / command `0x13`

- **Date:** 2026-07-19
- **Author:** repository maintenance pass (analysis of existing captures; **no code modified**)
- **Related:** [`command-surface.md`](../switch2/command-surface.md),
  [`firmware-versioning.md`](../switch2/firmware-versioning.md),
  [`docs/switch2/nfc-protocol-inventory.md`](../switch2/nfc-protocol-inventory.md) §2.3,
  [`docs/switch2/usb-spec.md`](../switch2/usb-spec.md)

Two analyses of existing dumps/captures: **(1)** a USB command A/B diff of the genuine Pro Controller 2
vs the dongle, and **(2)** a BLE analysis resolving whether command `0x13` drives Joy-Con mouse mode.

---

# Experiment 1 — USB command A/B diff: genuine Pro Controller 2 vs PicoSwitch2 dongle

## Question

For every command the host sends to a genuine Pro Controller 2, does PicoSwitch2's reply match
**byte-for-byte**? Where it diverges, is the divergence still present in current firmware, and does it
matter?

## Hypothesis

The dongle's replies are byte-identical for the init/streaming command set, with divergences confined
to (a) legitimately per-unit fields (serial), and (b) at most a few small shape bugs.

## Method

Both captures are **USBPcap** (`LINKTYPE_USBPCAP` = 249) recordings on a Windows host. A parser
(throwaway, in the job tmp dir — not committed) extracted every **bulk** transfer (the vendor command
channel: EP `0x02` OUT = host→device requests, EP `0x82` IN = device→host responses), decoded the
8-byte command header (`id, dir, transport, sub, …`), grouped by `(cmd, sub)`, and diffed the full
response bytes between the two files. HID input reports on the interrupt endpoint (`0x81`) and EP0
control traffic were excluded from the command diff. Findings were then re-verified against the
**current** `src/switch_pro2/switch_pro2.c` (the captures are dated 2026-07-10; some fixes postdate
them).

> Note: `tshark` was not on the analysis shell's PATH, so a self-contained Python pcapng/USBPcap
> parser was used instead; results below are reproducible from the two capture files alone.

## Environment / Captures

| File | Size | Role |
|---|---|---|
| `usbpcaptures/genuine_procon_2.pcapng` | 11.6 MB, 164,242 blocks | Genuine Pro Controller 2 (serial `HEW70001504982`) |
| `usbpcaptures/picoswitch_2_dongle.pcapng` | 10.4 MB, 144,347 blocks | PicoSwitch2 dongle (serial `HEJ7100112147`) |

**Nature of the sessions (important scoping fact).** These are **PC-host** captures in which a
Windows tool drives a *console-style* controller-init handshake over the bulk channel. They are **not**
recordings of a live Nintendo console. Evidence: identical, scripted 12-command request sets to both
devices; **no** `0x15` pairing, **no** `0x10` firmware-info, **no** `0x0D` update, and **no** EP0
vendor-class requests (`0xC0/0x02`, `0xC0/0x03`, `0x40/0x04`) appear in either file. This reconciles
with the NFC inventory calling `genuine_procon_2.pcapng` a "PC/Windows session." The session selects
**input report `0x05`** (`03 91 00 0a … 05`) and streams it — so this validates the init + report-0x05
streaming path, and cannot speak to pairing/firmware/update.

## Results

### The host sent an identical 12-command request set to both devices

`0x01/01`, `0x01/0C`, `0x02/01` (×7 memory reads), `0x03/0A` (select report 0x05), `0x03/0D`
(init USB), `0x07/01`, `0x08/02`, `0x09/07`, `0x0A/08`, `0x0C/02`, `0x0C/04`, `0x11/01`. Because the
stimulus is identical, every response difference below is a genuine device difference.

### Response comparison

| (cmd/sub) | Result | Note |
|---|---|---|
| `0x01/0C` | ✅ identical | `…61 12 50 10` NFC id |
| `0x03/0A` | ✅ identical | select report ack |
| `0x03/0D` | ✅ identical | `…01 00 00 00` init-USB ack |
| `0x07/01` | ✅ identical | `…00` first-init |
| `0x09/07` | ✅ identical | player-LED ack |
| `0x0A/08` | ✅ identical | vibration-data ack |
| `0x0C/02` | ✅ identical | set-feature-mask ack |
| `0x0C/04` | ✅ identical | enable-features ack |
| `0x01/01` | ⚠️ differ in capture | **already fixed in current code** — see below |
| `0x08/02` | ❌ **differ (live)** | ACK direction byte — see below |
| `0x02/01` | ◐ differ (expected + minor) | serial (per-unit) + padding byte — see below |
| `0x11/01` | — inconclusive | genuine response not cleanly captured (see below) |

**8 of 12 responses are byte-identical.** This is strong validation of the init/streaming replies.

### Divergence 1 — `0x08/02` ACK direction byte (**live divergence**)

```
genuine: 08 04 00 02 00 f8 00 00     (dir byte = 0x04)
dongle : 08 01 00 02 00 f8 00 00     (dir byte = 0x01)
```

`0x08/02` is **Enable Charging Grip Buttons** (the GL/GR enable, per `commands.md`). The genuine
controller answers a bare ACK with **dir `0x04`**, the same "bare-ack" shape it uses for NFC. Current
firmware has **no `case 0x08`**, so it falls through to `default:`, which leaves `r[1] = 0x01`
(`switch_pro2.c:638`). **This divergence is present in current code.**

Notably, the code *already knows*: the comment at `switch_pro2.c:766-768` says "the same
dir=0x04-on-bare-ack shape recurs on an unrelated cmd=0x08 response in the same window" — but only the
NFC `0x01` path was given `dir=0x04`; `0x08` was not. So this is a **documented-but-unfixed** shape
bug. Functional impact is probably low (the console accepts the dongle and GL/GR paddles work on
hardware), but it is a real indistinguishability gap and an easy, well-evidenced fix.

### Divergence 2 — `0x01/01` ACK direction byte (**already resolved**)

```
genuine        : 01 04 00 01 00 f8 00 00     (dir 0x04)
dongle (Jul-10): 01 01 00 01 00 f8 00 00     (dir 0x01)
current code   : dir 0x04  (switch_pro2.c:769)  ✓ matches genuine
```

The 2026-07-10 dongle capture predates the NFC dir=`0x04` fix. **Current firmware matches genuine** —
no action needed. Documents that the capture is stale for this command.

### Divergence 3 — `0x02/01` memory read (**expected + one minor live divergence**)

```
genuine: …40000000003001000100 484557373030303135303439383200 007e0569200106012323 23a0a0a0e6e6e6323232 ffffffffffffffffffffff
dongle : …40000000003001000100 48454a373130303131323132343700 007e0569200106012323 23a0a0a0e6e6e6323232 0000000000000000000000
```

- **Serial differs** — `HEW70001504982` vs `HEJ7100112147`. Expected: per-unit, and ours is a fixed
  constant (`switch_pro2.c:251`).
- **Identity fields are identical** — VID `05 7e`, PID `20 69`, `01 06 01`, body `23 23 23`, buttons
  `a0 a0 a0`, highlight `e6 e6 e6`, grip `32 32 32`. Good.
- **Padding byte differs (live):** the unpopulated tail of the factory block reads **`0xFF`** on
  genuine (uninitialised flash) but **`0x00`** on the dongle, because `ns2_factory_init` does
  `memset(factory, 0, …)` (`switch_pro2.c:249`). Inside the `0x13000` window our unset bytes are `00`;
  genuine flash is `FF`. (Note `ns2_mem_read` already returns `0xFF` *outside* the window, so this is
  an internal inconsistency — the fill byte should be `0xFF` for faithful blank-flash behavior.)
  Low impact, but a real fidelity divergence, and it rhymes with the firmware-region `0xFF` finding in
  `firmware-versioning.md` §3.

### `0x11/01` — inconclusive

The dongle replied `11 01 00 01 00 f8 00 00 03 00 00 00` (data `03`, the documented USB form). The
genuine controller's `0x11/01` response did not parse as a clean command frame in this capture (it
appears among fragmented all-`0xFF`/all-`0x00` bulk frames — likely a split-transfer artifact). No
conclusion; `commands.md` gives the BT-form ground truth (`…01 00 00 00`).

### Non-command artifacts

Both files contain a handful of all-`0xFF` and all-`0x00` bulk frames (`0xff/0xff`, `0x00/0x00`,
stray `0x5f`/`0x62` leading bytes). These are USBPcap fragments of split bulk transfers, not commands,
and were excluded from the diff.

## Conclusion

- PicoSwitch2's command replies are **byte-identical to genuine for 8/12** exercised commands, and the
  init + report-`0x05` streaming path reproduces faithfully.
- **One live, actionable shape bug:** `0x08/02` should ACK with **dir `0x04`**, not `0x01` (the fix
  the code comment already anticipated but didn't apply to `0x08`).
- **One minor live fidelity bug:** factory-window padding should be `0xFF`, not `0x00`.
- One earlier divergence (`0x01/01` dir byte) is **already fixed** in current firmware; the capture is
  simply stale for it.
- The remaining identity divergence (serial) is legitimate per-unit data.

## Remaining questions / future work

- **This capture cannot validate the firmware/update/pairing paths** (`0x10`, `0x15`, `0x0D`, EP0
  vendor requests, `0x16`, `0x17`, `0x18`, `0x0B`) — the PC tool never sends them. A **console-side
  capture** remains the only way to A/B those, and would also settle the `0x08` question in a real
  grip context. (Same standing gap as NFC/motion — `STATUS.md` next-steps #5.)
- **`0x17`/`0x18` were not exercised here** — the audio-config hypotheses in
  `command-surface.md` §6 still need a headset-present capture.
- **Recommended (separate, deliberate) fixes when code changes resume:** give `0x08` (and any other
  genuinely-observed bare-ack command) `dir=0x04`; change the factory fill byte to `0xFF`. Both are
  small, evidence-backed, and improve indistinguishability. Not applied in this pass (documentation
  only).

---

# Experiment 2 — Does command `0x13` drive Joy-Con mouse mode?

## Question

`commands.md` marks command `0x13` "Unknown. Seems to only be used on JoyCon controllers," and
[`command-surface.md`](../switch2/command-surface.md) §6 hypothesised it toggles a Joy-Con-only
feature such as **mouse mode**. Does the genuine Joy-Con 2 use command `0x13` to enter mouse mode?

## Hypothesis

Entering mouse mode issues a `0x13` command (the JoyCon-only command), which is why we've never needed
it for the Pro2/GC personalities.

## Method

Byte-scanned every **decrypted** nRF52840 BLE capture in
`nso-gc-refs/switch2_controller_research/captures/nrf52840/` for command frames (`id ∈ 0x01–0x18`,
`dir ∈ {0x91,0x01,0x04}`, `transport ∈ {0x00,0x01}`, reserved bytes zero). Command frames ride inside
ATT writes/notifications on the BLE command characteristic; the 8-byte header appears contiguously in
the value, so a byte scan of decrypted payloads reliably recovers them. Then extracted the full,
timestamped **request** stream (`dir=0x91`, distinctive/low-false-positive) from the mouse-mode
session specifically. **No code modified.**

## Captures (all in-repo, decrypted)

`btle_joycon2_mouse_mode_decrypted.pcapng` (10,817 pkts, primary), plus
`btle_joycon2_{pairing,reconnect,wake_console,ota_update}_decrypted.pcapng` and
`btle_procon2_{pairing,reconnect,wake_console}_decrypted.pcapng` as controls.

## Results

### `0x13` appears in **none** of the eight decrypted captures — including mouse mode

The scan is proven sound: it recovered a broad genuine command set in the mouse session
(`0x01/0C`, `0x02/04`×6, `0x07/01`, `0x09/07`, `0x0A/02`, `0x0A/08`, `0x0C/02`, `0x0C/04`, `0x0F/00`,
`0x10/01`, `0x11/01`, `0x11/03`, `0x16/01`). **`0x13` is absent from every file** —
mouse/pairing/reconnect/wake/OTA for Joy-Con 2 and Pro2 alike. The hypothesis is **refuted**.

### Mouse mode is enabled by the **feature-select command `0x0C` with the mouse bit**, not `0x13`

The timestamped request stream from the mouse-mode session:

```
t+0.000  0x07/01                       first-init
t+0.010  0x02/04  40 7e 00 00 00300100  memory read @0x013000 (factory)
t+0.020  0x10/01                       firmware info
t+0.030  0x16/01
t+0.570  0x0A/02  03000000             play vibration sample 0x03 (connection tone)
t+0.580  0x09/07  01000000…            player LED 1
t+0.590  0x0C/02  37 00 00 00          SET FEATURE MASK = 0x37   ← mouse bit set
t+0.84…  0x02/04  …                    memory reads (0x013080, 0x1FC040 user calib, …)
t+0.900  0x11/03
t+0.930  0x0A/08  0159090000…          vibration data
t+0.950  0x11/01
t+0.965  0x0C/04  37 00 00 00          ENABLE FEATURES = 0x37    ← mouse bit set
t+1.025  0x01/0C                       NFC id
t+4.835  0x0F/00                       (command 0x0F — see below)
```

The feature mask is **`0x37`**. Decomposed against the `commands.md` feature-flag table:

| Bit | Mask | Feature | In `0x37`? |
|---|---|---|---|
| 0 | 0x01 | Button state | ✅ |
| 1 | 0x02 | Analog sticks | ✅ |
| 2 | 0x04 | IMU (accel+gyro) | ✅ |
| 4 | **0x10** | **Mouse data (JoyCon only)** | ✅ |
| 5 | 0x20 | Rumble | ✅ |

`0x37 = 0x27 + 0x10`. The Pro Controller 2 / GameCube init uses **`0x27`** (buttons+sticks+IMU+rumble);
the Joy-Con mouse session adds exactly the **`0x10` "Mouse data"** bit. So **mouse mode is negotiated
through the ordinary `0x0C` feature mechanism** (`0x0C/02` set-mask + `0x0C/04` enable), and the
mouse-data fields then appear in the streamed input report. Command `0x13` is not involved.

### Bonus observation — command `0x0F` is real

`0x0F/00` (no data) is genuinely issued ~3.8 s after init settles (`t+4.835`), the first hard evidence
of the otherwise-"Unknown" command `0x0F` in this repo. Purpose still unknown; it is bare-ACKed by our
`default:` today, which appears sufficient (this Joy-Con session proceeds normally). Logged for the
`command-surface.md` §6 unknown-command backlog.

## Conclusion

- **Refuted:** command `0x13` does **not** drive Joy-Con mouse mode, and is **absent from every
  decrypted capture we hold.** Its purpose remains genuinely Unknown — the "JoyCon-only" note stands
  but is unobserved in pairing, reconnect, wake, OTA, or mouse sessions.
- **Resolved mechanism:** mouse mode = feature mask **`0x37`** via `0x0C/02`+`0x0C/04` (the `0x10`
  "Mouse data" bit on top of the standard `0x27`). This is a declarative feature toggle, not a
  dedicated command.
- **Implication for PicoSwitch2:** none of the console-native personalities need `0x13`. If Joy-Con 2
  mouse *output* were ever emulated, it would be gated by advertising/accepting the `0x10` feature bit
  in the `0x0C` handler and adding mouse fields to the report — not by implementing `0x13`.

## Remaining questions / future work

- **Where is `0x13` actually used?** Not in any state we've captured. Candidates to capture next: a
  brand-new Joy-Con↔console first-time setup, charging-grip attach/detach, or a calibration flow. Until
  observed, treat `0x13` as Unknown and leave it bare-ACKed.
- **`0x0F` semantics** — now confirmed real; needs a request/response correlation from a capture where
  its effect is observable.
- These findings do not require any firmware change; `0x13`/`0x0F` remain correctly bare-ACKed.

---

# Experiment 3 — Live USB capture: genuine Pro Controller 2 with headset plugged in

## Question

With a real Pro Controller 2 + headset attached to a PC, (a) can we capture the vendor `0x17`/`0x18`
audio-config commands, (b) what is the genuine USB audio (UAC) descriptor/format with a headset
present, and (c) how does the genuine device compare byte-for-byte to PicoSwitch2's descriptors?

## Method

**Live capture** (the user attached a genuine Pro Controller 2 with headset and authorized it).
Elevated `USBPcapCMD.exe` on the controller's root hub (`\\.\USBPcap1`, located by probing all three
hubs), `--inject-descriptors` to dump the connected device's descriptors at capture start. Two
captures: an 18 s baseline (descriptors + idle HID/mic), and a second while WAV audio was played to
the "Headphones (Switch 2 Pro Controller)" endpoint to force an isochronous OUT stream. Decoded with a
Python parser + Wireshark `tshark`. **No firmware/code modified.**

## Captures (local — `usbpcaptures/` is `.gitignore`d, per repo convention for capture binaries)

- `usbpcaptures/genuine_procon2_headset_2026-07-19.pcap` — full config descriptor + idle traffic.
- `usbpcaptures/genuine_procon2_headset_audio_2026-07-19.pcap` — trimmed slice of the genuine
  **headphones isochronous OUT** stream.

Both stay on the capture machine (like the other `usbpcaptures/*.pcapng`); the decoded findings in
this doc are the durable, committed record.

Environment: Windows host; controller enumerated as `VID_057E&PID_2069` composite device with Windows
audio endpoints **"Headphones (Switch 2 Pro Controller)"** and **"Microphone (Switch 2 Pro
Controller)"** both present → headset detected.

## Results

### Full genuine configuration descriptor (headset present) — decoded

268-byte config, **5 interfaces**, self-powered, `bMaxPower` 250 (500 mA):

| Iface | Class | Endpoints | Notes |
|---|---|---|---|
| 0 | HID (0x03) | INT `0x81` IN + `0x01` OUT, 64 B, **`bInterval`=4** | game controller |
| 1 | Vendor (0xFF) | **BULK `0x02` OUT + `0x82` IN, 64 B** | command channel |
| 2 | Audio Control (UAC 1.0) | — | topology below |
| 3 | Audio Streaming | ISO `0x03` OUT, 192 B, `bInterval`=1 | Headphones |
| 4 | Audio Streaming | ISO `0x83` IN, 192 B, `bInterval`=1 | Mic |

**UAC topology:** USB-stream (term 1) → Feature (2) → **Headphones OUT (term type `0x0302`)**; and
**Microphone (term type `0x0201`, term 4)** → Feature (5) → USB-stream OUT (6). Both streaming
interfaces declare **`FORMAT_TYPE`: 2 ch, 16-bit, `tSamFreq` = `80 bb 00` = 48000 Hz**.

### `0x17` = 48 kHz — corroborated (not directly captured)

The descriptor's `tSamFreq` bytes **`80 bb 00`** are the *exact* value in command `0x17`'s request
(`80 bb 00 00 02 f0 00`, `command-surface.md` §6). This independently supports **`0x17` =
audio sample-rate config**. The command itself did **not** appear (see negative results).

### Genuine headphones iso OUT stream (audio capture)

All iso frames on EP `0x03`. Payload **192 B per 1 ms frame** (48000 × 2ch × 2B ÷ 1000), and the
Windows host submits URBs of **1920 B = 10 frames (10 ms) each**. Confirms the 48 kHz/16-bit/stereo
format from the descriptor and shows the host's 10 ms URB batching — useful reference for our own
audio-bridge buffering.

### Byte-for-byte comparison to PicoSwitch2 (`switch_pro2.c`)

- **Audio function (IAD + iface 2/3/4, all AC/AS/format/endpoint descriptors): IDENTICAL** to
  `ns2_config_desc` (`switch_pro2.c:157-181`). Our UAC audio descriptor is already an exact match —
  strong validation of the audio implementation.
- **Vendor bulk interface (`0x02`/`0x82`, 64 B): IDENTICAL** (`switch_pro2.c:152-156`).
- **HID interrupt endpoints — DIVERGE:** genuine uses **`bInterval`=4** (≈250 Hz poll); we use
  **`bInterval`=1** (`switch_pro2.c:150-151`, ≈1000 Hz). Deliberate low-latency choice on our side,
  but a real, verifiable difference from genuine hardware. Trade-off: latency vs. indistinguishability.

### Negative results (important scoping)

- **No bulk/vendor traffic at all** → Windows never issues `0x17`/`0x18` (or any command). Those are
  **console-driven**; a PC host alone cannot produce them. Confirms the Exp 1 finding.
- **No UAC control handshake** (SET_INTERFACE alt-1, `SAMPLING_FREQ_CONTROL` SET_CUR) in the audio
  window — the audio interface was already active/open when capture started, so the one-time setup
  fell outside the window.

## Conclusion

- Genuine headset-present descriptor set captured and decoded for the first time in-repo; **our audio
  and vendor descriptors are byte-identical** to genuine.
- Audio format nailed down: **48 kHz / 16-bit / stereo, iso EP `0x03` (HP) & `0x83` (mic), 192 B/frame**.
- **`0x17` = sample rate (48000)** strongly corroborated by the descriptor, though the vendor command
  is unobtainable from a PC host.
- One genuine divergence found: **HID `bInterval` 4 (genuine) vs 1 (ours)**.

## Remaining questions / future work

- **To actually capture `0x17`/`0x18`:** either (a) run the PC console-init tool that produced
  `genuine_procon_2.pcapng` **with the headset attached** (it drives the bulk channel and may issue
  the audio-config commands), or (b) interpose on a real Switch 2 ↔ controller link. A PC audio
  session alone will not.
- **To capture the UAC control handshake:** start the capture *before* opening the audio device (e.g.,
  re-plug the headset or toggle the Windows endpoint while capturing) to record SET_INTERFACE alt-1 +
  the sample-rate SET_CUR.
- **HID `bInterval`:** consider whether matching genuine (4 ms) matters for indistinguishability vs.
  keeping 1 ms for latency — a deliberate, documented choice, not a defect. No change made here.
