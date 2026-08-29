# PicoSwitch2 Windows Companion — build, run, and where things are

Implementation of the design in `WINDOWS_PASS.md` at the repository root. That
document is the specification; this one is how you build what exists.

**Status: Phases 0–1 complete. Phase 2 implementation complete, hardware
validation pending. Phases 3–9 not started.**
The shell builds, packages and runs; 366 tests pass. **No part of this has been
executed against a PicoSwitch2 adapter.** Everything below that says "verified"
means verified in software, on this machine, by the tests or the run named.

---

## 1. What exists

| Project | Framework | State |
|---|---|---|
| `src/PicoSwitch.Bridge.Core` | `net9.0` | `Core/` and `Protocol/` complete (Phase 1). `Session/` and `Touch/` are Phase 6 / 6a. |
| `src/PicoSwitch.Management.Core` | `net9.0` | Complete (Phase 1). |
| `src/PicoSwitch.Companion.Windows` | `net9.0-windows10.0.22621.0` | Phase 2: `BleGattManagementTransport`, `WindowsBluetoothRadio`, `WindowsAdapterPairing`, the recovery/bond-mismatch policies, and the atomic document store. The input/motion/battery/output backends are Phase 6. |
| `src/PicoSwitch.Companion.Services` | `net9.0-windows10.0.22621.0` | Phase 2: `ManagementOwner`, `AdapterRepository`, `AdapterRegistry`(+codec, store), `AdapterRelationshipCoordinator`, `ActiveAdapterCoordinator`, `AdapterSwitch`, `PeerHistoryBook`(+codec, store), `ControllerInventory`, `DiagnosticLog`. |
| `src/PicoSwitch.Companion.App` | WinUI 3 | Phase 0 shell: navigation, Mica, custom title bar, single-instance activation, six placeholder pages. Builds x64 and ARM64, packages as MSIX, and runs — see §4. |

Tests: four projects, **239 passing**.

```
PicoSwitch.Bridge.Core.Tests            123
PicoSwitch.Management.Core.Tests        101
PicoSwitch.Companion.Windows.Tests       31
PicoSwitch.Companion.Services.Tests     111
```

---

## 2. Build and test

```powershell
cd windows/companion
./build.ps1                     # build + test everything except the WinUI shell
./build.ps1 -App                # additionally build the shell, unpackaged (x64)
./build.ps1 -App -Platform arm64
./build.ps1 -Msix               # produce an unsigned MSIX
```

Or directly:

```powershell
dotnet test tests/PicoSwitch.Management.Core.Tests/PicoSwitch.Management.Core.Tests.csproj
dotnet build src/PicoSwitch.Companion.App/PicoSwitch.Companion.App.csproj -p:Platform=x64
```

| Target | Needs |
|---|---|
| The four non-UI projects and all tests | **.NET 9 SDK only.** `global.json` pins the band. |
| The unpackaged WinUI shell | .NET 9 SDK plus the Windows App SDK build components |
| MSIX packaging | additionally **.NET Framework MSBuild** — see §4 |

Running the unpackaged build: `bin/x64/Debug/net9.0-windows10.0.22621.0/win-x64/PicoSwitch.Companion.App.exe`.
It is self-contained (`WindowsPackageType=None`), so it needs no deployment step.

---

## 3. The rules the build enforces

These are not review conventions. Each is a build failure or a red test.

| Rule | Mechanism |
|---|---|
| The two Core projects contain no Windows API reference | They target `net9.0`, so a Windows type is a **compile error** — the C# equivalent of keeping the Android SDK off `:bridge-core`'s classpath |
| Management and the bridge stay independent relationships | Neither Core project may `ProjectReference` the other (`ArchitectureGuardTests`) |
| The App project never opens a `GattCharacteristic`, builds a management command, or composes report bytes | `LayeringGuardTests` scans its sources |
| The Services project contains no WinUI type and no XAML | `LayeringGuardTests` |
| `ManagementProtocol` carries no BLE carrier mechanics | `ArchitectureGuardTests` — the same logical contract is spoken over UART |
| `Domain` carries no screen titles or colour packing, but keeps portable `PeerNaming` | `ArchitectureGuardTests` |
| The HID descriptor is byte-identical in C, Kotlin and C# | `tools/check_android_descriptor_parity.py` plus `BridgeContractTests` |
| A descriptor byte cannot change without a contract bump | The SHA-256 digest registry, checked from both the Kotlin and the C# side |
| The two face mappers are never collapsed into one | `ControllerLinkFaceMappingTests` asserts they can **never** resolve alike |
| The app declares exactly one capability (`bluetooth`) | `LayeringGuardTests` parses `Package.appxmanifest` as XML |

