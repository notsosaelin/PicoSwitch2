# A BLE keyboard is classified as a mouse

**Status:** classification fixed; role corrected after the first attempt was
wrong. Awaiting one hardware smoke test.

The 8BitDo Retro is now **KEYBOARD-primary**: it holds the keyboard slot and
leaves the mouse slot free. The first fix (`062e101`) made it COMBO, which
changed the label to `keyboard=True mouse=True` while the keyboard still did not
type, and made it squat the mouse slot. See "Correction" below.

The discriminator is the report descriptor's OPENING application collection plus
the shape of its keyboard half — see "The fix" below.

**Reported:** 2026-08-29, an 8BitDo Retro (Xbox edition) keyboard paired to the
adapter over BLE and appeared in the Windows companion as a mouse.

**Evidence:**

```
19:56:24.409 INFO [kbm] mode=kbmouse profile=kbm keyboard=False mouse=True bindings=53
```

## Is this the app or the firmware?

**Firmware.** The companion renders `kbm status`'s `keyboard` and `mouse` fields
verbatim: `ManagementProtocol.KbmStatus` reads them straight into
`KbmStatus.KeyboardConnected` / `MouseConnected`, and `KeyboardMouse.Project`
turns them into the sentence on the page. Nothing in the app interprets a
descriptor or picks a role.

## The exact path

A BLE peer that declares both a keyboard collection and a pointer collection is
classified as a **mouse**, and the classification is latched for the life of the
connection.

1. `bthid.c` reclassifies an unresolved BLE peer by descriptor, testing keyboard
   **before** mouse. A descriptor carrying keyboard usages therefore binds to the
   **keyboard driver** — that part is correct, and that driver can decode both
   halves.
2. `bthid_keyboard_set_descriptor()` then computes both capabilities and reports
   them:

   ```c
   kb->has_report_map = bthid_keyboard_parse_descriptor(...);   // true
   kb->has_pointer    = bthid_mouse_parse_descriptor(...);      // true
   ns2_kbm_runtime_note_classification(..., true, kb->has_pointer,
                                       classic_cod_declares_combo(device));
   ```

3. `classic_cod_declares_combo()` reads the Class of Device. **BLE has no Class
   of Device**, so this is always `false` for a BLE peer — stated outright in the
   firmware's own comment: *"BLE has no Class of Device, so a BLE peer is never
   classified combo here."*

4. With `declares_combo == false`, `ns2_kbm_runtime_note_classification()` falls
   through to capability precedence:

   ```c
   // ns2_kbm.c
   if (caps.pointer)  return NS2_KBM_PRIMARY_MOUSE;      // pointer wins
   if (caps.keyboard) return NS2_KBM_PRIMARY_KEYBOARD;
   ```

5. `primary_record()` only writes when the slot is still `NONE`, so **MOUSE is
   latched for the whole connection** and no later evidence can correct it.

So the driver binding is right and the *role* is wrong. The keyboard is bound to
a driver that could decode its keys, while the KB/M runtime treats it as the
mouse.

## Why the rule is there

It is not an oversight. "Pointer wins" was written against a real device:

> An ASUS ROG KERIS II gaming mouse parses as kbcap=true AND mousecap=true,
> because its macro buttons put a keyboard collection in its descriptor — yet it
> is a mouse, and treating it as a combo would let it occupy the keyboard role
> and lock out the user's actual keyboard.

For a **Classic** peer the tie is broken correctly, because Class of Device is a
statement by the device. BLE has nothing equivalent, so the same rule that
protects against a mouse-with-macro-keys misfires on **any BLE keyboard that
also declares a pointer collection** — media keys, a trackpoint, or 8BitDo's
programmable buttons.

**Confidence: Strong Evidence.** Every step is read from source and the observed
output matches exactly. What has *not* been captured is the 8BitDo's actual
report descriptor, which would turn this from "the only path that produces this
output" into a direct confirmation.

## The fix

The naive change — let a BLE peer reach COMBO when it declares both — reopens the
KERIS II defect verbatim. What was needed was a discriminator, and the strongest
one is not the keyboard collection's size but **which application collection the
descriptor OPENS with**.

That is the closest a report descriptor comes to a Class of Device: the device
stating what it primarily is. A keyboard opens with `Usage(Keyboard)`; a gaming
mouse opens with `Usage(Mouse)` and declares its macro keys afterwards. It is a
self-declaration, which is exactly the kind of evidence the COMBO rule has always
required — only carried by the descriptor instead of by CoD.

