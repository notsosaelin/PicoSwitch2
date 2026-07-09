# Switch 2 Output Emulation

This firmware emulates a **Switch 2 Pro Controller** (VID `0x057E` / PID `0x2069`) over wired USB. The relevant code resides in `src/switch_pro2/`.

## USB Descriptors & Identity (`switch_pro2.c`)
- **Device & Configuration:** The device enumerates as a composite IAD device (Misc/02/01). We output an "Option A" configuration which exposes two primary device functions via Interface Association Descriptors (IADs): the **Controller Input** (HID + Vendor Bulk) and a **Nonfunctional Audio Device** (Audio Control + Streaming).
- **Interface 0 (HID):** Endpoints `0x81` (IN) and `0x01` (OUT) handle all input streaming and HD rumble output.
- **Interface 1 (Vendor Bulk):** Endpoints `0x82` (IN) and `0x02` (OUT) handle the initial command and handshake protocol, which includes device memory reads and BLE pairing initialization.
- **Interfaces 2-4 (Audio):** Stub interfaces that satisfy the Switch 2's expectation for a Pro Controller with a headset jack. These currently do not process functional audio.

## Command Protocol & Handshake
Initialization of the controller operates over the Vendor Bulk Interface. 
- **AES-128 Pairing:** The Switch 2 performs a Bluetooth pairing routine over the wired USB link (`0x15`). We successfully satisfy this by performing the `AES128_ECB` challenge response.
- **Memory Reads:** The console initiates several `0x02` flash memory reads (`0x13000` block). We fake the responses with a static `factory[]` memory block containing calibration data, body colors, and serial strings.
- **Feature Selection:** Command `0x0C` sets up the feature mask (buttons, sticks, IMU, rumble). 

## Input Reporting (Report `0x09`)
Input reporting begins once the console selects the report id via command `0x03` / `0x0A`.
The state is constructed inside `ns2_build_report()` by reading `switch_pro_input_t` from the seam.
- **Buttons (3 bytes):** Bits correspond to standard Switch inputs.
- **Switch 2 Extra Buttons:** Captured via `switch_pro_input_t.extra`. These map to `GL` (Left Grip), `GR` (Right Grip), and `C` (Chat) buttons, placing them into byte 2.
- **Sticks:** Mapped into the same 12-bit nibble-packed structure as Switch 1.

## Output Reporting (Report `0x02` - Rumble)
The Switch 2 sends rumble commands via Report `0x02` over the HID `0x01` OUT endpoint.
It utilizes HD Rumble (packed frequency/amplitude LRA values).
Currently, `ns2_hid_out_report` parses this 42-byte block to extract a unified peak amplitude from both the left and right rumble requests. This amplitude is scaled into 8-bits and forwarded to the Bluetooth layer.