### Shared fixtures — the anti-drift mechanism

The C# side is a **Level 1** consumer (`WINDOWS_PASS.md` §9.4): it reimplements
the documented contract rather than linking Kotlin. What makes that safe is that
both languages read the **same files**, not copies:

| Fixture | Consumed by |
|---|---|
| `tools/fixtures/management/protocol-v1.json` | Kotlin `ProtocolConformanceTest`, C# `ProtocolConformanceTests` |
| `tools/fixtures/android_controller_hid.h` | firmware, the parity script, C# `BridgeContractTests` |
| `tools/fixtures/controller_link_face_mapping.csv` | Kotlin, the C golden test, C# `ControllerLinkFaceMappingTests` |
| `tools/fixtures/bridge_report_goldens.csv` | **new in this pass** — Kotlin `BridgeReportGoldenTest`, C# `BridgeReportGoldenTests` |

`bridge_report_goldens.csv` was generated from the Kotlin encoder and closes a
gap the descriptor guard could not: the descriptor proves both ends describe the
same report, and the goldens prove they **fill** it identically. 47 vectors,
covering every button, every hat direction including opposites cancelling,
clamping, both flag halves, and the timestamp wrap.

To confirm the guards actually bite, edit one descriptor copy and run
`python tools/check_android_descriptor_parity.py` — it names the first differing
byte and exits 1.

---

## 4. Toolchain notes for the WinUI shell

Both flavours build and the app runs. Two things about getting there are worth
recording, because neither is discoverable from the error you first see.

### `XamlCompiler.exe` reports XAML errors by exiting 1 in silence

On a machine missing the Windows App SDK build prerequisites, `dotnet build`
fails with nothing to go on:

```
Microsoft.UI.Xaml.Markup.Compiler.interop.targets(594,9): error MSB3073:
  ... XamlCompiler.exe ... exited with code 1
```

No stdout, no stderr, no `output.json`, no Windows Error Reporting entry — and
it still generates every `*.g.cs`, so it looks like a late-stage crash. It is
not: the XAML errors were real and the compiler simply could not report them.

**How to get the real message.** Force the alternative in-process code path:

```powershell
dotnet build src/PicoSwitch.Companion.App/PicoSwitch.Companion.App.csproj `
  -p:Platform=x64 -p:UseXamlCompilerExecutable=false
```

That path fails *loudly*. On this machine it named the missing dependency
(`System.Security.Permissions`), and after the SDK components were installed the
executable path itself started reporting properly — two genuine authoring errors
in this project's own XAML, which are now fixed:

| Error | Cause |
|---|---|
| `WMC0055: Cannot assign text value 'Bluetooth' into property 'Symbol'` | The WinUI `Symbol` enum has no `Bluetooth` member. Replaced with `FontIcon Glyph="&#xE702;"`, the system glyph. |
| `WMC9997: An error occurred while parsing EntityName` | A bare `&` in `Keyboard & Mouse`. XML needs `&amp;`. |

The lesson worth keeping: **a silent `XamlCompiler.exe` exit is a reporting
failure, not necessarily a tooling-only failure.** Run the in-process path before
concluding the environment is at fault.

### MSIX packaging needs .NET Framework MSBuild

`dotnet build -p:WindowsPackageType=MSIX` fails inside the Windows App SDK's own
packaging task:

```
error MSB4018: The "WinAppSdkValidateAppxManifestItems" task failed unexpectedly.
  System.IO.FileNotFoundException: Could not load file or assembly
  'System.Security.Permissions, Version=8.0.0.0'
```

The task is not shipped with that dependency and the .NET SDK's MSBuild cannot
resolve it; .NET Framework MSBuild can. `./build.ps1 -Msix` therefore locates
`MSBuild.exe` from a Visual Studio / Build Tools install and uses it, and throws
a clear message when there is none. This is a Windows App SDK packaging gap, not
a project defect, and it is not worked around by copying assemblies into the
NuGet cache — that fixes one machine and no other.

### `runFullTrust` is required, contradicting §27.3

`WINDOWS_PASS.md` §27.3 specifies the manifest declare "exactly `bluetooth`,
nothing else". That is not achievable, and the specification was written before
`MakeAppx` had been run against it:

```
error 80080204: App manifest validation error:
  Reason: The element or attribute or attribute value specified requires
  "runFullTrust" capability.
