# PicoSwitch2 — Bluetooth Final Software Closeout

> Continue directly from the current Bluetooth work on `ns2-testing`.
>
> Current Bluetooth checkpoint:
>
> `c6d53e7` — **Terminate all Bluetooth links during wipe**
>
> Do **not** restart the Bluetooth audit from zero. Do **not** broaden scope. This is the final software-closeout pass before Bluetooth is frozen as a subsystem.
>
> The major Bluetooth architecture work is complete. The remaining task is to close four source-visible issues, rerun the established software gates, update the canonical documentation, push/verify the final checkpoint(s), and then stop.
>
> Do not mutate attached hardware unless explicitly authorized.

---

# 0. Current Ground Truth

Preserve the Bluetooth architecture and behavior already established by the completed passes, including:

- explicit fresh-pair admission;
- stale-vs-fresh trust separation;
- Classic replacement-key quarantine / commit-after-auth behavior;
- sparse LE bond traversal;
- public GAP deletion APIs;
- wipe/install pairing lockout;
- Switch 2 reconnect material cleanup;
- RPA treated as a reconnect candidate rather than identity;
- HCI transient-state cleanup;
- production diagnostic secret removal;
- raw-vs-ready Bluetooth diagnostics;
- wall-clock owner LED policy;
- authoritative `docs/bluetooth/SECURITY.md`;
- current validation/evidence taxonomy.

Do not reopen completed architecture unless one of the closure findings below proves it necessary.

---

# 1. Confirmed Wipe Failure and Corrected Acceptance Contract

A physical Xbox Elite Series 2 test disproved the previous slot-level wipe assumption.

## Confirmed failed behavior

The controller had been paired, powered off, the adapter was wiped, and the controller was then able to present itself as connected again while PicoSwitch2 correctly delivered zero input.

That proved:

```text
trust/input gating worked
but
radio/HCI teardown was incomplete
```

The previous assumption that disconnecting only links represented in PicoSwitch2's controller slot tables was sufficient is **refuted**.

This result must remain recorded as a confirmed physical failure, not softened into a partial pass.

## Correction now in `c6d53e7`

Wipe now:

```text
locks pairing admission
→ stops BLE scan
→ stops Classic inquiry
→ disables Classic discoverability/connectability
→ calls BTstack's hci_disconnect_all()
→ terminates every stack-owned HCI link, including raw/in-flight links
→ completes trust deletion
→ persists pairing lockout
```

The strict physical retest for `c6d53e7` has now passed:

> Triple-tap wipe forces the controller into a disconnected state and it remains unable to establish a Bluetooth link until the user explicitly opens a pairing window.

Treat this as **Confirmed hardware evidence** for the corrected transport-owner wipe boundary.

Keep the detailed experiment record under:

```text
docs/experiments/bluetooth-wipe-transport-retention-2026-08-20.md
```

Do not regress this behavior.

## Wipe invariant

The final wipe contract is:

```text
no usable trust
+
no live controller HCI links
+
no new controller admission
+
persistent lockout
```

A post-wipe controller must not merely have its input ignored; it must be unable to maintain or establish the Bluetooth controller session until explicit pairing is reopened.

---

# 2. Closure Finding A — Install Reset Lock Must Be One-Shot Per Boot

Current source-visible concern:

`config_install_reset_performed()` is a sticky per-boot fact.

The Bluetooth `HCI_STATE_WORKING` path may consult that fact every time the HCI stack re-enters `WORKING`.

Potential sequence:

```text
new UF2 boot
→ install-reset erases persistence
→ Bluetooth recreates pairing lock
→ user explicitly opens pairing
→ lock clears
→ later HCI state loss/restart in the same Pico boot
→ HCI_STATE_WORKING again
→ install_reset_performed() is still true
→ pairing lock is recreated unexpectedly
```

Required behavior:

> Install-reset Bluetooth bootstrap semantics MUST be applied exactly once for the boot in which the install reset occurred.

