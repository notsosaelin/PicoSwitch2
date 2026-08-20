# 0002 — Connected channel separated from platform connection and carrier serialization

Status: Accepted

## Context / problem

The firmware BLE bridge provides one command slot and one reply slot, and replies carry no request
identifier. Android scanning, pairing, GATT setup, and connection state were coupled to the same
interface used by workflows. Coroutine cancellation after transmit could otherwise abandon a reply
that a later caller might consume.

## Considered options

- expose discovery and GATT state through the portable API;
- make all logical management universally concurrent;
- keep a minimal connected transaction channel and serialize only a carrier/session that needs it.

## Decision

Portable workflows depend on `ManagementChannel.transact(command, timeout)`. Android's
`ManagementTransport` adds platform connection lifecycle. `SerializedManagementSession` lives at
the carrier/session boundary and is used by the BLE backend.

Once an exchange owns the BLE session, external cancellation does not release ownership. The
exchange consumes its reply or the backend invalidates the session on timeout, overflow, or
disconnect. A caller cancelled while waiting for ownership never transmits. Disconnect is ordered
through the same mutex.

## Consequences / trade-offs

The API does not falsely require every future transport to be single-flight. BLE callers can wait
longer after cancelling an already-transmitted operation, which is intentional to preserve reply
ownership. UI live controls should debounce or coalesce before calling the channel; the transport
does not guess which domain mutations are safely supersedable.

## Evidence / validation

`SerializedManagementSessionTest` covers concurrent callers, cancellation before and after
ownership, and lifecycle ordering. Firmware host tests cover bridge busy handling, fragmentation,
response chunking, and stale-session rejection.