```

A WinUI 3 **desktop** app packaged as MSIX declares `Executable`/`EntryPoint`
under `<Application>`, and that shape requires `runFullTrust`. The capability is
what marks the package as a full-trust Win32 app rather than a UWP one; it runs
at medium integrity like any ordinary desktop program, so §27.5's "must never
require elevation" is untouched. The manifest now declares exactly
`runFullTrust` and `bluetooth`, and `LayeringGuardTests` asserts that exact set
so a third capability cannot arrive on the back of this one.

### Verified by running it

Not just compiled. On 2026-08-29 the unpackaged x64 build was launched and:

- the window appears, titled **PicoSwitch2 Companion**, with the custom title
  bar, Mica showing the desktop through it, the `NavigationView` rail
  (Adapter / Keyboard & Mouse / Amiibo / Gamepad / Settings), the connection
  `InfoBar`, and the Adapter page;
- **a second launch exits with code 0 and exactly one instance remains** — the
  Phase 0 exit criterion for single-instance activation, and the Windows
  enforcement of "one process, one active management session".

---

## 5. Layout

```text
windows/companion/
  PicoSwitch.Companion.sln
  Directory.Build.props        shared TFMs, the supported-OS floor, the pinned Windows App SDK
  global.json                  .NET SDK band
  build.ps1                    -Core / -App / -Msix, explicitly
  src/
    PicoSwitch.Bridge.Core/      Core/  Protocol/           net9.0, no Windows types
    PicoSwitch.Management.Core/                             net9.0, no Windows types
    PicoSwitch.Companion.Windows/  Bluetooth/ Storage/     the ONLY project allowed WinRT
                                   Platform/
    PicoSwitch.Companion.Services/ Diagnostics/             ownership + application operations
    PicoSwitch.Companion.App/      Pages/  Assets/          WinUI 3 shell
  tests/
    PicoSwitch.Bridge.Core.Tests/
    PicoSwitch.Management.Core.Tests/
    PicoSwitch.Companion.Windows.Tests/                     also holds the App-project guards
    PicoSwitch.Companion.Services.Tests/
  docs/README.md               this file
