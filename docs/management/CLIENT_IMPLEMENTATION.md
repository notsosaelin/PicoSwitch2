# Implementing a management client

This is a practical guide for a new client. Read [PROTOCOL.md](PROTOCOL.md) for normative field and
command rules and [TRANSPORTS.md](TRANSPORTS.md) before implementing a BLE backend.

## 1. Choose reuse or reimplementation

JVM clients can depend on the plain Kotlin/JVM `:management-core` module and implement
`ManagementChannel`. It provides domain types, command/reply handling, paging, and workflows.

Non-JVM clients implement the language-neutral protocol and verify it against
`tools/fixtures/management/protocol-v1.json`. Do not translate Android repository or ViewModel code;
those are consumers, not authorities.

There is now a worked example of the second path: `PicoSwitch.Management.Core` under
`windows/companion/` is a C# reimplementation whose `ProtocolConformanceTests` runs the same cases
as the Kotlin `ProtocolConformanceTest` against that same fixture file. Two details from it are
worth copying rather than rediscovering:

- **Parse strictly, and read the JSON value KIND before reading the value.** A string where an
  integer is expected is an error, never a coercion. The one exception is `batteryValid` and
  `charging`, where the firmware genuinely emits both a boolean and an integer form; a `boolInt`
  reader used anywhere else is a bug.
- **Capability probing keys off the adapter's own error SHAPE**, not an exception type:
  `unknown command`, `unavailable`, `unavailable in …` and `command unavailable over Bluetooth` mean
  "this firmware does not have that command" and degrade one capability. Everything else propagates
  and fails the refresh. That distinction is what keeps an older adapter usable instead of turning a
  half-supported firmware into a dead one.

## 2. Establish a usable session

For BLE:

1. obtain platform Bluetooth permissions;
2. scan for the PicoSwitch2 management service UUID, or connect to a known saved address;
3. complete the platform bond flow; a new adapter bond requires the physical double-tap window;
4. connect GATT and discover the custom service;
5. locate RX and TX;
6. enable TX notifications and write its CCC;
7. do not declare the session ready until the CCC write succeeds.

An OS association, OS bond, adapter bond, and usable encrypted session are separate. Preserve those
distinctions in state and diagnostics.

## 3. Implement one logical transaction

Validate a non-empty one-line UTF-8 command of at most 127 bytes. Append LF. For BLE, split into
ordered payloads that fit the ATT write size; 20 bytes is minimum-MTU-safe. Wait for each accepted
write before sending the next.

Subscribe before sending. Concatenate notification bytes until LF, reject more than 511 payload
bytes, strip an optional trailing CR, and return exactly that JSON object. Serialize BLE operations
because replies have no IDs.

If an operation times out, overflows, or disconnects after transmit, invalidate the session and
reconnect. Never let the next request consume a late reply. External cancellation after transmit
must still drain/fail the owned exchange before another request starts.

## 4. Parse replies and errors

Parse the complete reply as a JSON object. Check `error` before operation-specific fields. Preserve
the error string, optional numeric code, and triggering command for diagnostics. Ignore unknown
fields but validate required fields and ranges.

Distinguish at least:

- firmware rejection (`error`);
- malformed/incomplete reply;
- reply too large;
- inconsistent pagination;
- timeout/disconnect/security/backend failure.

Do not turn a malformed required value into a successful empty object. Do not expose platform
Bluetooth exception classes as the portable public error contract.

## 5. Initial state refresh

A current full refresh is:

```text
info
get
device
personality                 optional on older firmware
amiibo status               optional
mgmt status                 optional
bonds list (+ v2 pages)     optional
input sources               optional
kbm status                  optional
kbm mouse                   only if KB/M exists
```

Treat a typed firmware `unknown command` or explicit `unavailable` response as an unsupported
optional family. A timeout, disconnect, invalidated channel, reply overflow, malformed/incomplete
reply, pagination failure, or any other transaction failure MUST propagate and fail the refresh;
it must not be converted to `CapabilityState.Unknown`. `AdapterCapabilities.kbm` reflects whether
the KB/M status family was observed or explicitly unsupported. On disconnect, clear the live
snapshot, maps, and connection-scoped counters; retain local files/preferences and the platform
relationship.

The Kotlin reference implements this as `ManagementClient.refreshAll` and returns
`ManagementRefresh`.

## 6. Mutations and authoritative readback

Use the pattern:

```text
validate locally where the contract is fixed
send mutation
require successful acknowledgement
perform authoritative readback
publish the read state
```

Current readback pairs are:

| Mutation | Readback |
|---|---|
| `input active ...` | `input sources` |
| `body` / `jcl` / `jcr` | `get` |
| `kbm mode ...` | `kbm status` |
| `kbm bind` / profile reset | complete `kbm map` for that profile |
| `kbm reset all` | status, mouse, and both complete maps |
| `kbm mouse ...` | mutation reply is the complete authoritative mouse object |
| `bonds remove ...` | complete bond enumeration, if the caller remains connected |

