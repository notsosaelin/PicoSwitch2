# Virtual Amiibo read validation

Date: 2026-07-25
Result: **successful — the Switch 2 recognized the uploaded virtual amiibo**

## Tested path

1. Flash the Pico 2 W build containing the primary-capture-derived virtual reader.
2. Enter config mode.
3. Upload/select an amiibo image through the local Web Serial portal.
4. Enable the disabled-by-default **Virtual Amiibo** feature and save the configuration.
5. Return to the Pro Controller 2 personality with a non-NFC source controller.
6. Start an amiibo scan on a real Switch 2.

The maintainer confirmed that the console recognized the virtual amiibo.

The exact uploaded source size (540-byte raw or 572-byte extended) was not recorded in this
validation, so this result does not distinguish originality-signature handling between those two
formats.

## Validated implementation

- Config-mode upload and active-tag selection reach the console runtime intact.
- The feature gate survives the config-to-controller personality transition.
- The command-driven report-state sequence is sufficient for the console.
- The 61-byte status response is accepted.
- The primary-capture-derived 600-byte reader buffer is accepted.
- The console accepts eight 70-byte `0x15` chunks followed by one 40-byte final chunk.
- No 622-byte payload or 630-byte monolithic read response is required.

## Not validated by this test

- Console writes or download of console-mutated save data.
- Flash recovery after a power interruption.
- Tag replacement/removal during an active scan.
- Native Pro Controller 2 or Joy-Con 2 Right physical-tag writes.
- Switch 1 controller NFC translation.
