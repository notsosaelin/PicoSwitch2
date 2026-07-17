# Joy-Con 2 Open Questions

Last updated: 2026-07-16

Both side personalities enumerate and stream on real Switch 2 hardware. Their descriptor and
wire-layout facts remain in [protocol.md](protocol.md); this file owns unresolved interpretations.

## Mapping

- The complete sideways map for both sides is hardware-confirmed, including face-button rotation,
  L1/R1 -> SL/SR, trigger reassignment, D-pad-to-stick synthesis, and stick-axis rotation.
- The 2026-07-16 Windows/Steam Left fix is hardware-confirmed through the visible Steam controller
  UI as well as the Windows and exact-Steam-SDL layers. The original failure is now explained:
  - Steam's SDL build explicitly supports both PIDs, but logs the emulated Left as
    `driver = NONE (DISABLED)` while the emulated Right loads `SDL_JOYSTICK_HIDAPI_SWITCH2`.
  - Windows bound both sides' HID interface to `HidUsb` and vendor interface to `WinUSB`, but the
    failing Left's interface 1 had no `DeviceInterfaceGUID`. An exact-Steam-SDL probe showed libusb
    discover the composite parent and HID interface 0, then reject the `MI_01` child because no
    GUID was registered. SDL consequently had no libusb handle for the bulk interface and fell
    back to its generic HID path.
  - The working Right registry node did contain `DeviceInterfaceGUID` despite the firmware only
    serving the `WINUSB` Compatible ID descriptor. That retained Windows state hid the firmware
    omission and explains the apparent Left/Right protocol asymmetry.
  - Firmware now also serves the Microsoft OS 1.0 Extended Properties descriptor (`wIndex=0x0005`)
    for Joy-Con 2 interface 1, registering the already-observed working family GUID
    `{6F13725E-EF0E-4FD3-AE5F-B2DE989EC825}`. This is Windows-only enumeration metadata; Switch 2
    descriptors and protocol behavior are unchanged.
  - On a fresh Left device node, Windows registered that GUID and Steam's exact SDL build opened
    and claimed interface 1, assigned bulk endpoints `0x02`/`0x82`, completed its initialization
    transfers, and reported `driver = SDL_JOYSTICK_HIDAPI_SWITCH2 (ENABLED)`. Steam then recognized
    the Left normally without the previous `Begin Setup` prompt.
  - A Left-only test changed USB serial `"00"` to the existing fictitious factory serial
    `HBW99999999999`. Steam read the new serial, proving that Windows created/used the isolated
    identity, but still fell back to `driver = NONE`; the serial/devnode collision is therefore
    ruled out as the primary cause and the test change was reverted.

## Output and command behavior

- Rumble ON/STOP/reconnect behavior is hardware-confirmed on both output personalities. Exact
  opaque report `0x01` byte semantics still require a side-specific capture before fidelity claims.
- Several EP0 and vendor-command responses reuse confirmed family framing with Joy-Con-specific
  identity bytes. Capture the exact Joy-Con exchange before assigning meaning to opaque fields.
- Motion, mouse, magnetometer, NFC, and side-specific accessory fields remain outside the first
  implementation and must not be inferred from Pro Controller 2 or GameCube behavior.

## Physical USB constraint

One Pico USB peripheral has one address, so it cannot expose a genuine two-child wired Joy-Con
pair. Left and Right are intentionally separate personalities, matching the real Charging Grip's
two independently addressed children rather than inventing a merged device.
