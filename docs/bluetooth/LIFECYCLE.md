# Bluetooth trust lifecycle

Status: current implementation contract as of 2026-08-20. Automated policy coverage and board
builds pass; the physical wipe/flash matrix is pending.

## Admission authority

The pure policy has three inputs: post-wipe lock, explicit pairing-window state, and usable existing
trust.

| Lock | Window | Existing trust | Decision |
|---:|---:|---:|---|
| 1 | any | any | reject |
| 0 | 0 | 1 | bonded reconnect |
| 0 | 1 | 0 | fresh pairing |
| 0 | 0 | 0 | reject |

An LE resolvable-private-address advertisement may be admitted as a bounded reconnect candidate
when the LE DB is non-empty, because its identity is not yet available at advertisement time. That
does not authorize fresh trust: the connection's fresh-admission latch remains false, and an
unresolved Just Works request is declined.

## Fresh LE controller pairing

```text
explicit BOOTSEL pairing window
  -> scan sees controller
  -> admission says FRESH
  -> connection attempt latches fresh_pairing_admitted
  -> raw LE connection completes
  -> SM Just Works request checks that per-connection latch
  -> confirm and persist BTstack bond
  -> protocol/HIDS setup completes
  -> persist JPLC only after usable controller setup
```

Closing the UI window while the raw BLE connection is in flight defers scan closure. The per-link
latch remains valid only for that admitted attempt; it is not a global open-ended permission.

Switch 2 controllers use custom ATT setup. The UART-only `btfresh` command is an explicit local
diagnostic action and grants one Switch 2 candidate admission. Automatic stale-key recovery may arm
the custom handshake mechanics but MUST NOT grant this admission.

## Fresh Classic pairing

```text
explicit pairing window
  -> inquiry or incoming ACL identifies a candidate
  -> connection filter checks existing link key or open window
  -> pending state latches trust-present versus fresh-admitted
  -> legacy PIN or SSP proceeds
  -> link-key notification is interposed and held as an in-RAM candidate
  -> matching authentication completion succeeds
  -> eligible key persists in the Classic TLV DB
  -> HID/direct-L2CAP setup completes
```

Classic SSP auto-accept and discoverability are enabled only inside the explicit pairing window.
Admission is enforced at the ACL filter and defended again at link-key notification and matching
authentication completion. Because pinned BTstack writes the notification before emitting it, the
application restores the old key or removes an unadmitted write until the transition is proven.
Outside a fresh window only the identical existing key or `CHANGED_COMBINATION_KEY` is eligible;
an unrelated replacement is rejected and disconnected.

## Bonded reconnect

Valid existing trust MAY reconnect with the pairing window closed:

- Classic incoming ACL is admitted only when a stored key exists.
- LE exact-identity and RPA candidates may connect, but an RPA is only a bounded candidate: SM must
  resolve and reuse valid security state before it can become ready.
- Only `JPLC` can become the preferred direct LE reconnect target.
- A connected peer is excluded from candidate selection.
- An explicit pairing window disables speculative direct reconnect so discovery remains available.

Successful reconnect MUST NOT rewrite flash unless the durable target record actually changes.
Ordinary RF loss MUST NOT delete trust.

## Stale security

For a standard LE peer, a re-encryption failure removes only that peer's stale LE bond. If the
connection did not enter through an explicit pairing window, firmware disconnects and leaves fresh
pairing closed. The user must open a new window to create replacement trust.

For a Switch 2 custom reconnect, failure preserves the known custom material for bounded retry.
`btfresh` is the deliberate path that discards the target bond and admits one clean custom pairing.

For Classic `PIN_OR_KEY_MISSING`, only the affected link key is removed. A generic authentication
failure preserves the old key. The next fresh trust creation remains subject to the explicit
window.

## Forget one

The management remove operation runs on core 1 and is typed by LE address type:

```text
cancel matching pending LE connect
  -> disconnect matching controller or management LE link
  -> find matching slot across full DB capacity
  -> gap_delete_bonding(type, address)
  -> clear JPLC/custom material if it names that identity
  -> publish success or already-gone
```

The public address-only firmware helper additionally drops a Classic link key of the same address.
Forget-one MUST NOT wipe unrelated peers.

## Wipe all

```text
set persistent admission lock in RAM
  -> close pairing and discovery; cancel pending LE connect
  -> clear pending Classic/LE admission latches
  -> terminate every stack-owned HCI link, including links outside project slots
  -> delete all Classic keys
  -> enumerate every LE slot and delete via GAP
  -> clear JPLC and all in-memory Switch 2 security/reconnect material
  -> persist JPLK after database mutation
  -> queue management and controller links for disconnect
  -> remain locked across reboot
```

Disconnect-complete events arriving after the wipe see the lock and cannot restart an admitted
handshake. Only an explicit pairing-window gesture clears `JPLK` and re-enables discovery.

The HCI-owner sweep is required in addition to profile-slot disconnects. Hardware showed that
slot-only teardown could stop input while a controller still presented as connected; “neutral
input” is therefore not evidence that wipe completed its active-link contract. See
[`../experiments/bluetooth-wipe-transport-retention-2026-08-20.md`](../experiments/bluetooth-wipe-transport-retention-2026-08-20.md).

## Install reset

The install marker erases trust before BTstack or discovery starts. Because that erase also removes
`JPLK`, core 1 receives a boot-local `install_reset_performed` fact and writes the lock into the new
empty TLV before radio admission. See [`PERSISTENCE.md`](PERSISTENCE.md).

This directly addresses the observed ambiguity:

```text
old bond erased
  + remote still remembers PicoSwitch2
  + firmware silently accepts fresh pairing
  = apparent reconnect after wipe/flash, but a newly created relationship
```

The corrected policy blocks the last step outside an explicit window.

## Management and controller isolation

Management and controllers share the LE bond DB but use separate live connection tables. Ordinary
teardown is per link. Global wipe includes both trust classes by design and disconnects both. A
management identity cannot write `JPLC`, so it cannot become a direct controller reconnect target.

## Wake and Keyboard + Mouse

Wake replay is a bounded radio mode, not trust creation. It restores the previous scan/inquiry
policy afterward. Keyboard + Mouse may contain two physical peers, but it remains one logical input
source; role loss preserves the survivor and reopens only the controlled discovery policy described
in [`keyboard-mouse-input.md`](keyboard-mouse-input.md).