`bthid_keyboard_shape()` requires three facts to agree, and each rules out a
different wrong answer:

1. **the descriptor opens with a keyboard application collection** — keeps the
   gaming mouse out no matter how complete its macro collection is;
2. **a standard 8-bit modifier field over 0xE0..0xE7** — something a keyboard has
   and a handful of macro buttons does not;
3. **real key capacity**: a rollover array of 4+ slots, *or* — for an NKRO board
   with no array at all — 32+ key-bitmap bits. Requiring the array alone would
   fail every NKRO keyboard.

A controller carrying keyboard usages is excluded by `has_gamepad_collection`, as
it already was.

`ns2_kbm_primary_from_evidence()` then grants COMBO when a peer with both
capabilities carries EITHER a Classic combo Class of Device or strong keyboard
shape. `ns2_kbm_primary_from_caps()` is untouched, so capability-only resolution
still answers MOUSE — the clause that protects the keyboard role.

**Why this does not reopen the KERIS defect:** that mouse opens with
`Usage(Mouse)`, so clause 1 fails and it can never reach COMBO. A test pins the
hardest version of this — a mouse whose macro half declares a *complete* boot
keyboard, passing clauses 2 and 3 — and it still resolves to MOUSE.

**Role semantics.** COMBO rather than keyboard-primary, because the keyboard
driver already decodes both halves of such a descriptor
(`bthid_keyboard_set_descriptor` parses the pointer collection into
`kb->has_pointer` and forwards pointer input), and COMBO is what a Classic combo
peripheral has always resolved to. Keyboard-primary would work but would discard
pointer input the device genuinely provides.

**Diagnostics.** One UART line per descriptor arrival, and only for a peer that
actually has both capabilities:

```
[BTHID_KEYBOARD] kb+pointer: primary_usage=0x06 strong=1 modifier=1 rollover=6 bitmap=0 cod_combo=0 -> COMBO
[BTHID_KEYBOARD] kb+pointer: primary_usage=0x02 strong=0 modifier=1 rollover=6 bitmap=0 cod_combo=0 -> MOUSE (macro-safe fallback)
```

## What the capture actually contained

`dumps/kbm-pairing-8bitdo-keyboard-20260829-201415.jsonl` was taken before this
fix, and it holds **no descriptor**: the adapter answered `{"error":"unknown
command"}` to `btid clear`, `btid desc` and `btid dump`, because the flashed build
(`d531b8b7+dirty`) predates the `btid` command that exists in current source.
`kbm status` in the same capture shows `keyboard=false mouse=false` — the keyboard
was not connected at the time.

So the fix was derived from source and from descriptor structure, not from a
captured 8BitDo descriptor. Recorded plainly because a later reader would
otherwise assume the fixture in `test_bthid_keyboard_report.c` came off the
device; it did not. It is the standard shape a keyboard-plus-pointer composite
presents, and the rule is written against the structure rather than against one
device.

A firmware built from this change does carry `btid`, so a future capture will
return the real bytes — useful confirmation, not a prerequisite.

## Correction: COMBO was the wrong role (2026-08-29, after 062e101)

`062e101` made the 8BitDo COMBO. On hardware that produced `keyboard=True
mouse=True` — and **the keyboard still did not type**. The label changed; the
product behaviour did not. The role decision was wrong, and this section records
why, because the reasoning is the durable part.

### The reconstructed role model

Read from `ns2_kbm.c`, `ns2_kbm_runtime.c` and their tests:

| | |
|---|---|
| `NS2_KBM_PRIMARY_KEYBOARD` | this peer IS a keyboard; it may hold the keyboard slot |
| `NS2_KBM_PRIMARY_MOUSE` | this peer IS a mouse; it may hold the mouse slot |
| `NS2_KBM_PRIMARY_COMBO` | this ONE peer is an integrated keyboard-with-pointing-device and may hold BOTH slots |

`ns2_kbm_roles_t` holds exactly one keyboard slot and one mouse slot, and
`group_id` exists specifically so **two different Bluetooth peers** appear to the
arbiter as one logical source. Its own comment: *"what makes two Bluetooth peers
one arbiter-visible logical owner without either of them pretending to be the
other."*

