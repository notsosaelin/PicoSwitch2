# Unmapped Features & Known Limitations

While the core functionality of the PicoSwitch2 (connecting Bluetooth controllers and bridging them to a Switch 2) is fully operational, several Switch 2 and hardware-specific features remain unmapped or partially implemented.

## Switch 2 Console Output (`src/switch_pro2/`)

### 1. Motion Data (IMU) Packing
- **Status:** **Unverified / Disabled by Default**
- **Details:** The Switch 2 uses report `0x09` for input. We know how to emit IMU data for report `0x05` (the Switch 1 standard/PC profile), but the 40-byte motion packing format for report `0x09` is still undocumented. 
- **Current Behavior:** We have a hypothesis implementation that writes `[timestamp] + 3 x [accel + gyro]`, but until it's verified on hardware, we consider the Switch 2 IMU unmapped.

### 2. True HD Rumble
- **Status:** **Lossy mapping to standard rumble**
- **Details:** The Switch 2 sends 40-bit little-endian HD rumble data containing distinct frequency and amplitude values for both the left and right Linear Resonant Actuators (LRAs). 
- **Current Behavior:** `ns2_hid_out_report` simply extracts the *peak amplitude* of the left and right LRAs and forwards a unified 0-255 amplitude back to the connected Bluetooth device. The frequency data and stereo-separation are lost.

### 3. NFC / Amiibo
- **Status:** **Unmapped**
- **Details:** The console can request NFC operations (command `0x01`).
- **Current Behavior:** We hardcode an idle state / empty acknowledgment to satisfy the console. We do not read Amiibo data or forward NFC commands to connected controllers.

### 4. USB Audio
- **Status:** **Stubbed / Nonfunctional**
- **Details:** A full Switch 2 Pro Controller exposes 3 distinct audio interfaces (`Audio Control`, `Audio Streaming IN`, `Audio Streaming OUT`) for headset and microphone passthrough.
- **Current Behavior:** We expose these interfaces (Option A) so the console recognizes the controller perfectly, but the endpoints are connected to a stub class driver that stalls or ignores the audio stream. We haven't built out the routing yet.

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