An unrelated later HCI restart MUST NOT reapply install-reset lockout after the user has legitimately reopened pairing.

Audit the current exact flow before editing.

Implement the smallest correct one-shot mechanism.

Add a host-testable policy seam if useful.

Required test cases:

```text
normal boot, no install reset
install-reset first HCI working transition
install-reset second HCI working transition in same boot
explicit pairing unlock after first bootstrap
later HCI restart preserves the current unlocked/locked policy state
```

Do not weaken the guarantee that the initial post-UF2 boot locks fresh pairing before discovery begins.

---

# 3. Closure Finding B — Wake Timer Must Be Removed Before Wake State Is Cleared

Current source-visible concern:

`wake_adv.timer` is an intrusive BTstack timer object.

The wake path registers it into the BTstack run loop.

The HCI transient-reset path may clear the entire `wake_adv` structure with `memset`.

Pinned BTstack timer lists retain pointers to registered timer objects until they are removed or fire.

Therefore HCI loss during an active wake operation can leave the run loop pointing at a timer object whose fields were zeroed/reset.

Required behavior:

> Before clearing or reinitializing `wake_adv`, any registered wake timer MUST be safely removed/cancelled using the pinned BTstack API.

Audit:

```text
wake_adv_arm()
wake_adv.timer lifecycle
normal wake completion
wake failure
HCI state loss during PREPARE
HCI state loss during BROADCAST
HCI state loss during BETWEEN
HCI state loss during RESTORE
subsequent wake after recovery
```

Ensure post-HCI recovery:

```text
wake_adv inactive
wake timer not registered
no stale callback can fire against reset state
ordinary address/radio ownership can reconstruct cleanly
a subsequent wake can arm normally
```

Add pure/source-level regression coverage where practical.

Do not broaden this into a wake redesign.

---

# 4. Closure Finding C — Already-Admitted Classic SSP Must Survive Pairing-Window Expiry

Current architecture correctly uses per-attempt fresh-admission latches.

Current source also scopes Classic SSP auto-accept/discoverability to the global pairing window.

Potential race:

```text
pairing window open
→ Classic SSP controller is admitted
→ pending_fresh_pairing_admitted = true
→ global pairing-window deadline expires
→ global SSP auto-accept is disabled
→ SSP confirmation arrives after deadline
```

That specific candidate was already explicitly authorized.

Required invariant:

> Closing the global pairing window MUST prevent new candidates, but MUST NOT revoke fresh-pair authority already latched for an in-flight Classic attempt.

Audit the pinned BTstack SSP event flow.

Prefer per-attempt confirmation policy over a purely global auto-accept switch if the pinned stack exposes the needed event/callback.

If global auto-accept must remain, defer disabling it only while an already-admitted Classic SSP attempt is actually in flight.

Do **not** leave SSP globally auto-accepted outside pairing.

Do **not** silently extend discoverability.

Required tests:

```text
window open → Classic candidate admitted → window still open → completes
window open → Classic candidate admitted → window closes → same candidate completes
window closed → new Classic candidate → rejected
post-wipe lockout → candidate → rejected
old trusted Classic reconnect outside window → allowed without creating replacement trust
stale old trust → replacement SSP outside window → rejected
```

Preserve legacy PIN behavior for Wiimote/Wii U Pro.

---

# 5. Closure Finding D — Owner LED Diagnostics Must Be Truthful

The current wall-clock LED policy is accepted.

Do **not** change the current idle cadence merely to match an older implementation.

Current accepted idle behavior:

```text
~90 ms ON
about every 10 seconds
```

The user's current physical observation matches that cadence.

KB/M has not regressed.

Non-controller BLE/raw ACL presence must continue to be distinct from controller-ready truth; management-only or raw non-ready Bluetooth connections must not make the owner LED falsely indicate a connected controller.

## `last_transition_ms`

Current docs describe:

```text
owner_led.last_transition_ms
```

as the wall-clock time of the last LED output transition.

Current implementation may instead publish the start time of the current LED reason/mode.

