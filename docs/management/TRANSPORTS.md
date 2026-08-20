# Management transports

## Connected channel contract

Portable workflows depend on one small abstraction:

```text
transact(logical command, timeout) -> one complete logical reply
```

An implementation returns the JSON object without the trailing newline, or fails the current
operation/session. Discovery, permission prompts, platform peer handles, pairing relationships,
subscription setup, and reconnect policy do not belong to this interface.

The current Kotlin form is `dev.picoswitch.management.ManagementChannel`. Its default logical
transaction timeout is 10 seconds. Backends may have separate discovery/connect timeouts.

## Supported product carrier: BLE GATT

BLE management is available while PicoSwitch2 remains in a normal controller personality and is
the first-class carrier for the Android app and new general management clients.

| Item | UUID | Properties / direction |
|---|---|---|
| Primary service | `7c5ad4ed-2731-417c-b316-058505c7c083` | advertised service |
| RX | `5252186a-817f-489f-ad75-94c3bd444769` | client to Pico; Write and Write Without Response |
| TX | `81462706-8e64-407a-bc3d-d303529fbe1c` | Pico to client; Notify |
| TX CCC | standard `0x2902` | client enables notifications before first command |

The advertisement includes the service UUID and uses the friendly name `PicoSwitch2`. UUID
matching is the discovery authority; a name is not authentication.

### Security and admission

The static ATT database marks RX writes and TX notification access as encrypted. Firmware accepts a
management command only when all of these are true:

- the runtime management gate is enabled;
- the peer is the single connected management client;
- the link resolves to a durable LE bond;
- the active encryption key size is exactly 16 bytes;
- the command is in the production wireless allowlist.

A new bond is admitted only while management is enabled and the physical double-tap pairing window
is open. The no-display Just Works exchange supplies bonding and encryption but not MITM
authentication; documentation must call this **bonded/encrypted**, not authenticated pairing.

Android Companion Device association, an Android Bluetooth bond, the adapter's stored LE bond, and
a live encrypted GATT session are different states. A client must not collapse them into one flag.

Firmware advertises only while management is enabled, the console is awake, wake does not own the
advertiser, and no management client is connected. It drops an existing management client if the
gate is disabled, the console sleeps, or wake needs the advertiser. Controller-central discovery
can coexist with management advertising.

### Framing, fragmentation, and reassembly

The client appends LF to the logical command, splits the resulting bytes into ATT write payloads,
and writes the pieces in order. A conservative 20-byte payload works at the default ATT MTU. The
current Android backend deliberately uses 20-byte chunks and acknowledged GATT writes; it does not
expose MTU to portable workflows.

Firmware ignores CR, assembles until LF, and rejects/discards an overlong line. RX has one 128-byte
buffer including terminator/NUL capacity, giving the 127-byte logical limit.

Firmware stores one JSON reply plus LF in a 512-byte slot and notifies it in pieces sized for the
negotiated connection. The client concatenates TX notification bytes until LF, strips a preceding
CR, enforces the 511-byte payload limit while assembling, and parses only after the full reply is
owned.

Notification subscription MUST be complete before the first write. A zero-length internal signal
after disconnect is not a wire reply; it wakes the Android waiter so it can fail the transaction.

### Serialization and backpressure

The firmware bridge has one command slot and one reply slot. It rejects a second completed command
while either is occupied. Replies contain no request ID, so the BLE session is single-flight even
though the logical protocol itself does not impose concurrency on a hypothetical correlated
carrier.

`SerializedManagementSession` enforces this in Android:

1. a caller cancelled while queued for ownership never transmits;
2. once transmission owns the session, external caller cancellation does not release it;
3. the operation consumes the reply or the backend invalidates the GATT session;
4. only then can another exchange transmit;
5. suspendable disconnect is ordered through the same mutex and, once it owns the session, its
   cleanup completes even if its caller is cancelled.

