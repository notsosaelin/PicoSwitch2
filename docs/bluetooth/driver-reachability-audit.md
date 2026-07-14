# BT HID Driver Reachability Audit

> Confidence key: **Confirmed** (read directly in code, traced to the call site) / **Strong
> Evidence** (real mechanism, impact not independently hardware-measured) / **Hypothesis** /
> **Unknown**.

## Why this exists

2026-07-12: a rumble fix landed in `xbox_bt.c`/`xbox_ble.c`, built clean, and looked completely
correct — but those files were never registered (`bthid_registry.c` routed all Xbox controllers
through the generic driver instead), so the fix never ran on real hardware. The standing rule since:
**compiled code is not necessarily registered or reachable.** Every driver claim must trace

```
build inclusion → registration call → matcher ordering → actual match evidence
  → init → input task → feedback task → transport send
```

before being called "active," "fixed," or "tested." This doc is the reproducible inventory that
proves that chain for every driver, kept current as drivers change — re-run the greps in
"How to re-verify this" below rather than trusting this table blindly after future edits.

## Result: reachability inventory (2026-07-12)

| Driver | Compiled | Registered | Registry position | Transports (declared) | VID:PID / name match | Fallback if no match |
|---|---|---|---|---|---|---|
| `ds3_bt` (DS3/Sixaxis) | ✅ (glob) | ✅ | 1st | Classic | `0x054C:0x0268`; names "PLAYSTATION(R)3"/"Sony PLAYSTATION"/"SIXAXIS"; COD `0x000508`+no name | next driver |
| `ds4_bt` (DualShock 4) | ✅ | ✅ | 2nd | Classic | `0x054C:{0x05C4,0x09CC}`; excludes DualSense/Xbox PIDs+names; name "Wireless Controller"/"DUALSHOCK 4" | next driver |
| `ds5_bt` (DualSense) | ✅ | ✅ | 3rd | Classic | `0x054C:{0x0CE6,0x0DF2}`; name "DualSense"/"PS5 Controller" | next driver |
| `switch_pro_bt` (Switch 1) | ✅ | ✅ | 4th | **Classic** (fixed 2026-07-12, was unrestricted) | `0x057E:{0x2006,0x2007,0x2009}`; name "Pro Controller"/"Joy-Con" | next driver |
| `switch2_ble` (Switch 2) | ✅ | ✅ | 5th | **BLE** (added 2026-07-12) | `0x057E:{0x2066,0x2067,0x2069,0x2073}` from BLE advertisement manufacturer data (company ID `0x0553`), not GATT DIS | next driver |
| `wii_u_pro_bt` | ✅ | ✅ | 6th (before wiimote, deliberate) | Classic | `0x057E:0x0330`; name "Nintendo RVL-CNT-01-UC" | next driver |
| `wiimote_bt` | ✅ | ✅ | 7th | Classic | `0x057E:0x0306`; name "Nintendo RVL-CNT-01" excluding "-UC" | next driver |
| `xbox_ble` | ✅ | ✅ | 8th | **BLE** (added 2026-07-12, was relying on registration order only) | `0x045E:*` excluding Elite Series 2 (`0x0B05`/`0x0B22`); name "Xbox Wireless Controller"/"Xbox Elite"/"Xbox Adaptive" | next driver |
| `xbox_bt` | ✅ | ✅ | 9th | **Classic** (added 2026-07-12) | same VID/exclusions as above; name "Xbox..."/"Microsoft...Controller" | next driver |
| `stadia_bt` | ✅ | ✅ | 10th | **BLE** (added 2026-07-12, matches `stadia_init()`'s hardcoded transport) | Google VID + Stadia PID; name "Stadia" | next driver |
| `mouthpad_ble` | ✅ | ✅ | 11th | BLE (already self-guarded too) | name "MouthPad"; VID/PID fallback | next driver |
| `bthid_gamepad` (generic) | ✅ | ✅ | 12th, **last** | Both (explicit) | any BLE HID device; Classic COD Peripheral+Joystick/Gamepad | none — universal fallback |

**No compiled-but-unregistered drivers remain.** Cross-checked every `void *_register(void)`
definition against every call in `bthid_registry.c:36-72` — 11 defined, 11 called, one each.

## Bug found and fixed: Switch 1 driver could shadow Switch 2 over BLE

**Confirmed from code.** `switch_pro_bt.c`'s `switch_match()` (Switch 1, registered *before*
`switch2_ble.c`) explicitly excludes Switch 2 PIDs from its VID/PID check (correct), but then falls
through unconditionally to a name-based fallback — `strstr(device_name, "Pro Controller")` — with no
transport guard and no re-check of the PID it just examined. A BLE-connecting Switch 2 Pro
Controller, whose advertised name plausibly also contains the substring "Pro Controller" (unverified
exact string — flagged, see below), could be claimed by the Switch 1 driver before
`switch2_ble_match()` ever runs, since `find_driver()` (`bthid.c`) tries drivers in registration
order and returns on first match.

