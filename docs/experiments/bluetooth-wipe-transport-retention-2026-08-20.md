# Bluetooth wipe transport retention — 2026-08-20

Status: **Confirmed physical failure** on the pre-correction build; the transport-owner correction
passed the strict physical Xbox Elite Series 2 retest on `c6d53e7`.

## Question

Does triple-tap wipe terminate the active Bluetooth relationship itself, rather than merely erase
local trust and stop input forwarding?

## Observation

The maintainer tested the newest supplied build after the trust/admission hardening. After a
triple-tap wipe, the controller still presented as connected to the adapter, but controller input
was no longer forwarded.

The controller was later identified as an Xbox Elite Series 2. A byte-level trace and UF2 artifact
hash were not retained, so do not infer more detailed transport behavior from this observation.

## Result

The physical acceptance criterion failed. The result distinguishes two boundaries:

- trust/input invalidation took effect, because input stopped;
- active-link teardown did not produce the required externally disconnected state.

This does not prove that replacement trust was created. It proves that “no input” is insufficient
evidence for wipe completion and that active transport teardown must be independently enforced.

## Source finding

Wipe closed fresh admission, stopped scan/inquiry, disabled Classic page/discovery, erased the
Classic and LE stores, and then asked only project-tracked Classic/BLE controller slots to
disconnect. Those slot tables are not the HCI owner's complete connection inventory. A raw,
in-flight, management, SCO, or otherwise unrepresented HCI link can therefore escape a slot-only
sweep while later admission gates prevent it from becoming an input source. That source shape
matches the observed “connected, no input” split.

## Correction

After establishing lockout and disabling radio admission, wipe now calls pinned BTstack's
`hci_disconnect_all()`. This walks the stack-owned connection list and schedules every HCI link for
disconnect before trust deletion completes. Existing per-slot teardown remains for profile cleanup;
it is no longer the only transport boundary.

## Validation state

- physical failure on the earlier build: **Confirmed**;
- controller family: **Xbox Elite Series 2**;
- byte-level transport trace and earlier UF2 hash: **Not retained**;
- source ordering and regression guard: **Source-tested**;
- production board builds: recorded in [`../bluetooth/VALIDATION.md`](../bluetooth/VALIDATION.md);
- corrected `c6d53e7` physical result: **Confirmed pass** for the strict Elite 2 discriminator.

The retest discriminator is strict: after triple tap, the controller must enter its disconnected
state and remain unable to establish a link until an explicit pairing window is opened. Input
remaining neutral is necessary but not sufficient. The `c6d53e7` retest met this discriminator.
This single-family result confirms the corrected HCI-owner boundary without claiming the broader
controller-family wipe/flash matrix is complete.
