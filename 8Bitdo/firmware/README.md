# 8BitDo Ultimate Bluetooth independent-paddle firmware

These tools patch the first-generation Ultimate Bluetooth Controller's official
controller firmware 1.11 so its two physical rear switches remain independent
over Bluetooth. They are intentionally limited to one byte-exact vendor image.

Nothing in this directory runs automatically. The build tools only read and
write local files. The flash harness has separate validation and flash modes,
requires an exact approved SHA-256, and refuses to write unless the controller's
manual USB bootloader (`2DC8:3208`) is already present.

## Patch behavior

The stock firmware scans P1 and P2 into internal mask bits `0x02000000` and
`0x04000000`. Its configuration report exposes them independently, but its
Switch report builder omits both. The patch changes only the normal full-report
(`0x30`) exit:

- `0x00020FD2`: four-byte branch into unused 1.11 payload padding
- `0x00031838`: 36-byte Thumb helper
- P1 becomes reserved system-button bit 7
- P2 becomes reserved system-button bit 6
- the displaced report write and original function epilogue are restored

The PicoSwitch2 parser recognizes those bits only for the tested first-generation
8BitDo identity and maps P1/P2 to `JP_BUTTON_L4`/`JP_BUTTON_R4` (GL/GR). The
stock-firmware chord workaround remains as a recovery-compatible fallback.

## Build and validate

Required local inputs:

- official type-41 Ultimate Bluetooth 1.11 firmware
- `8BitDoFirmwareUpdaterTools.dll` from the official Windows updater
- 32-bit .NET Framework compiler for the optional flash harness

The patch builder checks the official input hash, header, decoded payload hash,
hook bytes, zero-filled code cave, intended changed-byte set, and both transform
round trips.

```powershell
python 8Bitdo/firmware/build_paddle_patch.py `
  path\to\Ultimate-1.11.dat `
  8Bitdo\research\Ultimate-1.11-PicoSwitch2-Paddles-v2.dat `
  --updater-dll path\to\8BitDoFirmwareUpdaterTools.dll
```

Expected patched SHA-256:

`8A561682AD6174322C95E70A53EDD2C0AB080A41D826DCB1694A55CFDE53167C`

The superseded image with SHA-256
`BC99674803782E59A25CC97655FBAEEED388FB7156BF33CC9E4663861475D989`
must not be flashed. Its hook used a Thumb-2 `b.w`, which is not supported by
the controller's ARMv6-M instruction set and stopped Bluetooth input reports.

## Rejected reconnect experiment

`build_reconnect_experiment.py` first reproduces the known-good independent-
paddle image, then changes only the bonded Bluetooth reconnect timeout and its
matching controller-side watchdog:

- Bluetooth-module timeout: 4,800 baseband slots (3,000 ms) -> 3,200 slots
  (2,000 ms)
- controller watchdog: 6,000 ms -> 4,000 ms

The 2:1 real-time relationship is unchanged. Pairing/discovery uses a separate
path.

```powershell
python 8Bitdo/firmware/build_reconnect_experiment.py `
  path\to\Ultimate-1.11.dat `
  8Bitdo\research\Ultimate-1.11-PicoSwitch2-Paddles-ReconnectTest-v1.dat `
  --updater-dll path\to\8BitDoFirmwareUpdaterTools.dll
```

Rejected experimental SHA-256:

`FADE1A967B6B7F46AAC34243BF9A10005A950E53F579858721065505E16B1E82`

Hardware testing found no reconnect-speed improvement and broke console wake
for this controller. This image must not be flashed again. The builder remains
only to preserve the rejected experiment and its exact changed-byte record.

Compile the vendor-DLL harness as a 32-bit process:

```powershell
C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe `
  /nologo /platform:x86 /optimize+ `
  /out:8Bitdo\research\UltimateFlashHarness.exe `
  8Bitdo\firmware\UltimateFlashHarness.cs
```

Offline validation performs no device operation:

```powershell
8Bitdo\research\UltimateFlashHarness.exe `
  8Bitdo\research\Ultimate-1.11-PicoSwitch2-Paddles-v2.dat --validate
```

## Recovery and flash gate

Keep the untouched official 1.11 image:

`1030145FEC364ACEB55CEAED221396131DCF02EAAEEB8BD9AD4044BA5596074D`

The bootloader is separate from the patched application region. The harness
accepts only the stock hash and the known-good paddle hash. Put the official
updater DLL beside the compiled harness. The harness creates the DLL's required
`data` logging directory automatically.

Do not use `--flash-approved-image` as an exploratory command. First place the
controller in manual update mode and independently confirm USB `2DC8:3208`.
Then run the harness only during an explicitly approved, monitored flash:

```powershell
8Bitdo\research\UltimateFlashHarness.exe IMAGE.dat --flash-approved-image
```

The vendor's type/version handshake can return zero in manual recovery mode.
In that case the harness permits the operation only when Windows has both the
live `2DC8:3208` boot identity and a prior Ultimate Bluetooth `2DC8:6007`
application identity at the same physical USB port. A completed write is
defined by updater progress `100`; the misleading `dll_errorUpdate` export is
an abort/reset action used by the official application after timeouts, not an
error-code getter.

If the custom application does not start correctly, return to manual update
mode and use the same command with the untouched stock image.
