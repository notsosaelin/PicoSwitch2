# A BLE keyboard is classified as a mouse

**Status:** root cause traced in firmware source. **No firmware change made** —
the fix reopens a previously fixed defect and needs a decision, not a patch.

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

## Why no fix was made

The obvious change — let a BLE peer reach COMBO when it declares both — reopens
the KERIS II defect verbatim: a gaming mouse with macro keys would take the
keyboard role and lock out the user's real keyboard. That is a regression of
something already fixed, so it needs a decision rather than a patch.

A discriminator that separates the two would have to look at the SHAPE of the
keyboard collection rather than its presence. A real keyboard declares a modifier
byte plus a 6-usage rollover array; a mouse's macro collection is typically a
short bitmap. The parser already distinguishes these — `bthid_keyboard_report_map_t`
carries `bitmap_count` and `array_count` — so the raw material exists. Whether
that separation holds across real hardware is exactly the question that needs
evidence before anything is changed.

## What would settle it

1. Capture the 8BitDo's report descriptor from the adapter's UART during pairing
   (`[BTHID_KEYBOARD] Parsed report ...` already prints `bitmap_count` and
   `array_count`).
2. Capture the same for a gaming mouse with macro keys, if one is to hand.
3. If the array/bitmap shapes separate the two cleanly, the precedence rule can
   consult that for BLE peers only, leaving the Classic path untouched.

Until then the observed behaviour is: **a BLE keyboard with a pointer collection
is used as a mouse, and its keys do not reach the keyboard role.**

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
