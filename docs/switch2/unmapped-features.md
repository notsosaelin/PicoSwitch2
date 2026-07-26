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
- **Status:** **🟡 Genuine Pro2 and Virtual Amiibo reads are hardware-confirmed; Virtual Amiibo
  write, logical eject, re-presentation, updated readback, and UART export are also confirmed,
  while native writes remain open.**
- **Details:** Direct Switch 2/UART/BLE captures now establish the command `0x01` subcommand
  sequence, 600-byte reader buffer, 70-byte offset chunks, genuine Pro2 relay framing, modulo-eight
  NFC events, and the 88-byte multi-packet `0x14` write request. The old per-USB-packet dispatch
  caused error `2168-0002`; bounded stream reassembly eliminated the crash.
- **Current Behavior:** A real Switch 2 recognizes both a physical amiibo through the UART-gated
  genuine Pro2 relay and a browser-loaded Virtual Amiibo through a non-NFC controller. The
  disabled-by-default virtual path supports transactional upload, separate immutable Unused and
  mutable Used images, automatic alternating-bank persistence, and exact retrieval. A game-owned
  write reached full staging, `0x08`, and accepted `05 00`. The runtime keeps the Used image loaded,
  waits for its snapshot before logical eject, and re-presents it on the next scan. Hardware
  confirms the write/eject/re-present lifecycle, valid mutated UART export, automatic persistence,
  power-cycle recovery, reversible Unused/Used selection, offline library operation, and backup
  restoration.
- **Remaining native work:** production relay gating/reconnect, a physical Pro2 write capture,
  Joy-Con 2 Right comparison, and Switch 1 MCU reader/writer translation. External projects remain
  supporting evidence rather than Switch 2 protocol truth. See
  [`docs/switch2/nfc-implementation.md`](nfc-implementation.md) and
  [`docs/switch2/nfc-protocol-inventory.md`](nfc-protocol-inventory.md).

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