**So separate keyboard and mouse peers are the NORMAL Keyboard + Mouse
composition, not a fallback.** COMBO is the special case — one physical unit that
genuinely supplies both halves — and it was only ever granted from a Classic
Class-of-Device combo peripheral, which is a device saying exactly that.

The profile follows from the roles, and is **not user-settable**: there is no
`kbm profile` command. `ns2_kbm_mode_profile()` derives it from the effective
mode, which `ns2_kbm_effective_mode()` derives from which slots are filled.

### What 062e101 got wrong

It conflated three separate things:

1. **descriptor capability** — the device declares keyboard and pointer
   collections;
2. **physical classification** — what kind of peripheral this primarily is;
3. **logical role ownership** — which slots it should occupy.

Having both collections does not make a device an integrated combo, and being an
integrated combo is what earns both slots. The 8BitDo is a keyboard that also
declares a pointer; granting it COMBO made it **squat the mouse slot**, so a
separately paired mouse would be refused as `NS2_KBM_ADMIT_REJECT_DUPLICATE` —
breaking the ordinary keyboard-plus-mouse configuration to fix a keyboard.

### The corrected policy

`ns2_kbm_primary_from_evidence()`:

- `declares_combo` (Classic CoD only) + both capabilities → **COMBO**, unchanged;
- `strong_keyboard` + keyboard capability → **KEYBOARD**, and nothing else. The
  pointer capability stays recorded as metadata and claims no slot;
- otherwise capability precedence, pointer first — which is what keeps a gaming
  mouse with macro keys out of the keyboard slot.

The 8BitDo is therefore KEYBOARD-primary: it holds the keyboard slot, its keys
resolve, and the mouse slot stays free for a real mouse. Its own pointer reports
are dropped at `admit_and_route()`, which is correct — it is a keyboard.

### The second defect, which is probably why nothing typed

The profile is derived, not chosen — but the Windows page let the user pick which
profile to EDIT and defaulted to Keyboard regardless of which one the adapter was
resolving. With the 8BitDo previously classified MOUSE, the effective mode was
Keyboard+Mouse and the live profile was `kbm`; a user binding keys on the default
page view was editing `kb`.

Every management operation reports success in that situation — the binding is
saved, survives a reload, and shows in the map — and the key does nothing at the
console. Silence there is indistinguishable from a broken keyboard.

The page now follows the adapter's active profile until the user deliberately
changes it, and says plainly when the profile being edited is not the one in use.

## Hardware result, 2026-08-29

The 8BitDo pairs and reports **keyboard and mouse connected**. That is COMBO, and
it is the intended outcome: the peer now holds both roles rather than only the
mouse.

It also confirms something the fix assumed rather than measured — the device
declares a REAL pointer collection. `bthid_mouse_parse_descriptor()` requires
relative X and Y, so `has_pointer` could not have been true otherwise. The
pointer half is not an artefact of the keyboard parser; the device genuinely
declares a pointing device alongside its keyboard.

### The consequence worth knowing

`ns2_kbm_roles_admit()` gives a COMBO peer every role it offers that is free, so
this keyboard now holds the mouse role as well. **A separate mouse connected
afterwards is refused as a duplicate** (`NS2_KBM_ADMIT_REJECT_DUPLICATE`), because
the composite already has both halves.

For a genuine combo — a keyboard with a trackpad or trackpoint — that is correct,
and it is exactly how a Classic combo peripheral has always behaved. It is only
unwanted if a device declares a pointer collection it cannot actually drive, in
which case it squats the mouse role for nothing.

**Not changed, because nothing has been observed to need it.** If a real mouse is
ever refused while this keyboard is connected, the smallest fix is to let a COMBO
peer yield its pointer half to a peer whose primary role is MOUSE, rather than to
narrow the classification again. Recorded here so that option is found rather than
rediscovered.

## The unrelated timeout in the same session

```
19:57:43.294 DEBUG [ble] retire reason=reply-timeout gen=1
19:57:43.295 ERROR [ui] The adapter did not answer 'peers list 4' within 10000 ms.
```

Not the same defect, and **not an app accounting error**: the reply deadline
starts inside the transport's exchange, *after* the single-flight lock, so
queued work delays when an exchange starts and never shortens the window it is
given. Verified in `BleGattManagementTransport.ExchangeAsync`.

The adapter genuinely did not answer within 10 s. From the host side that is all
that can be established; diagnosing it needs the adapter's own view.

