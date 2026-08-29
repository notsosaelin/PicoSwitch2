# Windows companion — the four remaining Phase 2 boundaries

**Status:** A, B and C **PASSED**. A and B each cost a defect on the first run
and were confirmed on the retest the same day. D outstanding.

**Build under test:** `windows/companion`. A/B/C were run against `9486c9d`;
the corrections are in the commit that records this result.
Run `./build.ps1 -App` and launch
`src/PicoSwitch.Companion.App/bin/x64/Debug/net9.0-windows10.0.22621.0/win-x64/PicoSwitch.Companion.App.exe`.

**Already settled, 2026-08-29:** the Phase 2 happy path. Discovery, Windows
pairing, the encrypted management session, the `info.id == "picoswitch"` gate,
firmware/personality reads, a complete five-peer inventory, and registry and
peer-history persistence. See `STATUS.md` § Windows companion.

Everything below needs a condition the happy path does not produce. The order is
cheapest-and-least-destructive first.

---

## Before starting

Open **Settings → Open Diagnostics** in the app and leave it there between steps.
Every experiment below reads that log; **Copy** puts the whole ring on the
clipboard. The transport now records the WinRT stage, `GattCommunicationStatus`,
the ATT error byte, the `HRESULT` and both trust facts on every failure — that
line is the evidence, not the message shown on the page.

---

## C. One management client, no churn

Cheapest, destroys nothing, so it runs first.

**Question.** Does the Windows app hold exactly one management client, with no
hidden connect/disconnect loop, across ordinary use?

**Why it is open.** `ManagementOwner` makes single ownership structural and
`AppInstance` redirects a second process, but neither has been observed against
an adapter. The Android defect this guards against — two live transports, with
Disconnect appearing to do nothing — was invisible from the app side and only
showed up in the adapter's own view.

**Setup.** UART on the adapter's GP0/GP1.

```powershell
pwsh -File tools/mgmt_watch.ps1 -Port COM11 -IntervalMs 1500 -RingEverySec 60 `
  -DurationSec 300 -OutputPath dumps/<name>.jsonl
