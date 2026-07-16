# Session backup — 2026-07-16

## Resume here

Working directory: `E:\PicoSwitch2`

Branch: `ns2-testing`

Current objective: stop the Switch 2 from prompting to update the emulated Pro Controller 2 every
time the PicoSwitch2 dongle is connected.

The implementation is complete and committed. The next required step is a real-console test in
Switch 2 Pro Controller mode. Flash the appropriate artifact:

- `build/pico_w/PicoSwitchWGA-pico_w.uf2`
- `build/pico2_w/PicoSwitchWGA-pico2_w.uf2`

Connect it to the Switch 2 and verify whether the controller-update prompt disappears. Also confirm
ordinary input, rumble, pairing, reconnect, and automatic console wake remain functional.

## What changed

`src/switch_pro2/switch_pro2.c` previously repeated an older genuine Pro Controller 2 response:

- Controller firmware `1.1.5`
- Bluetooth patch `12.0.0`
- No DSP firmware (`FF FF FF`)

It now reports a later genuine retail-controller response cited by NS-PC-Control's private
`PC2_Gyro_*.pcapng` reference capture:

- Controller firmware `2.0.17`
- Controller type `0x02` (Pro Controller)
- Bluetooth patch `12.0.0`
- DSP firmware `0.2.2`
- Command `0x10/0x01` payload: `02 00 11 02 0C 00 00 00 00 02 02 00`

Named constants are shared by both Pro2 version surfaces so they cannot drift:

1. EP0 vendor request `0x02` leading firmware triplet.
2. Vendor command `0x10/0x01` complete 12-byte firmware-information response.

Only the Pro Controller 2 personality was changed. Joy-Con 2 and NSO GameCube version identities
were deliberately left untouched because each model has its own firmware line.

Documentation was updated in:

- `docs/switch2/usb-spec.md`
- `docs/experiments/ns-pc-control-audit-2026-07-12.md`

## Evidence and decisions

- The bundled genuine USB capture contains the old `1.1.5` response exactly.
- NS-PC-Control originally used `2.0.17 / 12.0.0 / 0.2.2`, describing it as captured from a real
  reference Pro Controller 2.
- NS-PC-Control later changed to artificial ceiling values `2.9.99 / 12.9.9 / 0.9.9` to suppress
  updates. PicoSwitch2 deliberately did **not** copy those fictional values.
- Full Nintendo updater emulation was rejected for this step. Command `0x0D` transfers an
  approximately 240 KiB image, validates CRC32, switches controller flash banks, and reboots. The
  image targets Nintendo's controller MCU, not RP2040/RP2350, so accepting it would require a fake
  persistence/reboot protocol and would not update PicoSwitch2 firmware.
- If the prompt remains, do not guess another version. Query a fully updated genuine Pro Controller
  2 using `tools/switch2_input_viewer.py` (its `get_version_info()` sends command `0x10/0x01`) and
  capture the exact 12-byte response. Then update both Pro2 version surfaces consistently.

## Verification already completed

Both release targets built successfully with `./build.ps1 pico_w,pico2_w`.

Binary inspection found the new 12-byte response exactly once in each `.bin` and found the old
command response zero times:

- `build/pico_w/PicoSwitchWGA-pico_w.bin`: new=1, old=0
- `build/pico2_w/PicoSwitchWGA-pico2_w.bin`: new=1, old=0

`git diff --check` passed before the commit (only the repository's normal LF-to-CRLF warnings were
reported).

## Nearby completed work

- `30b4970 feat: add hardware-verified Switch 2 wake`
- `926b51c fix: remove BOOTSEL wake test hook`

Automatic controller-driven wake remains present. The temporary BOOTSEL single-tap wake trigger is
removed. Do not reintroduce it unless explicitly requested.

## Worktree caution

`COLOR-INVESTIGATION.md` was already present as an unrelated untracked user file when this backup
was created. It was intentionally not inspected, modified, staged, or included in this commit.

No push was performed in this session.
