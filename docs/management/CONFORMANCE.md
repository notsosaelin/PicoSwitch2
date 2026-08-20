# Management conformance

This is a minimum verification profile, not a certification program. A client conforms to the
current contract when it satisfies the applicable behaviors below and does not claim stronger
transport or hardware evidence than was exercised.

## Shared assets

`tools/fixtures/management/protocol-v1.json` is language-neutral JSON containing:

- protocol limits and BLE UUID constants;
- command-builder golden values;
- firmware-shaped successful replies;
- malformed/error replies;
- bond pagination pages;
- a representative multi-step KB/M mutation/readback trace;
- a representative transactional Amiibo upload trace.

Vectors are cross-checked against the formatters and parsers in `src/config.c`,
`src/mgmt_bonds.c`, and `src/config_wireless_bridge.c`. They are examples plus executable drift
guards; normative required/optional behavior remains in [PROTOCOL.md](PROTOCOL.md).

The reference test locations are:

| Layer | Tests |
|---|---|
| Protocol/vectors | `management-core/.../ProtocolConformanceTest.kt` |
| Portable workflows | `management-core/.../ManagementWorkflowTest.kt` |
| Session ownership | `management-core/.../SerializedManagementSessionTest.kt` |
| No-Android boundary | `management-core/.../ArchitectureGuardTest.kt` |
| Android consumer boundary | `app/.../ManagementProtocolTest.kt` |
| Android repository/state | `AdapterRepositoryTest.kt`, `KbmRepositoryTest.kt` |
| Firmware carrier/security | tests invoked by `tools/run_mgmt_tests.ps1` |

## Protocol checklist

A client implementation must prove:

- exact golden command spelling and argument encoding;
- LF framing, optional received CR handling, embedded-newline rejection, and 127-byte command limit;
- complete-reply assembly and 511-byte BLE payload limit;
- valid parsing of all fixture success shapes;
- unknown JSON fields are tolerated;
- required fields and known identifier/range constraints are enforced;
- malformed JSON, odd/non-hex data, and unexpected acknowledgements fail;
- any firmware `error` fails while preserving optional code and triggering command;
- unsupported operation is distinguished from transient/unknown failure where capability UI needs it.

## Workflow checklist

### Refresh and mutations

- Core refresh distinguishes available, unsupported, and unknown optional families.
- Active-input, color, KB/M, and bond mutations perform the authoritative readback specified by the
  contract.
- Personality/re-enumeration acknowledgements preserve side-effect flags.
- `queued:true` is represented as queued persistence, not durable completion.

### KB/M

- Both profiles assemble from page 0 through `more=false`.
- Changed totals, wrong profile/page, empty nonterminal pages, overflow, and excessive pages fail.
- Source and destination identifiers validate.
- `none` produces an explicit neutral destination; `default` clears the override.
- Mouse controls use adapter-reported bounds and consume the returned authoritative object.
- Disconnect drops session-scoped status/maps.

### Bonds

- Bounded v2 and 413-to-v2 fallback both work.
- Cursor progress, stable total, unique indices, and exact final count are checked.
- Legacy/unversioned results are labeled incomplete rather than silently trusted.
- Remove is followed by enumeration when the session remains usable.

### Virtual Amiibo

- Upload permits exactly 540, 572, and 2048 bytes and computes CRC32 over exact bytes.
- Chunks are contiguous and no larger than 32 bytes.
- Any failure after begin causes best-effort cancel.
- Persist acknowledgement is followed by status polling.
- Download validates size before allocation, exact returned byte count, generation, and available
  payload CRC.
- Dirty data is not replaced or cleared without explicit synchronization policy.

## Session/transport checklist

For a one-slot/no-request-ID carrier such as BLE, prove:

- concurrent callers transmit serially;
- cancellation before ownership does not transmit;
- cancellation after transmit cannot release ownership before reply consumption/session failure;
- disconnect/lifecycle mutation is ordered with an exchange and cannot be cancelled halfway after
  acquiring ownership;
- timeout, overflow, and mid-command disconnect invalidate the carrier;
- a late old-session reply cannot enter a new session;
- subscription readiness precedes writes;
- resources and buffered fragments are cleared on close.

Firmware must additionally pass access decision, bonded/encrypted write, allowlist, fragmented RX,
busy rejection, oversized-line recovery, response chunking, and stale-session tests.

## Repository commands

From `android/companion` with JDK 21 and an Android SDK available:

```powershell
.\gradlew.bat :management-core:test
.\gradlew.bat :bridge-core:test :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

From the repository root:

```powershell
pwsh -File tools\run_mgmt_tests.ps1
cmake --build build\pico2_w --config Release --parallel
cmake --build build\pico_w --config Release --parallel
python tools\verify_install_reset_marker.py build\pico2_w\PicoSwitchWGA-pico2_w.bin --flash-size 0x400000
python tools\verify_install_reset_marker.py build\pico_w\PicoSwitchWGA-pico_w.bin --flash-size 0x200000
```

Run instrumented smoke tests and install/launch on an emulator or device when one is available. A
successful JVM/build/lint run does not establish Android runtime, physical BLE, or Switch 2 gameplay
behavior.

## Change-control rule

A wire change is complete only when firmware, this protocol reference, the fixture, portable parser
and workflow tests, and Android regression tests change together. Moving code alone does not
justify a management version bump. If backward-compatible optional fields are added, old clients
must continue to ignore them; if required behavior changes, document the compatibility mechanism
before release.
