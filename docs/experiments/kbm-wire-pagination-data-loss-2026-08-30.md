# Why the Keyboard & Mouse page could not load: two wire defects

**Date:** 2026-08-30
**Status:** Resolved. Confirmed by reproduction; fix hardware-validation pending.
**Area:** Management protocol — KB/M read surface

## Question

After flashing the KB/M profile system, the Windows companion could not open the
Keyboard & Mouse page. It reported, in sequence across two builds:

1. `response_too_large`
2. `Adapter returned an incomplete KB/M binding list`, page stuck on "Not ready yet"

Why, and what class of mistake produced both?

## Background

The wireless management bridge holds one reply in a fixed slot
(`CONFIG_WIRELESS_RESPONSE_CAPACITY`, 512 bytes). `config_wireless_bridge_publish_response()`
stores the reply **plus a newline**, so the usable JSON payload is **511 bytes**.

An over-long reply is **not truncated**. The bridge refuses it and substitutes
`{"error":"response_too_large","code":413}`. A caller therefore loses the *entire*
read, not the field that overflowed — so one oversized reply takes down every
value a page needs.

## Defect 1 — `kbm status` outgrew the slot

Adding the active-mapping identity (`activeProfile`, `activeProfileName`,
`activeRevision`, `activeFingerprint`, `activeMatchesSaved`) to a reply that
already carried 15 ingress counters pushed it to **729 bytes worst case**.

### Evidence

Measured on the maintainer's adapter over UART, pre-fix build:

```
[kbm status] {"mode":"controller","override":"auto",...}    559 bytes
```

559 > 511. The bridge refused it, the KB/M read failed at its first command, and
the profile UI never rendered — which presented as the UI simply not existing.

### Why the test did not catch it

`tools/test_ns2_kbm_status.c` asserted the reply fit `KBM_STATUS_TEST_BUFFER`,
set to **2048** — the size of `ns2_uart_diag.c`'s local buffer. It measured the
wrong constraint entirely and passed while the adapter was refusing the reply.

### Fix

Split into `kbm status` (product state, 318 B worst case) and `kbm counters`
(the 15 counters, 414 B). One authoritative constant,
`CONFIG_WIRELESS_RESPONSE_MAX_JSON = CAPACITY - 1`, replaces every restatement of
512 — a formatter budgeted against the raw capacity is off by one exactly at the
boundary.

## Defect 2 — fixed page stride with a variable byte budget

The `response_too_large` fix paginated `kbm map` and `kbm profiles`. It kept a
**page index** request (`kbm map kb <page>`) resolved as `first = page * 8`,
while emitting only as many rows as the byte budget allowed.

**Those two rules contradict each other.** Rows are variable width:

```
{"src":"mouse:9","dst":"rstick_right","custom":false}   53 bytes  (worst case)
{"src":"key:04","dst":"a","custom":false}               41 bytes
```

Eight worst-case rows plus the wrapper is ~525 bytes, over the limit. So the
loop broke at 7 rows, reported `more:true`, and the client's next request —
`page 1` — was answered from index **8**. Index 7 was never sent, by anyone.

### Evidence

Replaying the shipped formatter against the Default template that is actually on
the adapter (`$CLAUDE_JOB_DIR/tmp/old_pagination_probe.c`, throwaway):

```
Default template (on adapter) kb   total=26 received=25 replies=4 largest=449
   missing indices: 7   (1 lost)
   missing sources: key:0F->rstick_right
Default template (on adapter) kbm  total=27 received=27 replies=4 largest=479
   missing indices:   (0 lost)
```

`rstick_right` (12 characters) is the longest destination name in
`KBM_DESTINATION_NAMES`, so its row was the one the budget cut. The Keyboard
layout is read first, so the page failed before reaching profiles at all.

Client-side this is `bindings.Count (25) != expectedTotal (26)` in
`ManagementClient.LoadKbmMappingAsync` — reported only as "incomplete".

### Why the tests did not catch it

The formatters lived in `src/config.c`, which is bound to TinyUSB, BTstack and
the flash driver and **does not compile on the host**. Their pagination was
therefore covered only by hand-written client fixtures — in *both* C# and
Kotlin — written from the same misunderstanding that produced the firmware. The
fixtures agreed with the bug.

## Interpretation

Both defects are the same mistake at different layers: **a size assumption
checked against the wrong constraint.** Once against a firmware-local buffer
instead of the wire; once against a nominal page size instead of the bytes
actually emitted.

The second is the more dangerous class, because its failure is *silent at every
individual observation*. Each reply was well-formed, under the limit, and had a
plausible `more`. Only the reconstructed total was wrong.

### Negative knowledge — do not reintroduce

**A fixed page size cannot be made safe here by choosing a smaller constant.**
This is attractive (3 rows would have fit) and wrong. Any constant is either
unsafe for the worst-case row or wasteful for the common one, and the failure
mode of guessing wrong is silent data loss rather than an error. The size that
fits is a property of the *content*, so only the producer can know it.

## Resolution

Cursor pagination. The request carries the index of the next logical item; the
firmware — the only party that knows how many rows it serialized — answers with
where to resume. `next` is null exactly when the walk is complete, the same shape
`bonds list v2` already uses in this repository.

```
-> kbm map kb 0
<- {"profile":"kb","profileId":1,"cursor":0,"total":26,"bindings":[…],"next":8}
-> kbm map kb 8
<- {…,"cursor":8,"total":26,"bindings":[…],"next":null}
```

Structural changes that make the class of bug catchable rather than merely fixing
this instance:

- **`src/ns2_kbm_commands.c`** — the read formatters, extracted from `config.c`
  and platform-neutral, so a host test can drive the real implementation.
- **`tools/test_ns2_kbm_commands.c`** — proves, under worst-case content: every
  reply within the wire limit; every item exactly once; guaranteed progress;
  correct termination; at least one item per reply. 1077 checks.
- **`tools/fixtures/management/kbm-wire-corpus.json`** — generated by that test
  from the real formatters. The Windows and Android integration tests replay
  those exact bytes through their real clients, so all three implementations
  check against one authority instead of three independent guesses.
- **`cfg <command>`** on the UART console runs any management command and reports
  the size the bridge would see. Both defects had to be diagnosed from source
  because the always-available channel could not reach the failing commands.

## Confidence

**Confirmed** for the diagnosis: defect 1 measured directly on hardware
(559 bytes), defect 2 reproduced exactly against the content on that adapter
(25 of 26 bindings, index 7 named).

**Implementation complete; hardware validation of the fix pending.** The new
firmware has not run on the adapter — it cannot be flashed without physical
BOOTSEL access.

## Remaining unknowns

- Whether any *other* management family (peers, bonds, amiibo) has the same
  page-stride-versus-byte-budget shape. `bonds list v2` already uses cursors;
  the others were not audited in this pass.
- Worst-case sizes of the `kbm draft` replies were not enumerated; they are
  small acknowledgements, but that is an assumption, not a measurement.

## Suggested follow-up

Audit the remaining paginated management families against the same five
properties, and generate their corpora the same way. The mechanism now exists.
