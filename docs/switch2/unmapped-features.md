# Unmapped Features & Known Limitations

While the core functionality of the PicoSwitch2 (connecting Bluetooth controllers and bridging them to a Switch 2) is fully operational, several Switch 2 and hardware-specific features remain unmapped or partially implemented.

## Switch 2 Console Output (`src/switch_pro2/`)

### 1. Motion Data (IMU) Packing
- **Status:** **✅ Implemented for genuine Pro Controller 2 passthrough and DualSense/Edge translation; length-`0x28` synthesis remains unmapped**
- **Details:** Report `0x09` carries both length-`0x1E` and length-`0x28` native motion PDUs. The
  length-`0x1E` quaternion carrier is decoded and hardware-validated. Length-`0x28` G6/G7/G8
  bit-packing is exact, but its other changing lanes and full semantics remain incomplete.
- **Current Behavior:** A genuine Pro Controller 2 supplies its native `0x1E`/`0x28` PDUs through
  an opaque passthrough path. DualSense and DualSense Edge use factory-calibrated IMU data,
  timestamp-aware quaternion integration, and the length-`0x1E` carrier. Both paths are
  hardware-confirmed in Splatoon 3 and survive reconnect. A static-template synthetic `0x28`
  experiment caused random motion and was removed.

### 2. True HD Rumble
- **Status:** **Partially improved 2026-07-12 — stereo amplitude now preserved; frequency data still lost.**
- **Details:** The Switch 2 sends 40-bit little-endian HD rumble data containing distinct frequency and amplitude values for both the left and right Linear Resonant Actuators (LRAs).
- **Current Behavior:** `ns2_hid_out_report` extracts each motor's own peak amplitude (across its 2 internal frequency bands) **independently** and forwards both via `report_set_rumble(idx, left, right)` — a real, previously-unused capability in joypad-os's `feedback_rumble_t` (`left`/`right` fields) that our cross-core seam was collapsing to one shared scalar before this fix (both Switch 1's `switch_pro.c` and Switch 2's `switch_pro2.c` had the same collapse; both fixed together, plus `ns2_seam.c`'s `feedback_get_state()` bridge). Drivers with true independent-motor output (DualSense, Xbox, per joypad-os) now get distinct left/right intensity instead of both motors buzzing identically. DualSense's native `0x39` haptic renderer is separately hardware-confirmed with interval peak preservation and a 3.25× waveform-preserving curve. **Still lost for generic rumble targets:** the original frequency component; full waveform/frequency translation requires a capability-aware renderer per controller family.

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
- **Status:** **✅ Pico 2 W DualSense and genuine Pro Controller 2 headphone output operational; microphone return open**
- **Details:** A full Switch 2 Pro Controller exposes 3 distinct audio interfaces (`Audio Control`, `Audio Streaming IN`, `Audio Streaming OUT`) for headset and microphone passthrough.
- **Current Behavior (2026-07-24):** The PC2-specific UAC1 driver owns both isochronous endpoints,
  accepts 48 kHz stereo speaker PCM, supplies continuous silent microphone PCM, and implements
  writable mute/volume controls. Pico 2 W can route that audio to a DualSense speaker/jack with
  native haptics or to a genuine Pro Controller 2 jack through its 240-byte/20 ms CELT framing.
  DualSense physical-jack state, repeated removal/reinsert, saved-bond reconnect, input, haptics,
  gyro, LED, and BOOTSEL behavior are hardware-confirmed. Pico W intentionally remains non-audio.
- **Remaining work:** DualSense microphone report decoding and Opus-to-USB return.

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
- **Status:** **Implemented for DualSense/Edge and genuine Pro Controller 2; other families unmapped**
- **Details:** Audio transport is controller-specific HID/GATT framing, not a generic SCO/eSCO
  feature that can be enabled for every gamepad.
- **Current Behavior:** Pico 2 W routes console/PC PCM to DualSense/Edge through Sony report `0x39`
  and to a genuine Pro Controller 2 through ordered GATT writes. Xbox and other headset-capable
  controller families remain unsupported until their transport is independently captured.