```

**Sequence**, in the app, while the watch runs:

1. Connect to the adapter.
2. Refresh twice.
3. Navigate Adapter → Keyboard & Mouse → Amiibo → Gamepad → Adapter.
4. Minimise and restore the window.
5. Disconnect.
6. Connect again.
7. Stop the watch (Ctrl-C).

**Pass criteria.**

- `cble.client` is true exactly once per connection and false after Disconnect;
- `mgmt.disconnects` increments **once** for step 5, not repeatedly;
- no `disc.ctrl` / `disc.hci` increments during steps 2–4;
- steps 2, 3 and 4 produce **no** connection transitions at all;
- step 6 produces exactly one new session.

**Fails if** the watch shows churn during Refresh or navigation, or two clients
at any point.

### Result — PASS, 2026-08-29 16:13–16:18

Evidence: [`dumps/windows-phase2-oneclient-2026-08-29.md`](../../dumps/windows-phase2-oneclient-2026-08-29.md).

- **Maximum concurrent management clients: 1.** 21 lifecycle records in the
  adapter's own ring, all on a single handle `0x0040`, strictly alternating
  connect/disconnect with **zero** alternation violations. Two GATT owners would
  have produced a second connect with no disconnect between, or a second handle.
  Neither appears.
- **Refresh, navigation and window activity create no session.** From 16:13:06 to
  16:13:46 — two Refreshes, all five destinations, and a minimise/restore — the
  adapter reported no transition of any kind.
- **Disconnect actually retires the session.** `cble.client` went true → false
  with `mgmt.disconnects` incrementing exactly once.
- **Reconnect creates one new session**, `cble.client` false → true with a single
  `mgmt.connects` increment.
- **No hidden loop.** Every disconnect carries `cause=none`, an application-level
  teardown rather than a link failure, and `disc.ctrl` / `disc.hci` never moved
  during the entire run — so no HCI-level disconnect occurred at all. The
  inter-event gaps are irregular and human-paced (4 s, 7 s, 17 s, 22 s, 110 s); a
  reconnect loop would show a regular cadence and would not stop.

**Method note for whoever runs this next.** `mgmt_watch.ps1` at
`-IntervalMs 1500 -RingEverySec 60` produced an effective host sample gap of
~42 s, because each ring dump dominates the cadence. That is too coarse to ORDER
a connect/disconnect pair, and the host-side counters alone were inconclusive.
The adapter's own `btlife` ring has millisecond resolution and settled it. Read
the ring, not the poll deltas.

---

## A. Stale Windows pairing → `RepairRequired`

**Question.** What exactly does WinRT report when Windows still believes the
adapter is paired but the adapter has lost its management key — and does the
implementation reach `RepairRequired`?

**Background.** The adapter clears its Bluetooth bonds on every firmware install,
deliberately, so a stale controller cannot reconnect after a flash and make a
test result meaningless. That leaves a three-party relationship with only one
party cleared: Windows keeps its pairing and the app keeps its registry row.

Android surfaces the HCI disconnect reason, so the Kotlin side matches status
0x05 / 0x06 at the connect stage. **Windows does not expose HCI status to user
mode at all**, so the signature had to be reconstructed. The pre-test hypothesis
was that the refusal would surface at the ATTRIBUTE layer: Windows reports the
peer paired, the peer answered, and an operation against an encrypted attribute
returns `AccessDenied` or an authentication `HRESULT`.

**Method.** Flash the adapter as usual; leave the Windows pairing alone; press
Connect on the remembered row; copy the Diagnostics log.

### Result — HYPOTHESIS DISPROVEN, 2026-08-29 16:46–16:47

The classifier behaved exactly as written and did **not** fire. That is the
finding: the condition set could not describe what Windows actually does.

**Observed stage:** `services`, every time.
**Observed status:** `GattCommunicationStatus=Unreachable`.
**ATT error byte:** none.
**HRESULT:** none.
**Signature recognised:** no.
**Extra retries:** none. One fallback scan per attempt, as designed.

Reconstructed from the log, attempt by attempt:

| | attempt 1, 16:46:29 | attempt 2, 16:47:09 |
|---|---|---|
| Windows pairing state | `paired` | `paired` |
| `BluetoothLEDevice` resolved | yes, `paired=True` | yes, `paired=True` |
| direct service discovery | `Unreachable` after 8.0 s | `Unreachable` after 0.5 s |
| clean retry | not taken — correct, stage ≠ connect | not taken |
| fallback scan | **timed out**, 15 s | **found it**, 0.8 s, −43 dBm |
| scan-resolved device | never opened | opened, `paired=True` |
| its service discovery | — | `Unreachable` |
| `peerAnswered` at classification | false | **true** |
| `AdapterResetSignature` | false | false |

Why it returned false in both: `IsBondMismatch` required
`outcome == AccessDenied` or an authentication `HRESULT`. The outcome was
`Unreachable` and the HRESULT was `null` — because the status was *returned* by
`GetGattServicesForUuidAsync` rather than thrown, so `GuardAsync` never ran and
there was no exception to carry one. The ladder then continued, correctly, given
what it had been told to look for.

### Interpretation

**`Unreachable` at the services stage is the Windows manifestation of a
bond mismatch on this hardware.** Windows holds an LTK for a bonded peer and
encrypts the link on connection, before any ATT transaction exists. The reflashed
adapter has no matching key, SMP fails, and the link drops. The failure is
*below* the attribute layer, so there is no attribute-layer error to report, and
WinRT's word for "the operation could not complete over the air" is
`Unreachable`.

Confounders, and how each was excluded:

| Confounder | Excluded by |
|---|---|
| Stale Windows GATT cache | Source: the first service resolution of every session is `BluetoothCacheMode.Uncached` (`BleGattManagementTransport.OpenAsync`). A cache hit would also *succeed*, returning the pre-flash shape — it cannot return `Unreachable`. |
| Management gate disabled | `mgmt_access.h`: `mgmt_should_advertise` and `mgmt_accept_connection` require neither bonding nor the pairing window. Only command *writes* are gated, on `client_bonded && client_encrypted`. **No firmware rule refuses service discovery.** |
| Adapter refusing access outside its pairing window | Same firmware rules, plus boundary C: on 2026-08-29 16:13–16:18 an ordinary disconnect/reconnect with no double-tap produced a full management session. |
| Adapter absent or out of range | It was seen advertising at −43 dBm by the service-UUID-restricted watcher, and opened as a device, in the same attempt. |
| Transient GATT failure | Reproduced on two independently resolved device objects per attempt, across four attempts spanning ~2 minutes. |
| Adapter genuinely broken | An unpair plus re-pair at 16:48:06–16:48:12 produced a working session with no firmware change. |

**Confidence: the observable behaviour is Confirmed. The SMP-level mechanism is
Strong Evidence** — Windows exposes no HCI or SMP detail to user mode, so the
mechanism is inference. Nothing in the implementation depends on the inference
being right; the signature is written against the observations.

### Corrected signature

`AdapterResetSignature` now recognises two shapes. The attribute-layer shape is
retained unchanged — this hardware did not produce it, but another radio, driver
or Windows build may, and it costs nothing. The link-layer shape is new and
requires four facts together:

1. Windows still reports the peer as **paired**;
2. the **exact remembered address** was seen advertising by the
   service-UUID-restricted watcher inside this attempt;
3. the failure was `Unreachable` at a stage **after** the device resolved;
4. that happened on **two independently resolved device objects** — the direct
   connect and the fresh scan-resolved connect.

`Unreachable` alone is deliberately NOT the signature. A powered-off adapter
produces exactly the same status, and fact 2 is what separates them: an absent
adapter cannot be observed advertising. Fact 4 stops a single transient link
failure against a present adapter from offering to destroy a working pairing.

### Consequence for the ladder — a corrected expectation

The pre-test method for this experiment said the pass criterion was
`RepairRequired` on the **first attempt** with **no fallback scan**. That
expectation was wrong, and it was wrong for a reason worth keeping:

- the attribute-layer shape is conclusive at the first failure and still ends
  the ladder immediately, with no retry and no scan;
- the **link-layer shape cannot be conclusive at the first failure**, because
  one `Unreachable` is indistinguishable from a momentary link failure. The
  fallback scan is what produces the corroborating second observation, so the
  ladder must run it and classify afterwards.

So the corrected criterion is: **`RepairRequired` on the first attempt whose
fallback scan observes the adapter.** In the captured run that is attempt 2 —
attempt 1's fallback scan timed out, plausibly because the preceding 8 s direct
attempt left the adapter not yet advertising again, though that is untested.

### Remaining unknown, and the instrumentation added for it

Whether the DIRECT connect can establish presence on its own, which would allow
first-attempt classification without waiting for an advertisement. On a bond
mismatch Windows may well open the link and have it drop at encryption, which
would leave a readable `BluetoothLEDevice.ConnectionStatus` / `GattSession`
state — but what those properties read at that exact moment has **not** been
measured, and encoding a guess is how the first version of this signature came to
be wrong.

The transport now logs, on any non-success service discovery:

```
[ble] link status=<...> connection=<Connected|Disconnected> session=<...> maxPdu=<n>
```

Diagnostic only. **Promotion criterion:** if a repeat of this experiment shows
`connection=Connected` at the moment of a services-stage `Unreachable`, and
`connection=Disconnected` for a genuinely absent adapter, then presence can be
established from the direct attempt and fact 2 may accept it in place of an
advertisement. Until then it stays out of the signature.

### Retest — PASS, 2026-08-29 17:40

Same conditions, corrected build. The **first Connect press after the flash**
reached `RepairRequired`, 2.4 s from press to diagnosis:

```
17:40:21.310 open device resolved paired=True
17:40:22.333 link status=Unreachable connection=Connected session=Closed maxPdu=23
17:40:22.334 fail stage=services GattCommunicationStatus=Unreachable
17:40:22.336 prepare reason=scan-fallback attempt=3 priorRetired=True
17:40:22.734 open device resolved paired=True
17:40:23.682 link status=Unreachable connection=Connected session=Closed maxPdu=23
17:40:23.682 fail stage=services GattCommunicationStatus=Unreachable
17:40:23.688 WARN [repair] ... The adapter was reset and no longer recognises this pairing.
17:40:23.688 ERROR [connect] failed [...] [paired=True observed=True answeredGatt=False
                                          linkFailures=2/2 -> BOND MISMATCH]
