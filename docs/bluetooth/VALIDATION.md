# Bluetooth validation and invariant traceability

Status: software gates passed on 2026-08-20 through Bluetooth hardening commit
`83bfc4139ca9d2d3f0524ab0bac3d6700c5f9047`; physical lifecycle validation is pending.

## Invariants

| ID | Contract |
|---|---|
| BT-INV-001 | Outside an explicit pairing window, a new controller trust relationship cannot be created. |
| BT-INV-002 | Valid existing trust may reconnect without opening a fresh-pair window. |
| BT-INV-003 | Forget-one removes only the named local relationship and its matching reconnect material. |
| BT-INV-004 | Wipe-all removes all usable local Classic/LE relationships, controller reconnect metadata and custom Switch 2 key material. |
| BT-INV-005 | Wipe lockout survives reboot and only an explicit pairing action clears it. |
| BT-INV-006 | LE deletion traverses sparse slots and preserves pinned BTstack resolving-list side effects. |
| BT-INV-007 | Ordinary RF loss does not destroy valid trust. |
| BT-INV-008 | Management teardown does not tear down an unrelated controller link. |
| BT-INV-009 | Controller teardown does not unnecessarily tear down management. |
| BT-INV-010 | Wake advertising restores coherent prior radio policy. |
| BT-INV-011 | Discovery ownership is policy-driven, not accidental call ordering. |
| BT-INV-012 | Project-owned Switch 2 reconnect material cannot resurrect an explicitly wiped relationship. |
| BT-INV-013 | Keyboard/mouse role loss preserves the survivor and permits controlled rejoin. |
| BT-INV-014 | Ordinary diagnostics never expose Bluetooth secret key material. |
| BT-INV-015 | Persistent BTstack mutation occurs only on the owning run-loop context. |
| BT-INV-016 | A Classic replacement key is not durable until matching authentication succeeds. |
| BT-INV-017 | An LE RPA may reach SM as a bounded reconnect candidate but never grants fresh trust. |
| BT-INV-018 | Raw radio slots cannot assert controller-ready connection truth or the solid owner LED. |
| BT-INV-019 | HCI state loss retires live source generations without deleting durable trust. |

## Automated evidence

| Gate | Result | Coverage |
|---|---:|---|
| `pwsh -File tools/run_mgmt_tests.ps1` | 21/21 passed | lifecycle policy, secret diagnostics, owner LED, sparse lookup, reconnect policy, management, KB/M and adjacent seams |
| compiled `build/host-tests/test_*.exe` inventory | 76/76 passed | complete currently built host-test set |
| `python tools/test_ns2_trace.py` | 5/5 passed | diagnostic trace parser regression |
| `build.ps1 pico_w,pico2_w` | both passed | production RP2040 and RP2350 firmware |
| install-reset marker verifier | both passed | marker present below `SIZE-6S` reserved persistence start |
| `git diff --check` | passed for checkpoint | whitespace integrity |

`test_ns2_bt_lifecycle` pins BT-INV-001/002/005/006/016/017 at the pure-policy layer, including a
bond in slot 15 with only two active entries. `test_ns2_owner_led` pins BT-INV-018 and elapsed-time
cadences. `test_bluetooth_secret_diagnostics.py` guards BT-INV-014 at the source surface. Board
builds prove the event wiring compiles against the pinned BTstack APIs. They do not prove RF timing
or controller behavior.

## Required physical matrix

Use one coherent firmware build. Record board, controller firmware, PicoSwitch2 build identity,
flash method, timestamps, pre/post `btstate`, `btbonds`, `btreconn`, and relevant `btlife` records.

Run at least these controller families where hardware is available:

- genuine Switch 2 Pro Controller custom LE;
- a standard LE HID controller (Xbox family is preferred);
- a Classic SSP controller (DualSense/Edge);
- a legacy-PIN Classic controller (Wiimote/Wii U Pro) if available;
- the validated Bluetooth keyboard + mouse pair;
- one bonded management client for coexistence.

### Wipe with peer on

1. Pair and confirm input plus a bond.
2. Triple-tap while connected.
3. Confirm immediate disconnect, `pairing.lockout=true`, zero applicable trust and no target.
4. Wait beyond normal reconnect intervals; confirm no reconnect or new bond.
5. Reboot and repeat the observation.
6. Power-cycle the controller; confirm it still cannot create trust.
7. Open one explicit pairing window and confirm re-pair/input succeeds.

### Wipe with peer off

1. Pair, record trust, then power the controller off.
2. Wipe, reboot and record clean state before the remote returns.
3. Power it on with the window closed; confirm rejection/no new bond.
4. Open a window and confirm clean recovery.

### Firmware-update semantics

1. Pair, record state, and power the peer off.
2. Flash the release UF2 path that rewrites the pending marker.
3. Boot and capture state before returning the peer.
4. Confirm the six-sector reset contract, lockout, and no automatic trust creation.
5. Repeat after an explicit wipe.

Also document one debug/programmer flash that intentionally does not rewrite the marker, if that
workflow is used, so preservation behavior is not confused with the release UF2 contract.

### Reconnect regression

For each family, fresh-pair if needed, verify input/output, power the controller off normally, and
use its ordinary wake action. Confirm encrypted reconnect, initialization, input, output/LED/rumble,
and motion/audio where that controller already has validated support. Run repeated cycles for the
Switch 2 Pro Controller to protect HOME reconnect.

### Coexistence and Keyboard + Mouse

- Keep controller input live while management connects, disconnects normally, reconnects, and loses
  power abruptly. Neither side should unnecessarily tear down the other.
- Confirm global wipe intentionally removes the management bond too.
- With keyboard + mouse, remove/power off each role independently, confirm the survivor remains the
  logical source, then confirm controlled bonded rejoin.
- Trigger wake advertising while applicable and confirm discovery policy returns coherently.

## Evidence classification

- **Confirmed** requires a recorded physical result or direct reproducible capture.
- **Source-tested** means host tests and/or board builds passed without physical RF validation.
- **Strong Evidence** is a pinned-source conclusion whose exact runtime edge was not directly
  observed.
- **Unknown** means the matrix has not answered it.

The old “post-wipe automatic readmission confirmed” result remains historical evidence for its
workflow, but the later owner observation reopened the broader claim. Do not restore `Confirmed`
until the powered-on, powered-off, reboot and UF2 cases above pass with pre-admission bond snapshots.

## Risk register

| Risk | Severity | Current mitigation | Residual evidence gap |
|---|---|---|---|
| Wiped peer silently re-pairs | Critical | per-path admission latches, lockout, policy test | physical matrix |
| Sparse LE entry survives forget | High | capacity traversal and test | physical sparse fixture optional |
| Resolving list retains deleted peer | High | public GAP deletion | controller-level observation |
| Valid bond deleted on ordinary RF loss | High | narrow stale-security classification | repeated hardware reconnect |
| Management teardown affects controller | High | separate live tables | abrupt-loss hardware test |
| Wake leaves discovery incoherent | High | bounded save/restore policy | combined hardware test |
| KB/M lifecycle regresses | High | existing host suite | targeted role-loss regression |
| Flash semantics misunderstood | High | exact six-sector contract and procedure | release-UF2 hardware test |
| Classic replacement trust commits before authentication | Critical | deferred candidate plus matching auth-complete commit | Classic family hardware matrix |
| RPA accidentally becomes fresh trust | Critical | reconnect-only RPA admission and final protocol gate | privacy-enabled LE hardware matrix |
| Raw slot presents false connected LED | Medium | ready-count policy and deterministic timer test | passive/runtime observation |
