# Windows companion — the four remaining Phase 2 boundaries

**Status:** prepared, not yet run. Results go in this file.

**Build under test:** `windows/companion`, commit `9486c9d`.
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

**Setup.** UART on the adapter's GP0/GP1. The CP210x on `COM11` is present but
the adapter did not answer `btstate`, so the wiring or adapter power needs
attention before this runs.

```powershell
pwsh -File tools/mgmt_watch.ps1 -Port COM11 -IntervalMs 2000
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

**Result:** _not yet run_

---

## A. Stale Windows pairing → first-attempt `RepairRequired`

The important one, and the only remaining **Hypothesis** in the management half.

**Question.** What exactly does WinRT report when Windows still believes the
adapter is paired but the adapter has lost its management key — and does the
implementation reach `RepairRequired` on the first attempt?

**Background.** The adapter clears its Bluetooth bonds on every firmware install,
deliberately, so a stale controller cannot reconnect after a flash and make a
test result meaningless. That leaves a three-party relationship with only one
party cleared: Windows keeps its pairing and the app keeps its registry row.

Android surfaces the HCI disconnect reason, so the Kotlin side matches status
0x05 / 0x06 at the connect stage. **Windows does not expose HCI status to user
mode at all**, so the signature is reconstructed from three facts held together:
Windows reports the peer as paired, the peer answered, and an operation against
an encrypted attribute returns `AccessDenied` or an authentication `HRESULT`.
Deliberately not stage-restricted — the refusal may surface at service discovery,
the CCC write, or the first command.

**Do not** change firmware to accommodate Windows, weaken the security level, or
add retries to make this eventually pass.

**Method.**

1. Confirm the app connects normally. Disconnect.
2. Flash the adapter as usual (this clears its credentials by design).
3. Leave the Windows pairing alone — do **not** unpair in Settings.
4. In the app, press **Connect** on the remembered row. Once.
5. Copy the Diagnostics log.

**What to capture.** The `connect failed` line. It carries:

```
connect  failed [ (BOND MISMATCH) ] paired=<bool> peerAnswered=<bool>
         [stage=<connect|services|subscribe|command>
          GattCommunicationStatus=<...> ATT=0x<..> HRESULT=0x<........> <name>]
         <message>
```

plus the preceding `ble open device resolved paired=<bool>` and any
`ble fail ...` lines from the transport.

**Questions it answers.**

1. which WinRT stage the failure surfaces at;
2. the exact `GattCommunicationStatus` / ATT byte / `HRESULT`;
3. whether `AdapterResetSignature` recognised it — the `(BOND MISMATCH)` marker;
4. whether the relationship reached `RepairRequired` on the **first** attempt —
   the banner should say *Repair pairing*, and the row should show
   *repair required*;
5. whether any pointless retry or fallback scan ran — there should be **exactly
   one** `connect` attempt and **no** `scan-fallback` in the log.

**If the signature does not fire**, the log still names the real stage and
`HRESULT`. That is the finding: bring it back and the condition set gets
corrected from evidence, with a test pinning the real values. A wrong hypothesis
recorded honestly is worth more than a passing test.

**Result:** _not yet run_

**Observed stage:** _—_
**Observed status/HRESULT:** _—_
**Signature recognised:** _—_
**First-attempt `RepairRequired`:** _—_
**Extra retries or fallback scan:** _—_

---

## B. Repair action

Only reachable once A has produced the stale-bond state.

**Question.** Does the Windows repair flow restore a healthy relationship without
losing app-side state?

**Method.**

1. From the state left by A, press **Repair** on the row. Confirm the dialog.
2. Double-tap the adapter's pairing button when the app asks.
3. Press **Connect**.

**Pass criteria.**

- `UnpairAsync` removes the stale Windows pairing (Windows Settings no longer
  lists the adapter);
- the app never unpairs anything without that explicit confirmation;
- re-pairing uses `ConfirmOnly` + `Encryption` — Windows shows its own consent
  prompt and the app does not imitate it;
- `info.id == "picoswitch"` passes and the relationship reaches Connected;
- the row keeps its alias and its identity, and `repair required` clears;
- peer history for that adapter survives.

**Result:** _not yet run_

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

Phase 2 may be marked **hardware-qualified** only when A, B and C have passed.
D is required before a second adapter is advertised as supported, but does not
block Phase 3 on a single-adapter setup.
