# windows-hogp-bridge-feasibility — the Phase 6 gate

**Date:** 2026-08-31
**Status:** **Partial.** B1 and B2 are answered and both **pass**. B3–B6 are
**unmeasured**, blocked on a failure `WINDOWS_PASS.md` §14.5 did not anticipate.
The package-identity explanation for that failure has since been **falsified**;
the radio/driver explanation stands unrefuted and untested on a second radio.
Phase 6 remains gated. Phase 6a is not, and is unaffected.

## Question

Can a Windows PC present the 161-byte PicoSwitch Bridge HID report descriptor as
a BLE HID-over-GATT peripheral, such that the firmware's
`android_bridge_identify()` matches it and enables the v2 capability block?

## Background

`WINDOWS_PASS.md` §14.2 rejects Path A (Classic HID Device) with evidence: L2CAP
is unreachable from user mode and PSMs `0x11`/`0x13` are owned by the in-box
stack. §14.3 makes Path B — HOGP peripheral — the primary candidate and lists six
open questions, of which §14.3 names **B3 the decisive one**: HOGP mandates the
Device Information Service, Windows blocks applications from publishing it, and
if BTstack's `hids_client` insists on DIS then Path B is dead on Windows without
a firmware change that is out of scope.

The interesting result below is that **the experiment never got as far as B3.**

## Method

`tools/hogp_probe/` — a standalone console application, outside
`PicoSwitch.Companion.sln`, staged so each step answers exactly one question and
a negative names a layer rather than a symptom. It project-references
`PicoSwitch.Bridge.Core` so the report map is the same bytes
`tools/check_android_descriptor_parity.py` verifies across C, Kotlin and C#; a
pasted copy would have made the experiment prove nothing about the descriptor the
product ships.

Re-runnable per radio, as §14.5 requires:

```powershell
cd tools/hogp_probe
dotnet run -c Release -- --probe-only                     # B1, B2, advertising
dotnet run -c Release -- --advertiser-control             # can this radio advertise at all?
dotnet run -c Release -- --probe-only --control-service   # is the objection to HID specifically?
dotnet run -c Release -- --probe-only --plain             # is the objection to encryption?
dotnet run -c Release -- --probe-only --no-discoverable   # …to discoverability?
dotnet run -c Release -- --probe-only --not-connectable   # …to connectability?
dotnet run -c Release -- --seconds 120 --out findings.json  # full run, B3–B6
```

## Environment

| | |
|---|---|
| OS | Windows 11 Pro, 10.0.26200 |
| Radio | Intel Wireless Bluetooth, `USB\VID_8087&PID_0032` |
| Bluetooth driver | Intel 24.40.10.8, dated 2026-06-07 |
| Radio address | `14:18:C3:47:C4:89` |
| `IsLowEnergySupported` | true |
| `IsCentralRoleSupported` | true |
| `IsPeripheralRoleSupported` | **true** |
| `IsAdvertisementOffloadSupported` | true |
| `MaxAdvertisementDataLength` | 160 |
| Probe process | run BOTH unpackaged and under a package identity declaring `runFullTrust` + `bluetooth` |
| Report descriptor | 161 bytes, sha256 `f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054` |
| Bridge contract | 4 |
| Adapter firmware | `71373293+dirty`, `bridge_contract` 4 |
| Live LE links during the run | five (two Xbox controllers, an 8BitDo keyboard, an OhSnap MCON I, the adapter) |

## Results

### B1 — `GattServiceProvider.CreateAsync(0x1812)`: **PASS**

```
B1.error = Success
```

Windows published the HID service. §14.3's stated worry — that the reserved-service
list is described as "at this time" and that Windows runs its own HID host — did
not materialise on this build. **This is a positive result and it retires B1.**

### B2 — the `0x2908` Report Reference descriptor: **PASS**

Every characteristic in the mandatory HOGP set was created, including both Report
characteristics and, on each, a `0x2908` Report Reference descriptor
(`01 01` input/report-1 and `02 02` output/report-2):

- Report Map `0x2A4B` (read), carrying the 161 bytes
- Report `0x2A4D` input (read + notify) + `0x2908`
- Report `0x2A4D` output (read + write + write-no-response) + `0x2908`
- HID Information `0x2A4A` (read) — `11 01 00 02`
- HID Control Point `0x2A4C` (write-no-response)
- Protocol Mode `0x2A4E` (read + write-no-response) — Report mode

**B2 is retired.** Reserved descriptors are creatable here.

### The blocker: connectable advertising is refused

`GattServiceProvider.StartAdvertising` never advertises on this machine.

