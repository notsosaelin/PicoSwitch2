# Smash native NFC write-capture attempt

Date: 2026-07-25
Result: **no write transaction occurred**

## Setup

- Real Switch 2 running Smash Bros.
- Genuine Pro Controller 2 paired through PicoSwitch2.
- UART-gated native NFC mirror active.
- USB protocol trace filtered to NFC.
- BLE capture filtered to NFC.
- The presented amiibo device changes the emulated tag/UID between presentations.

Both capture rings were stopped before they filled:

- USB: 82 records, 0 overwritten.
- BLE: 92 records, 0 dropped.
- Mirror: 41 commands submitted, 41 sent, 41 responses, 0 timeouts, 0 rejected.

## Result

The first tag, UID `04 A4 47 F2 6F 40 80`, completed a normal read:

```text
03 → 05(09 00) → 04
03 → 05(09 00) → 06 → 05(04 00)
15@0000 → 0046 → 008C → 00D2 → 0118 → 015E → 01A4 → 01EA → 0230
05(04 00) → 04
```

The next presentations used a different UID, `04 D5 E7 48 CC F9 71`. The console repeatedly issued
only scan, status, and stop commands. It never issued:

- `0x01/0x14` write-buffer chunks;
- `0x01/0x08` write/commit;
- a UID-bearing `0x01/0x06` write descriptor.

The changing tag therefore prevented this attempt from reaching a game-owned write transaction.

## Additional state evidence

The genuine controller's report NFC state changed through:

```text
0 → 1 → 2 → 3
```

during the complete read, then:

```text
4 → 5 → 6 → 7 → 0
```

during repeated presentation/removal activity with the second UID. These later state values are
confirmed observations, but their individual semantic labels remain unknown because no write
commands accompanied them.

## Next capture requirement

Use one writable tag whose UID and contents remain stable across the prerequisite read and the
game's write prompt. The capture must include the read, UID-bearing begin-write descriptor,
all `0x14` chunks, `0x08` commit, removal/re-presentation if requested, and readback.