One app-side contributor was found and fixed in the same pass, though it does not
explain the timeout: **Reload was not gated on the KB/M busy flag**, so five
impatient clicks queued five complete re-reads — around twenty extra exchanges
against an adapter that was already slow. The flag existed; the buttons were
simply not wired to it.


## The HOGP report-ID theory, and why it is WRONG (2026-08-29)

After the role fix the keyboard still produced nothing, and the next theory was
that the BLE transport discards the HOGP Report ID: HID-over-GATT carries the ID
in the Report Reference descriptor (0x2908) rather than in the notification
payload, so a transport that only prepends `0xA1` would hand the shared decoder
`[0xA1][payload]` where Classic gives `[0xA1][report_id][payload]`.

**Disproven from source before implementing it, and implementing it would have
broken every BLE HID device.**

BTstack's `hids_host` already inserts the Report ID.
`hids_host_setup_report_event_with_report_id()` writes it at the start of the
report payload, which is why `gattservice_subevent_hid_report_get_report()`
returns `&event[9]` — pointing at the ID — with `report_len = value_len + 1`.
`route_ble_hid_report()` then prepends only the `0xA1` transaction header, and
`bt_on_hid_report_with_generation()` strips exactly that one byte. A driver
receives `[report_id][payload]` on BLE, identical to Classic.

Prepending the ID again would have shifted every BLE report by one byte.
`test_ble_transport_shape_matches_classic` now pins this so the idea cannot come
back, and it checks the shift on the MODIFIER byte rather than on a key: a boot
report is six interchangeable key slots, so a one-byte shift merely moves the key
into the next slot and it still decodes. That is exactly why this class of bug is
hard to see from behaviour.

## Why the printf diagnostic produced nothing

```cmake
pico_enable_stdio_usb(PicoSwitchWGA 0)
pico_enable_stdio_uart(PicoSwitchWGA 0)
```

**Both stdio backends are disabled**, so every `printf` in this firmware is
compiled in and discarded. The UART command console is a separate path
(`uart_putc_raw` in `ns2_uart_diag.c`), which is why `btstate` replies arrive
while nothing unsolicited ever does.

The `[KBM_TRACE]` instrumentation added for this investigation could never have
emitted a line, and has been removed. Any earlier note in this record suggesting
`[BTHID...]` lines are observable over the UART command connection is wrong for
the same reason.

## What is still unexplained

The keyboard produces no console input, and the following are all now proven
correct: descriptor classification, primary role, slot ownership, active profile,
binding persistence, the binding namespace, report-ID handling on both
transports, decode, mapping lookup and resolve. `test_kbm_keyboard_pipeline`
exercises that whole chain from report bytes to a pressed button.

What remains untested by any host test is the runtime admission path
(`admit_and_route()` in `ns2_kbm_runtime.c`), which needs the Bluetooth stack.
Three of its exits still increment no counter: no peer key, classification
pending, and no keyboard role. `kbm status` already reports `keyboardReports`,
`rejectedMode`, `rejectedDuplicate` and `rejectedNotOwner`, so those four are
readable today over the existing command surface without any new firmware.

## The admission path, read from source (2026-08-29)

A new control changes the shape of this investigation: an **ASUS ROG Falchion RX
Low Profile** keyboard was paired to the same adapter in the same debugging
cycle and **works** — its keys reach the gamepad mapping. So the keyboard →
gamepad feature is functional on this adapter, and the downstream publication
path is no longer a plausible common cause. The failure is specific to something
in the 8BitDo's runtime path.

### The branch structure

`admit_and_route()` (`src/bt_hid/ns2_kbm_runtime.c:539`), in order:

| # | Predicate | On failure | Counter |
|---|---|---|---|
| 1 | `peer_key_for_connection()` — is `bthid_get_device(conn_index)` non-NULL? | return false | **none** |
| 2 | `primary == NONE` and `primary_authority_pending()` — a BLE peer on the keyboard driver with no classification yet | return false | **none** |
| 3 | `ns2_kbm_roles_admit()` → `REJECT_MODE` | return false | `rejectedMode` |
| 3 | `ns2_kbm_roles_admit()` → `REJECT_DUPLICATE` | return false | `rejectedDuplicate` |
| 4 | `from_keyboard && !keyboard_role` — admitted, but not to the keyboard slot | return false | **none** |
| 5 | `ns2_active_input_submit_group()` — does the composite own the console? | return false | `rejectedNotOwner` |