```
attempt 1: Aborted    (AdvertisementStatusChanged: Aborted, Error = Success)
attempt 2: Aborted
attempt 3: Aborted
attempt 4: Aborted
attempt 5: Aborted
```

Five consecutive attempts, one second apart. Not a transient.

**The controls, one variable each:**

| Run | `IsConnectable` | `IsDiscoverable` | Service | Protection | Result |
|---|---|---|---|---|---|
| baseline | true | true | HID `0x1812` | Encryption | **Aborted** |
| control service | true | true | random 128-bit | Encryption | **Aborted** |
| plain | true | true | HID `0x1812` | Plain | **Aborted** |
| control + plain | true | true | random 128-bit | Plain | **Aborted** |
| not discoverable | true | false | HID `0x1812` | Encryption | Created (never starts) |
| not connectable | false | true | HID `0x1812` | Encryption | Created (never starts) |
| neither | false | false | HID `0x1812` | Encryption | Created (never starts) |
| **advertiser control** | — | — | *no GATT service;* `BluetoothLEAdvertisementPublisher` with manufacturer data only | — | **Started** |

Two things follow, and both are load-bearing:

1. **The refusal is not about HID.** A meaningless 128-bit service with no
   assigned meaning aborts identically. Nothing here is evidence against Path B's
   premise.
2. **The radio can transmit an LE advertisement.** The plain publisher reaches
   `Started`. What is refused is specifically a **connectable** advertisement from
   a `GattServiceProvider`.

`Error = Success` on the abort is itself informative: Windows is not reporting a
policy denial, a permission failure, or a radio-off condition. It stopped the
advertisement without an error to give.

### B3, B4, B5, B6 — **unmeasured**

Not "failed". No client can connect to an advertiser that never advertises, so
the adapter was never given the chance to discover the PC, read the report map,
or reject it over the absent DIS. Recording these as failures would be exactly the
promotion of an assumption into a fact that this project's evidence standards
forbid — and it would send the schedule to Path C on no evidence at all.

The firmware's own verdict (`bridge` over UART) was therefore not read, because
there is nothing yet for it to report on. `calls` would necessarily be 0, and a
`calls == 0` reading here would mean only "no connection happened".

## Interpretation

Two explanations remain, and they lead to **different product answers**. Neither
is currently supported over the other.

**H1 — package identity.** The probe is an unpackaged console application with no
manifest and therefore no declared `bluetooth` capability. The shipping companion
is a packaged WinUI 3 app whose `Package.appxmanifest` declares exactly that
capability, with a comment already anticipating this use. Windows gates several
peripheral-role behaviours on app identity. If this were the cause, **Path B
would be alive** and the only consequence would be that Controller Link requires
the packaged build — which would matter, because §33.2 requires the unpackaged
build to run too.

*Predicted against H1 before testing:* `GattServiceProvider.CreateAsync` and
`CreateCharacteristicAsync` both succeeded from the same unpackaged process,
through the same broker; an identity gate permitting a GATT server to be
published but not advertised would be an odd boundary. **That prediction held —
see below.**

**H2 — radio or driver.** `IsPeripheralRoleSupported` reports true, but that
property describes the radio's claimed capability, not the driver's willingness
to accept a connectable advertisement in the current state. This machine held
four live LE links during every run, and Intel parts have finite simultaneous
LE link plus advertising-set budgets.

*Against H2:* five attempts over five seconds all aborted identically, which
looks more like a policy than a resource race — though the link count did not
change between attempts, so the two are not separated.

### H1 was tested, and is FALSIFIED

Developer Mode was enabled and the **identical executable** was registered under
a minimal package identity — `tools/hogp_probe/package/AppxManifest.xml`,
declaring `runFullTrust` and the `bluetooth` device capability, the same
capability the shipping companion declares — and launched through
`Invoke-CommandInDesktopPackage` so it ran inside that identity.

Identity was the only variable. The result is byte-for-byte the same:

```
B1.error                  = Success
advertisementStatus       = Aborted
advertisementTransitions  = [ "Aborted/Success" ]
```

**Package identity does not change the outcome.** H1 is dead, and with it the
comfortable reading that Controller Link would simply require the MSIX build.

*(A deliberately separate package, not the companion's: putting a lab probe in
the product's layout would contaminate what ships, and the identity question does
not care whose identity it is. The registration is per-user and was removed again
after the run — `run-packaged.ps1 -Unregister`.)*

### What remains

**H2 stands unrefuted.** The radio or its driver will not accept a connectable
peripheral advertisement, whatever `IsPeripheralRoleSupported` reports. Five live
LE links were present throughout (`8BitDo Retro 87 Keyboard X`, `OhSnap MCON I`,
two `Xbox Wireless Controller`, and the adapter), so the resource-budget form of
H2 is not separated from the outright-refusal form.