Personality and re-enumeration are side-effect operations: validate the acknowledgement flags and
expect the relevant USB device to detach/reappear. BLE management normally stays connected during a
controller-side USB re-enumeration. A Config CDC caller can lose its own transport.

## 7. Save correctly

Settings changes are RAM-immediate unless their command says otherwise. Call `save` separately when
the user requests persistence.

Over BLE, `{"ok":true,"queued":true,"requested":N}` means the deferred flash operation was
accepted. Report **save requested/queued**, not **saved to flash**, until `save status` reports a
`completed` counter that has reached `N` in modulo-uint32 order. A later automatic save may make the
status remain `pending:true` after `N` completed, so wait for the acknowledged identity rather than
only for a naked pending flag.

`ManagementClient.save()` preserves queued/accepted state and the optional request identity.
`saveAndAwait()` and `awaitPersistence()` provide bounded authoritative polling. If old firmware
omits the identity or does not implement `save status`, requesting persistence remains compatible
but durable completion cannot be claimed. The Config CDC path waits synchronously before its `save`
reply, and now returns the same completed request identity.

Virtual Amiibo keeps its separate `amiibo persist` flags and workflow.

## 8. Retrieve paged data safely

### Bonds

Request `bonds list`. If it returns a v2 envelope, validate and consume it. If it returns error 413,
request `bonds list v2`, then follow each integer `next` cursor until null. Require:

- `v == 2`;
- one stable non-negative total;
- non-negative, progressing cursors;
- unique bond indices;
- no empty nonterminal page;
- final aggregate count equal to total.

An unversioned legacy array can be shown only as explicitly incomplete/unknown completeness.

### KB/M maps

Start at page 0 and increment while `more` is true. Require the requested profile/page, one stable
total, non-empty progress, no aggregate overflow, a bounded page count, and exact final total.

## 9. Configure keyboard and mouse

Read `kbm status` and `kbm mouse` before presenting controls. Keep configured `override` distinct
from effective `mode`. Use only the source and destination identifiers in the protocol reference.

`none` creates an explicit unassigned mapping. `default` removes a custom override, revealing the
firmware's canonical binding. They are not synonyms.

Build mouse controls from adapter-reported min/max values. For sliders, debounce pending previews
before transmission and always send the final released value. Do not cancel an already-transmitted
request to make room for a newer value. Read the returned mouse object and show what firmware
accepted. Call `save` separately if persistence is desired.

## 10. Upload a Virtual Amiibo

1. validate local file/crypto policy outside the protocol core;
2. accept only 540, 572, or 2048 bytes;
3. read status and refuse replacement when unsynced `dirty` data would be lost;
4. compute CRC32 over the exact bytes;
5. send `amiibo begin <size> <crc>`;
6. send contiguous `amiibo chunk` commands of at most 32 bytes;
7. commit the primary image or secondary save copy as intended;
8. send `amiibo persist`;
9. poll status until `persisted=true` and `persistPending=false`;
10. read final status.

After any exception following `begin`, send `amiibo cancel` best-effort, then propagate the original
error. Progress callbacks are local presentation policy.

## 11. Download and synchronize a Virtual Amiibo

Read status first. Reject unloaded and unsupported sizes before allocating. Read exactly the
reported size in chunks of at most 32 bytes. Validate each returned byte count and compute CRC32.
Compare with `payloadCrc` when it is authoritative (always for v3; for standard images when it is
not the unavailable `00000000` value).

Before `amiibo downloaded`, reread status and ensure generation and available CRC still match the
download. Then acknowledge download, request `amiibo persist`, and poll the persistence flags. This
prevents acknowledging data that changed mid-sync.

Refuse `clear` while dirty data has not been synchronized. Standard tags can select save1/save2;
v3 does not expose a separate console-written copy.

## 12. Manage bonds and reconnect

Adapter bond indices belong to the firmware device DB and may change after mutation. Never cache an
index as permanent peer identity; enumerate before presenting/removing and enumerate again after.

Removing the bond used by the current phone can make the next reconnect fail even if Android still
retains an association or OS bond. Surface these separately and offer the platform-specific repair
flow. `mgmt off`, console sleep, wake advertising, and remote radio loss can also close a session.

After any terminal carrier failure:

1. stop accepting transactions;
2. close platform resources;
3. discard buffered fragments and live adapter state;
4. reconnect and resubscribe;
5. perform a fresh state read before enabling mutations.

## 13. Validate the implementation

Run every vector in `tools/fixtures/management/protocol-v1.json` through the client parser/builder.
Then exercise the behaviors in [CONFORMANCE.md](CONFORMANCE.md), especially malformed/oversize
replies, bond and KB/M paging inconsistencies, `none` versus `default`, queued saves, Amiibo failure
cancel, concurrent callers, cancellation after transmit, timeout invalidation, and disconnect state
clearing.

Passing vectors and software tests is Source-Tested evidence. Claim physical BLE or console
confirmation only after that hardware was actually exercised.