Those are not the same thing.

Required behavior:

> The diagnostic field name and documentation MUST match actual semantics.

Preferred result:

- track the actual last `on ↔ off` output transition timestamp;
- publish that value;
- keep a separate reason-start timestamp only if genuinely useful.

If actual output-transition tracking is not retained, rename the field/documentation so it is truthful.

Do not redesign LED cadence/patterns during this closeout.

At minimum regression-check:

```text
idle
pairing
wipe
config
mode acknowledgement
GameCube diagnostic patterns
controller-ready solid
management-only / raw-not-ready non-solid
```

---

# 6. Preserve Raw-vs-Ready Connection Truth

The owner LED correctly uses protocol-ready controller truth rather than raw ACL/slot occupancy.

Keep that.

Preserve diagnostics for:

```text
connections.classic_raw
connections.classic_ready
connections.ble_raw
connections.ble_ready
controller_connected
owner_led.reason
owner_led.on
disc.state_losses
```

The new `hci_disconnect_all()` wipe boundary must not become an excuse to stop auditing ordinary teardown correctness.

Re-check teardown paths only as necessary for this closeout:

```text
HCI disconnect
HID disconnect
authentication failure
failed HID open
connect timeout
HCI state loss
wipe
```

Do not launch another broad connection-lifecycle redesign.

---

# 7. Final Hot-Path Logging Check

Do one targeted scan for unconditional high-rate Bluetooth logging in production paths.

Focus on:

```text
advertisement callbacks
GATT notifications
HID report paths
raw packet paths
reconnect loops
timers
```

Keep useful transition/error logs.

Packet-rate diagnostics should be one of:

```text
NS2_DIAG gated
one-shot
rate-limited
counter-based
pull-based
```

Only fix clearly hot production paths that could perturb timing.

Do not turn this into a logging refactor.

---

# 8. Documentation

Update only canonical Bluetooth docs whose truth changes.

Likely:

```text
docs/bluetooth/SECURITY.md
docs/bluetooth/LIFECYCLE.md
docs/bluetooth/DIAGNOSTICS.md
docs/bluetooth/VALIDATION.md
docs/bluetooth/ARCHITECTURE.md
STATUS.md
```

Preserve the confirmed wipe experiment record.

Explicitly document:

- slot-level wipe teardown was physically disproved;
- `c6d53e7` moved wipe teardown to BTstack's complete HCI registry;
- strict Elite 2 post-wipe disconnected-state retest passed;
- install-reset lock bootstrap is one-shot;
- HCI recovery safely cancels transient wake timer/state;
- an already-admitted Classic SSP attempt survives global window expiry;
- current owner LED idle cadence remains ~90 ms every ~10 seconds;
- exact meaning of `owner_led.last_transition_ms`;
- raw/non-controller Bluetooth presence does not equal controller-ready LED truth.

Do not downgrade or erase negative evidence.

Do not falsely generalize one Elite 2 retest into every controller family.

---

# 9. Validation Gates

Current baseline after the wipe correction:

```text
Focused management/Bluetooth suite: 22/22
Compiled host tests: 76/76
Trace parser: 5/5
Pico W production build: PASS
Pico 2 W production build: PASS
Install-reset marker checks: PASS/PASS
```

The strict Elite 2 wipe retest on `c6d53e7` is physically passing.

After the final closure changes, rerun:

```text
focused management/Bluetooth suite
complete compiled host-test inventory
trace parser
Pico W production build
Pico 2 W production build
both install-reset marker checks
git diff --check
```

If tests are added, report:

```text
previous baseline
new total
```

Do not quote old results as newly run.

---

# 10. Git / Staging Rules

This workflow rule applies to all sessions:

> Do not create or use temporary `.patch` files for selective staging.
>
> Do not use `git apply` for selective staging.

Preserve unrelated user changes in shared files such as `STATUS.md`.

Stage only agent-owned hunks with normal Git-native selective staging, preferably:

```text
git add -p
```

or another non-patch-file Git-native index method.