**Worse: the existing self-healing mechanism doesn't catch this specific case.**
`bthid_update_device_info()` (`bthid.c:180-257`) re-evaluates a device's driver once more accurate
identity (VID/PID from async SDP/DIS, or updated name) arrives — but only re-searches if the
*current* driver's own `match()` call returns `false` on re-check. `switch_match()`, called again
with the now-correct Switch 2 PID, still returns `true` via the same unconditional name fallback —
so `needs_reval` never becomes `true`, and the wrong driver sticks permanently for that connection.

**Fix, implemented as structural infrastructure rather than a one-line patch to this one driver:**
added `bthid_transport_mask_t transports` to `bthid_driver_t` (`bthid.h`) — every driver now
declares which real physical transport(s) its hardware family can ever use (`BTHID_TRANSPORT_CLASSIC`
/ `_BLE` / `_BOTH`, default `_BOTH` if unset for fail-open safety). Checked centrally via a new
`driver_transport_ok()` helper in both `find_driver()` and the re-eval search loop in
`bthid_update_device_info()`, *before* `match()` is ever called — closing this bug class for every
current and future driver at once, not just this one instance. Switch 1 hardware is Classic-only;
Switch 2 is BLE-only (per `switch2_ble.c`'s own header comment); both now declare accordingly.

**While implementing this, also hardened `xbox_ble.c`/`xbox_bt.c`** (same real hardware family,
different transports, same physical device): previously only registration order (`xbox_ble` before
`xbox_bt`) kept them from potentially stealing each other's connections — `xbox_ble_match()` already
had its own `if (!is_ble) return false;` guard, but `xbox_bt_match()` had none, relying entirely on
the BLE driver claiming BLE connections first. Correct today, fragile — a future reordering (e.g.
alphabetizing the registry) would have silently broken it with no compiler or runtime signal. Both
now declare `.transports` explicitly. Also added explicit `.transports = BTHID_TRANSPORT_CLASSIC` to
every Classic-only driver (DS3/DS4/DS5/Wii U Pro/Wiimote) and `_BLE` to every BLE-only one
(Stadia/MouthPad, both previously correct only by luck of narrow name/VID matching, not by
structural guarantee) for the same defense-in-depth reason, and `_BOTH` explicitly on the generic
driver for clarity.

**Not independently confirmed:** the exact BLE-advertised name string of a real Switch 2 Pro
Controller (whether it literally contains "Pro Controller" as a substring) — this project has no
direct capture of that specific advertisement payload's name field on record. The fix does not
depend on this being true (the transport guard closes the bug regardless of the exact string), but
it's flagged as the concrete scenario that made this a real risk rather than a theoretical one, and
worth confirming via the identity log (see the Gate 2 identity-log work) next time a Switch 2
controller connects.

## Other matcher-overlap risks reviewed and found narrow/acceptable

- `ds3_bt`'s COD-based fallback (`cod == 0x000508 && no name`) is registered first among Sony
  drivers; the file's own comment acknowledges "may also match some other legacy gamepads" — narrow
  (exact COD + empty name only) and unchanged this pass; flagged, not fixed, no concrete collision
  identified.
- `ds4_bt.c` explicitly excludes DualSense and Xbox by both PID and name before its own name match —
  good existing defensive pattern, no change needed.
- No name-substring collisions found among the remaining Classic-only drivers'
  ("Wireless Controller", "DUALSHOCK 4", "Nintendo RVL-CNT-01"[-UC], Xbox strings) or BLE-only
  drivers' ("Stadia", "MouthPad", Xbox strings) match strings.

## How to re-verify this table after future changes

```
# Every driver file has both, exactly once:
grep -rn "_register(void)" src/bt_hid/bt/bthid/devices/*.c src/bt_hid/bt/bthid/devices/**/*.c
grep -n "_register();" src/bt_hid/bt/bthid/bthid_registry.c
# Should be the same count, one-to-one.

# Every driver struct declares .transports (grep for its absence = defaults to BOTH, worth a look):
grep -L "\.transports" src/bt_hid/bt/bthid/devices/**/*.c
```

## Remaining Gate 2 work this audit feeds

- Identity acquisition tracing (why VID/PID often reads `0000:0000`) — separate section, see
  `docs/bluetooth/btstack-implementation.md` "Gate 2: identity and driver-binding architecture".
- A host-side/config-mode reachability self-check (this doc is the human-readable version; DATA.md
  asked for "a small registry self-audit/debug dump" as an option) — not built this pass, this
  document plus the `driver_transport_ok()` structural guarantee were judged sufficient for now
  since the actual bug class (silent shadowing) is now structurally prevented, not just documented.
  Revisit if a new instance of this bug class is found despite the guard.