```

Reproduced identically on a second press (attempt 4). No clean retry ran, which
is correct — the failure stage is not retryable.

**Every predicate resolved as the corrected model predicts:** `paired=True`
(Windows kept its bond), `observed=True` (the restricted scan saw this exact
address), `answeredGatt=False` (nothing ever answered at the attribute layer —
the old signature's entire basis, absent again), `linkFailures=2/2` (the direct
and scan-resolved devices agreed).

### The mechanism, now measured rather than inferred

The `link` line was added specifically to settle this, and it did, four times
identically:

```
link status=Unreachable connection=Connected session=Closed maxPdu=23
```

- **`connection=Connected`** — Windows established the LE link. The adapter is
  physically present and reachable, and the DIRECT attempt can prove it without
  waiting for an advertisement.
- **`session=Closed`** — the `GattSession` is closed despite
  `MaintainConnection = true`. The link exists; GATT over it does not.
- **`maxPdu=23`** — the default ATT_MTU. **No MTU exchange ever completed**,
  which means no ATT transaction of any kind occurred.

A link that comes up and carries no ATT traffic at all is exactly the shape of
encryption failing immediately after connection. The interpretation
"Windows encrypts a bonded peer before any ATT transaction; the reflashed adapter
has no matching key; the link is unusable" is now supported by direct observation
of all three properties rather than by reasoning from the API surface alone.

Windows still exposes no SMP or HCI detail, so *"SMP specifically"* remains
inference. **Confidence: Confirmed** for the observable behaviour and the
compound signature; **Strong Evidence** for the SMP-level cause. Nothing in the
implementation depends on the latter.

### The open unknown — criterion fully met, promotion declined anyway

The promotion criterion was written before the retest and had two halves. Both
have now been observed:

| Half | Result |
|---|---|
| `connection=Connected` during a services-stage `Unreachable` against a present, reflashed adapter | **met**, 4 observations, 17:40 |
| `connection=Disconnected` for a genuinely absent adapter | **met**, 17:52:42, adapter powered off |

So `BluetoothLEDevice.ConnectionStatus` **is** a valid presence discriminator on
this platform. The direct connect alone can establish that the peer is physically
there, without waiting for an advertisement.

**It is still not going into the signature, and this is not an oversight.**
Working through what promotion would actually change, against every attempt
observed across both sessions:

| Attempt | With `ConnectionStatus` in the signature |
|---|---|
| 17:40 att. 3 and 4 (reflashed, present) | already classified on the first press — no change |
| 16:46 att. 1 (reflashed, direct 8.0 s, scan timed out) | presence not established either way; only ONE resolved-device failure, so fact 4 still fails — no change |
| 16:47 att. 2 (reflashed, present) | already classified — no change |
| 17:52 (powered off, `Disconnected`) | presence correctly absent — no change |

The binding constraint is fact 4, the two-independently-resolved-devices
corroboration, and presence is not what limits it. Promotion would buy nothing
that has ever been observed.

Relaxing fact 4 *given* proven presence is the tempting next step — "the link
came up and GATT was unusable, that is conclusive" — and it is exactly the move
to refuse. A transient discovery failure on a perfectly healthy adapter produces
the identical shape, and the cost of a false positive is offering to destroy a
working pairing. One observation of `Connected` during a genuine mismatch does
not establish that `Connected + Unreachable` is conclusive.

`ConnectionStatus` therefore stays **diagnostic only** — but it is now a
*confirmed* diagnostic rather than a speculative one, which is what makes the
`link` line worth reading in any future investigation.

### A timing observation that closes the last loose end

Direct-connect durations, across both sessions:

| Condition | Direct connect before `Unreachable` | `connection` |
|---|---|---|
| powered off, 17:52 | **8.0 s** | `Disconnected` |
| reflashed, first run att. 1, 16:46 | **8.0 s** | not instrumented |
| reflashed, present, 17:40 | 1.0 s | `Connected` |
| reflashed, present, 16:47 | 0.5 s | not instrumented |

Eight seconds is Windows failing to establish a link at all; sub-second is a link
that came up and then became unusable. That retroactively explains the one thing
left unexplained from the first run: **attempt 1 at 16:46 was not a missed
diagnosis.** The adapter had just been reflashed and was not yet advertising, so
Windows never got a link and the fallback scan timed out for the same reason.
`observed=False` was the correct answer to the question actually being asked.

Recorded as an observation only. Nothing is built on it: n is small and timing is
environment-dependent.

**Status: PASS.** Signature confirmed against hardware in both directions — it
fires on a genuine mismatch and does not fire on an absent adapter.

---

## B. Repair action

### Result — DEFECT FOUND, 2026-08-29 16:47:19 and 16:47:24

Repair did not repair anything. Both presses produced:

```
WARN [repair] no Windows device path cached for 88:A2:9E:D1:77:78; pair again to re-establish trust
```

and then **cleared the repair flag and reported success**, leaving the stale
Windows bond exactly where it was.

**Root cause.** Repair resolved the adapter's Windows pairing object through
`AdapterRecord.DeviceId`, a WinRT device path cached on the registry row.
Nothing ever populated it. The single construction site,
`AdapterRecord.Of(address, displayName)`, left the parameter at its `null`
default, and no other code path ever assigned one — so `record.DeviceId is { }`
was false for every row that has ever existed, and Repair took its "no device
path" branch every time.

The transport was resolving the same adapter successfully throughout the same
run — `open device resolved paired=True`, four times — from its **address**, via
`BluetoothLEDevice.FromBluetoothAddressAsync`. Repair was the only code that
insisted on a path.

**Why the tests did not catch it.** A repair test existed and passed. It
asserted that the row kept its alias and its address afterwards, and both held
perfectly — because nothing had happened to them. It never asked whether Windows
had been told anything, because there was no seam through which to ask.

### Fix

- `WindowsAdapterPairing.UnpairByAddressAsync` resolves the paired device fresh
  from the Bluetooth address and calls `DeviceInformationPairing.UnpairAsync`.
  No live management session, no cached state, no discovery pass.
- It returns `AdapterUnpairResult` rather than a bool, because "Windows could not
  resolve the device", "there was nothing paired" and "the pairing was removed"
  are three different answers and only two of them may clear the repair flag.
- Repair **verifies**: it re-reads the pairing state afterwards and refuses to
  report success if Windows still says paired.
- On any failure the repair flag **stays set** and the real reason is reported.
- `AdapterRecord.DeviceId` and `AdapterRelationship.DeviceId` are removed
  outright. A field nothing writes is not a cache, it is a trap. Documents
  written before this change may still carry a `"device"` key; it is ignored on
  load, and a regression test pins that such a document still opens.
- `IAdapterPairingGateway` puts a seam under the three Windows pairing calls the
  service makes, so the unpair can be asserted rather than assumed.

### Retest — PASS, 2026-08-29 17:40

```
17:40:45.897 INFO [repair] unpair 88:A2:9E:D1:77:78: removed
```

The line that was entirely absent from the first run. `UnpairAsync` executed
against a device resolved from the **address**, with no live management session
and no cached path, and Windows confirmed the removal.

**Independently corroborated by what happened next**, which is stronger than the
log line itself. Compare the two runs at the same point:

| | first run, broken repair | retest, fixed repair |
|---|---|---|
| after Repair, Pair reports | `pairing=paired` | `pairing=unknown` → NotPaired |
| pairing ceremony | **skipped entirely** | ran |
| physical double-tap | not required | **required** — `TimedOut` at 17:40:51, then `Paired` at 17:40:55 |
| result | `services Unreachable`, loop | connected, identity/firmware/personality read |

The bond really was gone: the adapter demanded its physical pairing window again,
which it only does for a NEW bond (`mgmt_accept_bonding`). A no-op could not
produce that.

**Status: PASS.**

### Two adjacent findings from the same log

**Pair reuses a stale OS bond without saying so** (16:47:26, :31, :41). With
Windows still holding a bond, `DeviceDiscovered` returns `Connect` and the
pairing ceremony is skipped entirely — `WindowsAdapterPairing.PairAsync` is never
called. Four Pair presses therefore ran no ceremony at all and failed the same
way each time. Attempting the connection is still correct (the existing pairing
may be perfectly good), but the flow now says plainly that no ceremony ran, and a
stale bond met through Pair reports `StalePairingMessage`, which names a way out.
Reached through Pair there is no remembered row and therefore no Repair button,
which is why the message differs from the remembered-adapter one.

**Remove is local-only, and that is correct** (16:47:39). The log line
`removed adapter 88:A2:9E:D1:77:78` was immediately followed by a first-pair that
found Windows still paired, which reads as though Remove had failed. It had not:
Remove is defined as dropping the app's relationship without touching Windows
trust or the adapter's own bonds (WINDOWS_PASS.md §19.5), and the UI copy already
said so. **Behaviour unchanged.** The diagnostic line now says what it did not
do, and the semantics are pinned by test.

**Physical pairing authorisation is Confirmed** (16:47:56 → 16:48:12). With the
window shut, `AuthenticationTimeout` and the double-tap instruction. With it
open, `Paired`, then identity, firmware and personality. This matches
`mgmt_accept_bonding` = `enabled && pairing_window_open` exactly, in both
directions, and it is the security property that matters most here.

---
## D. A → B → A adapter handoff

Needs a second PicoSwitch2. Skip if one is not available and record that.

**Question.** Does the ordered handoff hold under real asynchronous callbacks?

**Why it is open.** The ordering is unit-tested over a fake port. What that
cannot show is whether a real trailing callback from A can reach B's state.

**Method.**

1. Pair adapter A. Refresh. Give it the alias `A`.
2. Pair adapter B. Refresh. Give it the alias `B`.
3. From B, press **Connect** on A's row. Watch the banner during the switch.
4. Refresh. Confirm the snapshot is A's.
5. Press **Connect** on B's row. Refresh.

**Pass criteria.**

- both rows remain remembered throughout;
- exactly one live session at any moment;
- the banner never shows B's name against A's snapshot, or vice versa;
- aliases and peer histories stay separate and attached to the right adapter;
- neither switch unpairs or forgets the other adapter at the OS level;
- switching back works.

**Result:** _not yet run_

---

## Recording results

Fill in each **Result** above with what was observed, then update:

- `STATUS.md` § Windows companion — move whatever is settled out of the
  "specifically unproven" list, and keep anything that is not;
- `windows/companion/docs/README.md` §7 — the same, in the two tables;
- `AdapterResetSignature`'s doc comment — if A produced different values, the
  Hypothesis marker comes off and the real condition set goes in, with tests.

**A, B and C have passed.** Phase 2's management half is hardware-qualified on a
single-adapter setup.

D is required before a second adapter is advertised as supported, but does not
block Phase 3 on a single-adapter setup.

## What is still open after A, B and C

Neither blocks Phase 3.

1. **The recovery ladder's retry and 350 ms backoff have still never executed on
   hardware.** Every failure observed across both sessions was at a
   non-retryable stage, so the clean retry has correctly never been taken. It
   needs a *connect-stage* failure to exercise, which no natural scenario has
   produced yet.
2. **D**, the A → B handoff, which needs the second unit.
