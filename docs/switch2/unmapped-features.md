# Unmapped Features & Known Limitations

While the core functionality of the PicoSwitch2 (connecting Bluetooth controllers and bridging them to a Switch 2) is fully operational, several Switch 2 and hardware-specific features remain unmapped or partially implemented.

## Switch 2 Console Output (`src/switch_pro2/`)

### 1. Motion Data (IMU) Packing
- **Status:** **Unverified / Disabled by Default**
- **Details:** The Switch 2 uses report `0x09` for input. We know how to emit IMU data for report `0x05` (the Switch 1 standard/PC profile), but the 40-byte motion packing format for report `0x09` is still undocumented. 
- **Current Behavior:** We have a hypothesis implementation that writes `[timestamp] + 3 x [accel + gyro]`, but until it's verified on hardware, we consider the Switch 2 IMU unmapped.

### 2. True HD Rumble
- **Status:** **Partially improved 2026-07-12 — stereo amplitude now preserved; frequency data still lost.**
- **Details:** The Switch 2 sends 40-bit little-endian HD rumble data containing distinct frequency and amplitude values for both the left and right Linear Resonant Actuators (LRAs).
- **Current Behavior:** `ns2_hid_out_report` extracts each motor's own peak amplitude (across its 2 internal frequency bands) **independently** and forwards both via `report_set_rumble(idx, left, right)` — a real, previously-unused capability in joypad-os's `feedback_rumble_t` (`left`/`right` fields) that our cross-core seam was collapsing to one shared scalar before this fix (both Switch 1's `switch_pro.c` and Switch 2's `switch_pro2.c` had the same collapse; both fixed together, plus `ns2_seam.c`'s `feedback_get_state()` bridge). Drivers with true independent-motor output (DualSense, Xbox, per joypad-os) now get distinct left/right intensity instead of both motors buzzing identically. **Still lost:** the frequency component of HD rumble (only amplitude is forwarded) — genuine HD-rumble fidelity (waveform/frequency-accurate haptics) would need a much deeper translation layer, tracked separately under "Advanced Haptics" in `PLAN.md`. Untested on hardware — no observable regression risk (amplitude range/scaling unchanged, only the L/R duplication was removed).

### 3. NFC / Amiibo
- **Status:** **🔵 Partially reverse-engineered, still unmapped (implementation unchanged).**
- **Details:** The console requests NFC operations via command `0x01`, whose subcommand family
  and one full request/response pair (subcommand `0x0C`) are now traced to exact packets in this
  repo's own genuine-controller capture, plus a second, previously-undocumented exchange
  (subcommand `0x01`, bare acknowledgment). Full confidence-qualified inventory, six-claim
  evidence separation (official confirmation vs. hardware ID vs. protocol behavior, per
  controller type), and the next recommended capture/analysis task:
  [`docs/switch2/nfc-protocol-inventory.md`](nfc-protocol-inventory.md). No NFC IC has been
  identified in either controller; no real amiibo tag transaction (detect/read/write/mount/
  unmount) has been observed in any capture this project holds.
- **Current Behavior:** We hardcode an idle state / empty acknowledgment (subcommand `0x0C`
  returns the real captured bytes `61 12 50 10`; the response `dir` byte for other bare-ack
  subcommands is `0x04`, fixed 2026-07-12 — see the inventory doc §2.3) to satisfy the console. We
  do not read Amiibo data or forward NFC commands to connected controllers.
- **Reference implementation exists, not ported (2026-07-12):** `Dycool/NS-PC-Control` has a
  complete, working amiibo read/write emulation for the native Switch 2 vendor channel (scan mode,
  status, begin-operation, a 622-byte read-buffer format, a 454-byte write-staging format). Not
  adopted — their source captures aren't independently verifiable by this project, per this
  project's own evidence discipline. Recorded as a structured hypothesis (not fact) in
  `nfc-protocol-inventory.md` §4 and `docs/experiments/ns-pc-control-audit-2026-07-12.md` §2, ready
  to validate against if this project ever captures its own real amiibo transaction.

### 4. USB Audio
- **Status:** **Stubbed / Nonfunctional** (hardened 2026-07-12 — see below)
- **Details:** A full Switch 2 Pro Controller exposes 3 distinct audio interfaces (`Audio Control`, `Audio Streaming IN`, `Audio Streaming OUT`) for headset and microphone passthrough.
- **Current Behavior:** We expose these interfaces (Option A) so the console recognizes the controller perfectly, but the endpoints are connected to a stub class driver that stalls or ignores the audio stream. **2026-07-12:** confirmed (by reading TinyUSB's own `usbd.c`) that `SET_INTERFACE`/`GET_INTERFACE` alt-setting switches were never actually broken — the framework ACKs those regardless of what our stub does; added static Mute/Volume `GET_CUR` answers on both Feature Units so a host's audio driver doesn't see failed control transfers. Still no functional audio routing — spec-compliance polish only.
- **Research for an actual future implementation:** [`docs/switch2/audio-passthrough-research.md`](audio-passthrough-research.md) — a working MIT-licensed reference (`awalol/DS5Dongle`) does the structurally identical PC-facing bridge for DualSense's own onboard audio (Opus codec over BT report `0x39`/`0x32`, real `tud_audio` USB side). Not started; documented so a future session doesn't have to re-derive the protocol.

### 5. Config / Memory Writes
- **Status:** **Unmapped**
- **Details:** The console sends memory writes (e.g., to calibration or pairing records).
- **Current Behavior:** We acknowledge the writes (`0x02/0x05`) but silently discard them.

---

## Bluetooth Input (`joypad-os`)

### 1. Advanced Haptics (DualSense Triggers)
- **Status:** **Unmapped**
- **Details:** Controllers like the PS5 DualSense have advanced trigger haptic states (resistive, weapon fire, etc.).
- **Current Behavior:** We only forward simple vibration commands to the controller.

### 2. Audio Over Bluetooth
- **Status:** **Unmapped**
- **Details:** Controllers with headset jacks (Xbox, PlayStation) require routing audio streams over Bluetooth.
- **Current Behavior:** While `btstack` can theoretically support SCO/eSCO profiles, joypad-os and PicoSwitch2 do not currently route audio.
