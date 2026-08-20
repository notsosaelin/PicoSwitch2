# Management architecture

## Goals and constraints

PicoSwitch2 management must remain usable while the adapter owns a console-facing controller
personality, preserve the existing wire contract, and keep operating-system connection mechanics
out of reusable protocol/workflow code. A future client must not need Android UI source to learn
commands, paging, persistence, or request ownership.

The architecture also preserves these constraints:

- one logical command produces one JSON reply;
- the in-band firmware bridge has one command slot and one reply slot;
- BLE writes require a durable bond and active 16-byte link encryption;
- management is not Controller Bridge input;
- Android permissions, Companion Device association, GATT objects, and lifecycle stay platform-side;
- the portable core has no Android SDK dependency.

## Context view

```text
user / automation
       |
platform UI or CLI                  local files and app state
       |                                      |
platform connection backend                   |  (not adapter state)
       |
connected ManagementChannel
       |
portable ManagementClient + protocol/domain
       |
newline command / JSON reply contract
       |
PicoSwitch2 config parser and persistence services
```

The client sees one already-connected logical transaction channel. Discovery, pairing, and OS
peer identity are outside that channel. The firmware owns validation and the authoritative state.

## Module view

```text
android/companion/app
  UI, ViewModel, local Amiibo library, Android BLE backend
           |
           v
android/companion/management-core
  domain + commands/parsers + portable workflows + session serialization
           |
           v
Kotlin/JVM + kotlinx-coroutines + kotlinx-serialization-json

android/companion/bridge-core       (independent sibling; no management dependency)
```

`:management-core` is physically below `android/companion` because the existing Gradle build is
rooted there. Its build uses the plain `kotlin("jvm")` plugin, contains no Android imports, and is
independently tested. `ArchitectureGuardTest` mechanically checks that boundary.

The app retains a thin source-compatibility alias file under `companion.protocol`; it delegates to
the core and contains no parser or constants. `ManagementProtocolTest` prevents Android repository,
UI, and transport code from becoming a second protocol authority.

## Responsibility view

| Layer | Owns | Does not own |
|---|---|---|
| Protocol/domain | command builders, reply parsers, identifiers, shape/range checks, firmware errors | scanning, GATT, Android types, presentation strings |
| Portable workflows | refresh composition, paging, mutation/readback, save meaning, Amiibo transfer and polling | file pickers, NFC, artwork/cache, reconnect UI |
| Connected channel | one complete logical request/reply or failure | discovery, permissions, pairing prompts |
| Serialized carrier session | single-flight reply ownership, cancellation boundary, lifecycle ordering | firmware domain semantics |
| Android backend | scan/connect, saved address, GATT discovery, CCC subscription, MTU-safe writes, notification assembly, cleanup | commands and JSON interpretation |
| Android app/UI | relationship UX, local library, presentation, durable local preferences | raw command construction and reply parsing |

## Runtime views

### Ordinary transaction

```text
UI -> ManagementClient -> ManagementChannel -> Android GATT -> firmware bridge -> config parser
UI <- typed result      <- complete JSON     <- notifications <- bounded reply <- command handler
```

The portable client validates/builds the command and parses the complete reply. BLE fragments are
invisible above the backend.

### Mutation and readback

```text
client     firmware
  | mutation |
  |--------->|
  |  ack     |
  |<---------|
  | read     |
  |--------->|
  | state    |
  |<---------|
```

The acknowledgement proves acceptance, not the final observed value. `ManagementClient` performs
readback for active input, colors, KB/M mode/bind/reset/mouse, and bond removal. Personality
changes and USB re-enumeration return explicit side-effect flags instead.

### General settings persistence

```text
save -> accepted(requested=N) -> poll save status -> completed has reached N
                                      requested may be newer than N
```

Firmware assigns every explicit or automatic general-settings save a session-local monotonic
identity. Core 1 snapshots the newest request before composing and writing the settings record, and
publishes that identity as completed only after flash erase/program returns. A request arriving
around a write remains newer than completed and cannot be erased by completion of the older
snapshot. BLE never busy-waits; Config CDC may synchronously wait for its own identity.

### Transactional Amiibo upload

```text
status -> begin(size, CRC) -> chunk(0..n, max 32 bytes) -> commit -> persist
                                                            |
                                      poll status until persisted and not pending
```

Any exception after `begin` triggers a best-effort `amiibo cancel`. Local Android file validation,
key use, and library storage remain outside the portable workflow.

## State ownership

| State | Owner | Lifetime |
|---|---|---|
| Adapter runtime configuration and active input | firmware | until mutation/reboot as defined per field |
| Adapter-persisted settings, Amiibo banks, LE bonds | firmware flash/device DB | until explicit reset/removal/new-install reset |
| Connected peer, subscription readiness, pending reply | platform backend | one GATT session |
| Typed adapter snapshot and KB/M maps | client repository | invalidated on disconnect |
| Android association and saved address | Android/app | across sessions until forgotten |
| Local Amiibo library and UI preferences | app | independent of adapter connectivity |

A disconnect clears adapter-derived live state but does not erase local files, preferences, or an
OS relationship.

## Error and security boundaries

Firmware JSON errors become `AdapterCommandException`; malformed or incomplete replies become
`ManagementProtocolException`; reply overflow and inconsistent pagination have dedicated portable
errors. Android connection failures remain backend failures expressed through `ManagementException`
without exposing Android exception types to the core.

Security is enforced by firmware before command dispatch. Service discovery is not authorization.
The platform backend establishes a bonded/encrypted session; portable workflows do not infer trust
from an address, app association, or a `connected` label.

## Compatibility

Unknown JSON fields are ignored. Required fields for a typed result are validated. Unsupported
optional families are reported through `CapabilityState` during refresh. Transport/session/protocol
failures propagate instead of fabricating an Unknown capability. KB/M has one explicit capability
derived from its status family. Bond v2 metadata is required for provably complete pagination; a
bounded legacy reply is exposed as `complete=false`.

No management protocol version was bumped. `save status` and the `save.requested` field are
additive; older clients ignore the field, while newer clients degrade to request-only semantics on
older firmware rather than claiming durability.

## Relationship to Controller Bridge

Controller Bridge turns a host device into an input source. Management inspects and configures the
adapter. They have separate modules, models, state machines, transports, and contracts. The only
relationship is that the Android product can use both features and management can select an input
source reported by firmware.

## Decisions and risks

The portability and serialization decisions are recorded under [decisions](decisions/README.md).
The principal remaining validation gap is physical BLE exercise of this refactoring; existing
firmware and app behavior was preserved and all software-visible gates pass.