**What would settle it:**

- Run the identical probe on a **second radio**, per §14.5's own instruction that
  a single-machine result is not a product claim. A different vendor advertising
  successfully falsifies H2 for that radio; a second Intel part failing the same
  way strengthens it. **No second radio is available on this bench**, so this
  remains open.
- Re-run with the other LE peripherals disconnected, holding everything else
  fixed — separates the resource-budget form of H2 from an outright refusal.
- Try an Intel driver other than 24.40.10.8, since the abort carries no error and
  a driver-level refusal is invisible from user mode.

## Conclusion

**The Phase 6 entry gate is NOT met.** §31 requires that "the §14.5 experiment has
been executed and recorded under `docs/experiments/`, **and B1–B4 passed**". B1
and B2 passed; B3 and B4 were not reached.

**No §14.6 branch is taken.** The decision table maps outcomes to product
decisions, and every branch requires knowing *which* question failed. Escalating
to Path C now would commit a joint firmware + Windows pass on the strength of an
advertiser abort that is not even specific to HID, and that is now known not to
be about app identity either.

**One product requirement is already established, whatever B3 turns out to be.**
Controller Link must be gated on an ACTUAL advertising attempt, not on
`IsPeripheralRoleSupported`. That property reports true on the only radio tested
while every connectable advertisement aborts, so a capability check built on it
would offer the user a feature that cannot work and would report no reason.

**Phase 6a is unaffected and remains available.** Its entry gate is "None beyond
Phase 3" and §31 states explicitly that it is independent of §14's outcome:
layouts, profiles, composition, alignment, audit and persistence are entirely
local, and the work is shippable whether or not Controller Link exists on
Windows. §31 also states that Phase 6a and 6b still run even when the gate fails.

## Confidence

- **B1 pass — Confirmed.** Directly observed, reproduced across seven runs.
- **B2 pass — Confirmed.** Same.
- **Connectable `GattServiceProvider` advertising is refused on this radio, from
  an unpackaged process — Confirmed** for this machine, this driver and this
  build. Reproduced five times consecutively and across four parameter
  combinations, with a control service and a control advertiser.
- **H1 (package identity) — Disproven.** The identical executable under a package
  identity declaring `runFullTrust` and `bluetooth` produced the identical abort.
- **H2 (radio or driver) — Hypothesis.** The only surviving explanation, and
  unfalsified rather than confirmed: one radio is not a product claim, and the
  resource-budget form was not separated from an outright refusal.
- **B3, B4, B5, B6 — Unknown.** Not attempted, not failed.

## Negative knowledge preserved

`IsPeripheralRoleSupported == true` **is not sufficient evidence that a Windows PC
can advertise connectably.** It was treated as the peripheral-role precondition in
§8.3 and §14.3, and on this machine it reports true while every connectable
`GattServiceProvider` advertisement aborts. A future reader should not conclude
from that property alone that the peripheral path is available; the probe's
`--advertiser-control` and the connectable/discoverable matrix are what
distinguish the claim from the behaviour.

## Negative knowledge, second entry

**Package identity is not what blocks connectable peripheral advertising.** It is
the obvious first suspect — the probe is unpackaged, the product is not, and
Windows does gate peripheral behaviours on identity elsewhere — and it is wrong
here. A future reader should not spend the packaging work again on this symptom.
`tools/hogp_probe/run-packaged.ps1` reproduces the disproof in one command if the
question is ever reopened on different hardware.

## Remaining unknowns

- Whether another radio advertises connectably (H2), and how peripheral quality
  varies by vendor — §14.5 asks for this explicitly and one machine cannot answer
  it. **No second radio is available on this bench.**
- Whether the live-LE-link count affects the abort. Five links were up throughout.
- Whether a different Intel driver behaves differently. The abort carries no
  error, so a driver-level refusal is invisible from user mode.
- Everything B3 was designed to ask: whether BTstack's `hids_client` proceeds
  without DIS. This remains the decisive question for Path B and is **untouched**.

## Suggested follow-up

1. Re-run on a second radio of a different vendor — the single highest-value
   next step, and the only one that can move H2.
2. Re-run with the other LE peripherals disconnected, to separate the
   resource-budget form of H2.
3. Only then read `bridge` over UART for the B3/B4 verdict, and take the §14.6
   branch the evidence actually supports.
4. Independently of all of the above: when Phase 6 is eventually implemented,
   gate the Controller Link capability on a real advertising attempt rather than
   on `IsPeripheralRoleSupported`.