```

Namespaces mirror the Kotlin packages exactly — `PicoSwitch.Bridge.Core` ⟷
`dev.picoswitch.bridge.core`, `PicoSwitch.Management` ⟷
`dev.picoswitch.management` — so the two trees can be reviewed side by side.

---

## 6. Deliberate translation decisions

A Level 1 port is a place where quiet divergence hides. These are the places the
C# does **not** mirror the Kotlin line for line, and why.

| Kotlin | C# | Why |
|---|---|---|
| `Set<ControllerButton>` in a data class | `ControllerButtonSet` over the wire bitmask | Kotlin gets structural equality free; a C# record holding a `HashSet` compares by reference, so `StateFlow`-style conflation would break and publish 125 identical snapshots a second. Storing the mask also leaves exactly one `1 shl ordinal` in the codebase. |
| `Set<PeerTransport>` | `PeerTransportSet` | Same reason, for record equality on `PeerInfo`. |
| `List<T>` in a data class | `ValueList<T>` | Same reason again: structural equality is load-bearing for change detection and for test assertions. |
| `StateFlow<T>` | `StateValue<T>` / `IReadOnlyStateValue<T>` | A current value plus conflated change notification, with no reactive dependency. |
| `roundToInt()` | `Math.Floor(x + 0.5)` in `MotionScale.Clamp16` | Kotlin rounds half **up**; .NET's defaults do not. They differ only on negative midpoints — exactly the kind of one-count divergence a golden catches months later. |
| `withTimeout` + `delay` | injected clock and delay on `ManagementClient` | The persistence, Amiibo and wake polls are bounded in wall-clock time. A seam keeps their tests in milliseconds instead of seconds, and production passes neither. |
| `java.util.zip.CRC32` | `Crc32` written out in the project | Keeps `PicoSwitch.Management.Core` dependency-free. Pinned against the `amiibo begin` vector and the standard `123456789` check value. |
| `object ManagementProtocol` parser names | same names, types qualified at `new` | Six parser methods share a name with the type they return; C# resolves the simple name to the method group, so those call sites qualify the namespace. The names stay 1:1 with the Kotlin on purpose. |

---

## 7. Phase 2 — what is built, and what is not proven

Everything in Phase 2's component list exists and is unit- and
integration-tested over fake transports. **The happy path has since been
exercised against a real adapter; four specific boundaries have not.**

### Hardware-observed, 2026-08-29 — Confirmed

One session against a real PicoSwitch2, reconstructed from the persisted
artifacts it left behind rather than from a live capture. Everything in this
list is a state that could not exist unless the step before it worked:

| Step | Evidence |
|---|---|
| BLE discovery on the management service UUID | a registry row exists for an adapter that was found by UUID filter, not by name |
| Windows pairing ceremony (`ConfirmOnly` + `Encryption`) | the encrypted session below it could not otherwise have opened |
| Encrypted management GATT session | `info` was answered |
| `info.id == "picoswitch"` identity gate **before** persistence | a row is written only on the far side of that check |
| Firmware and personality reads | `firmware: "2.0"`, `personality: "pro2"` cached |
| Complete five-peer logical inventory | folded into history, which refuses a partial read outright |
| Registry persistence and reload | `adapters.json` written and re-read on the next launch |
| Peer-history persistence and reload | `peer-history.json` written and re-read |

**One design assumption confirmed, not merely reasoned.** Four of the five
peers reported `role: "unknown"` while `bonded: true`, and only one carried a
live identity (`Xbox Wireless Controller`). That is the post-reboot shape
Bluetooth Management 2.0 predicts, and it is direct evidence that Paired
Controllers must route on **durable bonded/trust evidence** rather than on live
`role`. Routing on role would have shown the user an empty list with four real
pairings hidden behind it.

Built and software-verified:

- `BleGattManagementTransport` — advertisement watcher filtered on the service
  UUID, `GattSession` with `MaintainConnection`, uncached service resolution,
  20-byte ordered writes, `BleReplyAssembler` reassembly, generation authority on
  every callback, and a session that is invalidated on any failure after transmit.
- `AdapterRepository` — the two-stage recovery ladder (one clean retry at 350 ms,
  then ONE fallback scan restricted to the selected address), the lean identity
  boundary, complete-or-nothing peer reads, and independent capability probes.
- `AdapterRelationshipCoordinator` — the §18.1 lifecycle including the
  `Validating` phase, so `Connected` cannot be entered without `info` answering
  `id == "picoswitch"`.
- `ActiveAdapterCoordinator` + `AdapterSwitch` — the ordered A → B handoff, with
  a failed activation settling at "B selected, not connected" and never falling
  back to A.
- `AdapterRegistry` / `PeerHistoryBook` and their codecs and stores — atomic
  writes, total decoding, and re-sanitisation of remote text on read.
- `WindowsAdapterPairing` — `ConfirmOnly` + `Encryption`, and `UnpairAsync`,
  which is what makes the repair message an action rather than an instruction.

### The four Phase 2 boundaries

| # | Item | Confidence | State |
|---|---|---|---|
| A | Stale Windows pairing after an adapter flash/reset → `RepairRequired` | **Confirmed PASS** | Ran 2026-08-29, disproved the pre-test hypothesis, signature rewritten from the evidence, confirmed on retest the same day: first Connect press after the flash reached `RepairRequired` in 2.4 s. See below. |
| B | The repair action (unpair → re-pair → reconnect) | **Confirmed PASS** | Ran 2026-08-29. Repair had never worked; fixed and confirmed on retest. See below. |
| C | One management client, no churn | **Confirmed PASS** | 2026-08-29, from the adapter's own `btlife` ring: 21 lifecycle records, one handle, zero alternation violations, no transition across two Refreshes and full navigation. `dumps/windows-phase2-oneclient-2026-08-29.md`. |
| D | A → B active-adapter handoff under real asynchronous callbacks | **Unknown — deferred** | No second adapter available. Unit-tested over a fake port, including the ordering rule and generation-based rejection of stale callbacks; whether a real trailing callback from A can reach B's state is untested. **Not hardware qualified.** Run before a second adapter is advertised as supported. |

The signature is confirmed in **both** directions: it fires on a genuine
mismatch, and with the adapter powered off it correctly declines
(`observed=False ... -> not a bond mismatch`). That negative is the property that
protects the user, since a switched-off adapter returns the same
`GattCommunicationStatus` as a reflashed one; the observed values are pinned as a
regression test.

Still open, and not blocking Phase 3: **the recovery ladder's retry and 350 ms
backoff have still never executed on hardware**, because every failure observed
so far was at a non-retryable stage.

#### A — what Windows actually does

The pre-test signature expected the refusal at the ATTRIBUTE layer:
`AccessDenied` or an authentication `HRESULT`. Windows produced neither. Four
attempts against a genuinely reflashed adapter all gave:

```
open device resolved paired=True
fail stage=services GattCommunicationStatus=Unreachable
```

with no ATT byte and no `HRESULT` — the status was *returned* by
`GetGattServicesForUuidAsync`, not thrown, so there was never an exception to
carry one. Windows encrypts the link for a bonded peer before any ATT
transaction exists; the reflashed adapter has no matching key, so the failure is
BELOW the attribute layer and `Unreachable` is all WinRT has to say about it.

`Unreachable` is also what a powered-off adapter produces, so the corrected
signature is **compound** — see `AdapterResetSignature` for the full condition
set and the negative cases each clause exists to reject. The consequence for the
ladder is that the attribute shape still ends it at the first failure, while the
link shape must let the fallback scan run: the fallback is what produces the
corroborating second observation.

The retest confirmed all of it, and a diagnostic added for it measured the
mechanism instead of inferring it — four times identically:

```
link status=Unreachable connection=Connected session=Closed maxPdu=23
```

Windows established the LE link, the GATT session never opened, and the ATT MTU
never left its 23-byte default, so **no ATT transaction of any kind occurred**.
A link that comes up and carries no attribute traffic is the shape of encryption
failing immediately after connection.

**`ConnectionStatus` is a confirmed presence discriminator, and is deliberately
still not in the signature.** Both halves of the criterion were observed:
`Connected` for a present reflashed adapter, `Disconnected` for one powered off.
But it would change no outcome that has ever been observed — the binding
constraint is the two-resolved-devices corroboration, not presence. Relaxing
*that* given proven presence is the tempting next step and is refused: a
transient discovery failure on a healthy adapter has the identical shape, and the
cost of a false positive is offering to destroy a working pairing. See the
experiment record for the attempt-by-attempt working.

#### B — Repair had never worked

Repair resolved the Windows pairing through `AdapterRecord.DeviceId`, a cached
WinRT device path that **nothing ever populated**. It took its null branch every
time, logged `no Windows device path cached`, cleared the repair flag and
reported success with the stale bond untouched.

A repair test existed and passed. It asserted that the row kept its alias and
address, and it did — because nothing had happened to it. Repair now resolves the
device fresh from the address, unpairs once, verifies Windows agrees, and clears
the flag only then. `DeviceId` is gone, and `IAdapterPairingGateway` is the seam
that makes the unpair assertable.

Confirmed on the retest by `[repair] unpair <addr>: removed`, and more
convincingly by what followed: the subsequent Pair found the adapter **not**
paired, ran a real ceremony, and required the physical double-tap — which the
adapter only demands for a NEW bond. On the broken run the same step had found
`pairing=paired` and skipped the ceremony entirely. A no-op cannot produce that
difference.

#### Before spending more bench time

An audit before the run found the signature could not fire at all: thrown WinRT
failures were never wrapped, "Windows still paired" was read off an already
disposed connection, and "peer answered" was set only on the scan path while a
remembered adapter connects directly. Fixing those is what made the run produce a
usable answer rather than a second mystery. It is worth doing again before the
next hardware session.

Full evidence, the confounder table, and the two remaining open questions are in
`docs/experiments/windows-phase2-boundaries-2026-08-29.md`.

#### One more thing the retest exposed

The classification was right and the **user was told something else**. The
relationship reached `RepairRequired` and the banner said *Repair pairing to
continue*, while the error surface — the most prominent element on the page —
said `The adapter did not expose its management service`, twice, because both
ladder branches failed identically and were joined verbatim. A classified bond
mismatch now surfaces as `AdapterBondMismatchException` carrying the actionable
message, with the tagged failure retained as its inner exception, and
`ManagementErrorText.Summarize` de-duplicates identical branch messages without
collapsing genuinely different ones.

Two deliberate departures from the Kotlin original, both caught by their own
tests:

- **An identity rejection is terminal.** `AdapterIdentityException` is not a
  connectivity failure: the address was reached and something answered, so a
  retry and a fallback scan can only waste the deadline. The Kotlin path did not
  make that distinction; a test written against the ladder exposed it.
- **The bond-mismatch signature is not stage-restricted to Connect.** Android
  matches HCI 0x05/0x06 at the connect stage. On Windows the refusal surfaces
  wherever encryption is first required — service resolution, the CCC write, or
  the first command — so the stage is not part of the condition.

## 8. Next

The four boundaries above, then Phase 3. Some Phase 3-shaped UI already exists
because Phase 2 could not be exercised without it — the Adapter page, Pair /
Refresh / Disconnect, the remembered-adapter list with Connect / Repair /
Remove, live session state, the peer inventory, and the Diagnostics page. That
glue is real and stays; Phase 3 is an audit of what remains on top of it, not a
rewrite of it.

Phase 6 remains gated on the §14.5 HOGP peripheral-role experiment, which has not
been run.