Only on reaching the end does `ns2_kbm_runtime_submit_keyboard()` continue to
`s_roles.keyboard_reports++`, state update, resolve and `publish_locked()`.

### The counter names do not mean what they look like

**`keyboardReports` counts ACCEPTED reports, not arriving ones.** It is
incremented at `ns2_kbm_runtime.c:629`, *after* `admit_and_route()` has already
returned true. This matters for reading a test result: a report refused at any
branch above never touches it, and a report dropped at branch 1, 2 or 4 touches
nothing at all.

`publishes` is the only counter that says the output side ran.

`rollover` is incremented in the keyboard *driver*, before admission — but only
on a rollover decode, so it cannot serve as a general arrival counter.

There is therefore **no counter that proves a report arrived**. That is the gap.

### The three silent exits

**1 — no peer key.** `peer_key_for_connection()` fails only when
`bthid_get_device(event->dev_addr)` returns NULL. The report is dispatched from
that same device's driver callback, so the device is live by construction.
*Implausible.*

**2 — classification pending.** `primary_authority_pending()` is
`device->is_ble && device->driver == &bthid_keyboard_driver`. A BLE keyboard-driver
peer whose `primary_lookup()` misses is dropped here, silently, on every report.
The lookup is keyed on **`(conn_index, connection_generation)`**, and
`ns2_kbm_runtime_note_classification()` records it with the generation current
when the *descriptor* arrived, while the report carries the generation current at
*report* time (`bthid_keyboard.c:173`). A generation change between the two, or a
reconnect that does not re-read the descriptor, orphans the classification.
*Plausible, and it is a BLE-only branch — a Classic peer can never reach it.*

**4 — no keyboard role.** Reached when admission succeeded but returned
`ADMIT_MOUSE`: the peer was admitted to the mouse slot only. Requires
`primary == MOUSE`, which contradicts the current strong-keyboard classification.
*Implausible for the current build.*

### What a stale generation would actually look like

Worth stating, because it is the intuitive guess and it is wrong:
`peer_equal()` (`ns2_kbm.c:1087`) compares `connection_generation` first, so a
role held under an older generation makes `already_keyboard` false while
`roles->keyboard.valid` stays true — `keyboard_free` is false, and admission
returns `REJECT_DUPLICATE`. A stale *role* is therefore **visible** as
`rejectedDuplicate`. A stale *classification* is the silent one.

Note also that `kbm status`'s `keyboard=true` reads `s_roles.keyboard.valid`,
which is a different state object from the per-generation classification the
report path looks up. The two can disagree, which is exactly why the status line
alone cannot settle this.

### What can be compared between the ROG and the 8BitDo

Available from `kbm status` alone, without re-pairing either: mode, profile,
which slots are held, `keyboardConn`/`mouseConn`, `group`, `source`, and every
counter. `group` and `source` were always in the reply and had never been read by
either companion; the Windows app now shows them.

Not available from any current diagnostic: transport actually used (BLE vs
Classic), bound driver, HID generation, resolved-identity/RPA path, and the
per-generation classification record — none of which `kbm status` reports. The
`is_ble` fact is the single most valuable missing one, because branch 2 is
BLE-only. **The ROG working does not by itself prove it took a different branch;
it may simply have connected over Classic.** That is not manufactured evidence
either way — it is unknown from the current surface.

### Interpreting the one keypress test

| Result | Meaning |
|---|---|
| `keyboardReports` +1, `publishes` +1 | accepted and published; the failure is downstream of KB/M and this document's premise is wrong |
| `keyboardReports` +1, `publishes` unchanged | admitted, dropped in state/resolve/publish |
| `rejectedMode` +1 | branch 3: role policy or offered-role mismatch |
| `rejectedDuplicate` +1 | branch 3: the slot is held under a different peer key — a stale generation |
| `rejectedNotOwner` +1 | branch 5: the composite does not own the console. `source=0` corroborates |
| **nothing changes** | branch 1, 2 or 4 — or the report never reached `admit_and_route()` at all. These are indistinguishable with the counters that exist |

Only the last row needs new firmware, and it is the outcome the source analysis
makes most likely. Three bounded counters at branches 1, 2 and 4 would separate
it; an arrival counter at the driver would separate "reached admission" from
"never arrived". That instrumentation has deliberately **not** been added yet, to
avoid a flash that the existing counters may make unnecessary.
