# Virtual Amiibo persistence and clean/used library model — 2026-07-25

## Trigger

Hardware testing proved that a console write updated the live Virtual Amiibo and completed the
logical eject/re-present flow, but the written image did not survive dongle power loss. Source
inspection found the exact boundary: browser upload commit requested the flash journal service,
while `virtual_amiibo_store_apply_console_write()` changed RAM only.

## Implemented correction

- A successful console `0x08` commit now requests persistence immediately.
- Logical TagRemoved is deferred if Stop arrives before that persistence request completes.
- Storage version 2 retains two images:
  - **Unused** — the immutable user-imported baseline.
  - **Used** — the mutable console-written image.
- Selecting Unused or Used changes only which copy is presented; neither copy is destroyed.
- Dirty state, selected copy, both images, the optional 32-byte originality signature, and
  generation are stored together.
- Two non-adjacent 4 KiB flash sectors alternate snapshots. The previous bank remains valid until
  the destination bank is erased, programmed, and CRC-verified. Version-1 records remain readable
  and migrate as an Unused baseline on the first version-2 save.
- Flash work still runs only from the existing core-1 config-save service. There is no NFC idle
  polling and no steady-state CPU cost.

The new snapshot adds one 540-byte clean image and a 1,280-byte static flash-program buffer. Current
linked `.bss` is 179,848 bytes on Pico 2 W and 112,612 bytes on Pico W, leaving substantial board
headroom. Pico 2 W remains at the validated 300 MHz clock; no clock change was made.

## Portal model

The production portal is now usable as an amiibo library without Web Serial. Cached entries retain
separate Unused and Used byte arrays plus the selected state. Carousel badges and details identify
the presented copy. When connected, loading a library entry transfers both copies and restores the
selection.

**Save current Amiibo** retrieves both copies, updates the matching IndexedDB record, then clears
firmware dirty protection only after the browser write succeeds. A versioned JSON library backup
exports all cached images and both copies; the same page can import that backup after browser data
loss.

## Evidence level and remaining gate

- Pure clean/used load, write, selection, export, and used-copy import: host-tested.
- Stop-before-persist removal deferral: host-tested.
- Pico W and Pico 2 W firmware: compile-confirmed.
- Portal JavaScript and DOM references: statically validated.
- Power-cycle recovery, live-console flash timing, Used/Unused selection, and browser backup UX:
  **hardware/browser validation pending**.

## Hardware/browser follow-up

The maintainer subsequently validated the complete intended workflow on 2026-07-25:

- a console-owned write completed and logically ejected only after the automatic snapshot;
- the Used image and dirty state survived dongle power loss;
- Unused and Used remained independently selectable after reboot;
- **Save current Amiibo** preserved both copies in the browser-local library;
- the library remained usable without a serial connection; and
- export, cache clear, and backup import restored the saved library successfully.

No regression was reported in the tested controller, console, or configuration behavior.