This prevents a late reply from becoming the next command's reply. Timeout or reply overflow closes
and invalidates GATT, because draining an unknown late response cannot prove synchronization.
Platform `close()` remains an immediate lifecycle escape hatch: it cancels pending Android
primitives, closes GATT, and causes the owned exchange to fail rather than handing the carrier to a
new request.

The carrier does not automatically coalesce domain mutations. UI live controls debounce before
calling it and send the final value on commit. Workflows that can be safely superseded should
coalesce before transmission; an already-transmitted request is never abandoned.

### Timeouts and errors

The Android backend uses 15 seconds for scan and connect, and the channel default is 10 seconds per
logical command. Firmware bond database operations have a one-second internal deadline and can
return `{"error":"timeout"}`. Portable Amiibo persistence polling has a separate six-second
workflow deadline. General settings completion polling is also bounded to six seconds by the
reference client and follows the request identity returned by `save`.

Scan, GATT discovery, missing characteristics/CCCD, write status, disconnect, and platform timeout
are backend failures. Firmware JSON errors are parsed above the carrier. On transaction timeout,
oversize reply, or disconnect during a command, session-scoped state is invalid and the caller must
reconnect before another command.

### Connection lifecycle

The Android backend:

1. scans by service UUID or directly connects to a previously saved address;
2. discovers the service and RX/TX characteristics;
3. enables local notifications and writes the TX CCC;
4. treats the descriptor write as carrier-ready but reports product Connected only after the
   management identity probe verifies PicoSwitch2;
5. serializes transactions until explicit or remote disconnect;
6. retires the owned GATT generation by requesting disconnect, waiting up to 1.25 seconds for its
   matching callback, then closing exactly once and rejecting late callbacks;
7. gives connect statuses 133, connection timeout, and congestion at most one clean retry before one
   service scan restricted to the saved relationship address.

Portable `ManagementClient` starts after carrier subscription in step 4 and performs the identity
probe that promotes the session. Android permission, association, bond repair,
and user-facing retry behavior remain app/backend responsibilities.

## Existing Config USB CDC path

Repository audit found a conflict with the pass premise that USB CDC configuration had been
removed. At this revision, current source, `AGENTS.md`, `STATUS.md`, the portal, and
`docs/architecture/config-transports.md` explicitly retain a **CDC-only Config USB personality** at
`CAFE:4012`. The removed behavior is MSC and the embedded FAT/web disk, not the current CDC serial
parser.

This pass did not create, revive, expand, or make CDC a dependency of the portable client. It
documents the source truth: Config CDC reaches the same logical parser, can return larger diagnostic
replies, and uses synchronous save/persist waits because the controller personality is already
absent. New in-band clients should use bonded/encrypted BLE so management does not drop the console.

Changing or removing Config CDC is a separate product/hardware task and was deliberately not
undertaken here.

## Diagnostic UART overlap

UART exposes diagnostic command handling and shares KB/M parsers/formatters with management. It is
useful for instrumentation and bounded hardware investigation, but it is not the product connected
management carrier, has different operational/security assumptions, and is not implemented by
`ManagementChannel`.

## Audited alternatives

- **Existing controller USB personalities:** Source review found no current descriptor-neutral,
  host-visible general management endpoint. Adding one would change validated USB contracts or
  require speculative descriptor work, so none was implemented.
- **Generic BLE UART service:** not present and intentionally not used; the project-specific UUID
  prevents accidental matching of unrelated devices.
- **Controller Bridge HID:** carries controller input/output and is not a management transport.
- **Android NFC/local files:** platform data sources, not adapter management carriers.

The result is one first-class in-band product carrier (BLE GATT), one pre-existing explicit Config
CDC maintenance path, and diagnostic UART overlap. No new physical transport was added.

## Evidence and tests

Firmware host tests exercise access-control truth tables, allowlisting, fragmented RX, busy
rejection, overlong recovery, response chunking, stale-session invalidation, bond pagination, and
general-save request/completion ordering.
Core JVM tests exercise serialization/cancellation. Android unit/architecture tests enforce core
ownership and repository state invalidation. Android SDK callback behavior is build/lint-tested;
physical BLE behavior was not revalidated during this pass.