Do not undo already-correct source changes merely to change staging technique.

Do not blindly stage untracked Android/user files.

Do not rewrite or force-push existing Bluetooth commits.

Use one or a few coherent green checkpoints for this closeout.

For each checkpoint:

1. run focused tests;
2. ensure build state is coherent;
3. inspect diff;
4. stage only relevant hunks/files;
5. inspect staged diff;
6. run staged diff whitespace check;
7. commit;
8. push;
9. verify exact remote SHA.

---

# 11. Hardware Rule

Do not flash or otherwise mutate attached hardware unless explicitly authorized.

The Elite 2 strict wipe retest has already provided the hardware evidence needed to accept the new HCI-registry teardown direction.

Do not repeat hardware experiments merely to satisfy process unless a final source change actually requires a targeted retest.

If a closure change affects only:

```text
install-reset one-shot
wake timer cancellation
Classic SSP window-expiry authority
LED diagnostic timestamp truth
```

state clearly which physical validation, if any, remains advisable.

Do not claim unperformed hardware validation.

---

# 12. Bluetooth Freeze Criteria

Bluetooth is ready to freeze when all of the following are true:

```text
wipe locks admission before teardown
wipe disconnects every BTstack-owned HCI link
post-wipe controller cannot establish a link until explicit pairing
confirmed Elite 2 failure remains documented as historical negative evidence
install-reset lock bootstrap applies once per boot
wake timer cannot remain registered when wake state is reset
already-admitted Classic SSP can finish after global pairing-window expiry
new Classic candidates remain blocked after window expiry
owner LED diagnostic timestamp is truthful
current ~10 s idle cadence remains deterministic
raw/non-controller BLE presence does not masquerade as controller-ready
hot production Bluetooth logging is bounded/gated where necessary
all automated gates pass
both production board builds pass
docs reflect source and hardware truth
remote branch contains exact final checkpoint(s)
```

Then stop Bluetooth subsystem development.

Do not start another Bluetooth architecture pass.

Future Bluetooth problems should be treated as ordinary targeted bugs/regressions unless new evidence justifies reopening the subsystem.

---

# 13. Final Report Format

## Bluetooth Software Closeout

### Starting point

```text
branch:
starting HEAD:
remote:
unrelated work preserved:
```

### Wipe correction status

```text
confirmed failed behavior:
root cause:
c6d53e7 correction:
physical retest:
final acceptance contract:
```

### Closure finding A — install reset

```text
root cause:
change:
test:
```

### Closure finding B — wake timer

```text
root cause:
change:
test:
```

### Closure finding C — Classic SSP

```text
root cause:
change:
test:
```

### Closure finding D — owner LED diagnostics

```text
idle cadence:
last_transition_ms final meaning:
raw/non-controller connection effect:
change:
test:
```

### Additional findings

Only evidence-backed items discovered while closing these remaining issues.

### Validation

```text
focused suite:
compiled host tests:
trace parser:
Pico W:
Pico 2 W:
install-marker checks:
diff check:
```

### Hardware

```text
Elite 2 wipe failure preserved: yes/no
Elite 2 corrected retest: pass/fail
additional hardware mutated: yes/no
remaining physical validation:
```

### Documentation

List changed canonical Bluetooth docs and the retained experiment record.

### Git

```text
new checkpoint SHA + subject
...
final HEAD:
remote verified:
ahead/behind:
tracked worktree:
intentionally untouched unrelated files:
```

Then stop.

---

# 14. Final Principle

Bluetooth should not remain open indefinitely because more theoretical edge cases can always be imagined.

At this point the closeout goal is:

```text
source truth
=
trust/admission truth
=
BTstack/HCI connection truth
=
PicoSwitch2 ready-state truth
=
diagnostic truth
=
owner LED truth
```

with the corrected wipe contract now physically demonstrated for the failure that originally contradicted the software model.

Once the remaining four source-visible closure items are fixed and the full software gates are green, freeze Bluetooth and move on.
