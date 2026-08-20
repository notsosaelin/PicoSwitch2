# Bluetooth subsystem

Status: current source architecture as of 2026-08-20. The trust-lifecycle correction is host-tested
and board-built; its controlled hardware matrix is still pending.

This directory is the canonical entry point for PicoSwitch2 Bluetooth. The current-state documents
are:

| Need | Document |
|---|---|
| Runtime owners and radio roles | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| Admission, authentication, encryption and trust guarantees | [`SECURITY.md`](SECURITY.md) |
| Flash, keys, bonds, update/reset, forget and wipe | [`PERSISTENCE.md`](PERSISTENCE.md) |
| Pair, reconnect, stale-key recovery and coexistence sequences | [`LIFECYCLE.md`](LIFECYCLE.md) |
| UART snapshots and failure classification | [`DIAGNOSTICS.md`](DIAGNOSTICS.md) |
| Automated and physical regression gates | [`VALIDATION.md`](VALIDATION.md) |
| Detailed implementation history and controller-specific notes | [`btstack-implementation.md`](btstack-implementation.md) |
| Keyboard / mouse composite-source behavior | [`keyboard-mouse-input.md`](keyboard-mouse-input.md) |

The first six documents describe current contracts. `btstack-implementation.md` remains the deeper
implementation record; when an older passage conflicts with the current source or these contracts,
the source and the current-state documents win.

## Evidence status

- **Confirmed:** the pinned SDK installs Classic and LE databases into one two-bank TLV store;
  current source uses public GAP deletion for LE trust and runs persistent mutations on core 1.
- **Source-tested:** fresh-controller trust is rejected outside explicit admission; sparse LE slots
  are traversed by capacity; Classic replacement keys are deferred until authentication; an RPA is
  only a cryptographic reconnect candidate; install reset covers the full six-sector reserved
  persistence region.
- **Reopened:** a wiped or newly flashed adapter cannot silently form replacement trust when the
  remote returns. Older hardware results said this passed, but the owner later observed reconnects
  after wipe/flash. The code audit found an automatic re-pairing path that can explain that symptom.
- **Unknown until the matrix is run:** whether every supported controller family behaves correctly
  with the corrected admission gates on physical Pico W/Pico 2 W hardware.

## Non-negotiable invariants

The durable IDs and traceability table live in [`VALIDATION.md`](VALIDATION.md). The central rule is:

> Outside an explicit pairing window, PicoSwitch2 MUST NOT create a new controller trust
> relationship. A valid existing relationship MAY reconnect; erased or stale trust MUST NOT be
> silently replaced.

Bluetooth diagnostics MUST NOT print link keys, LTKs, IRKs, PIN material, or derived Switch 2 key
components.
