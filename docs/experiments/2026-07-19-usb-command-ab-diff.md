# Experiment — USB command A/B diff: genuine Pro Controller 2 vs PicoSwitch2 dongle

- **Date:** 2026-07-19
- **Author:** repository maintenance pass (analysis of existing captures; **no code modified**)
- **Related:** [`COMMAND-IMPLEMENTATION.md`](../../COMMAND-IMPLEMENTATION.md),
  [`FIRMWARE-IMPLEMENTATION.md`](../../FIRMWARE-IMPLEMENTATION.md),
  [`docs/switch2/nfc-protocol-inventory.md`](../switch2/nfc-protocol-inventory.md) §2.3,
  [`docs/switch2/usb-spec.md`](../switch2/usb-spec.md)

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
  `FIRMWARE-IMPLEMENTATION.md` §3.

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
  `COMMAND-IMPLEMENTATION.md` §6 still need a headset-present capture.
- **Recommended (separate, deliberate) fixes when code changes resume:** give `0x08` (and any other
  genuinely-observed bare-ack command) `dir=0x04`; change the factory fill byte to `0xFF`. Both are
  small, evidence-backed, and improve indistinguishability. Not applied in this pass (documentation
  only).
