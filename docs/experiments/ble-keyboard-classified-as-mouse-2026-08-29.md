# A BLE keyboard is classified as a mouse

**Status:** FIXED in firmware and **confirmed on hardware**, 2026-08-29.

The 8BitDo Retro keyboard now pairs and is reported as **keyboard and mouse
connected** — the COMBO outcome. Before the fix the same device reported
`keyboard=False mouse=True` and its keys never reached the keyboard role.

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
