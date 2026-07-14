# joypad-os Upstream Comparison — 2026-07-12

## Provenance

- Upstream: `https://github.com/joypad-ai/joypad-os.git`
- Branch: `main`
- HEAD SHA: `b29200581d7376f8d2419c1e2b920b50a3d042f0`
- HEAD commit date: 2026-07-11 10:10:18 -0500
- Retrieval date: 2026-07-12 (this session), fresh `git clone` into a scratch directory outside
  the tracked tree (not a cached checkout)
- Upstream `VERSION`: `2.2.0` (CHANGELOG's `[2.2.0]` entry dated 2026-06-23; HEAD is newer,
  unreleased commits on top)
- This project vendors only `src/bt/` (as `src/bt_hid/bt/`) from a much larger monorepo — upstream
  is a multi-platform (RP2040/RP2350/ESP32/nRF/WCH), multi-app (30+ output targets: GameCube, N64,
  3DO, PCEngine, Nuon, Casio Loopy, USB HID, …) firmware project. PicoSwitch2 only needs the BT HID
  host + drivers; do not chase parity on anything outside that scope.

## Nearest vendored base

Not independently determinable from this repo's git history (no vendoring commit references an
upstream SHA). Most driver files diverge only in PicoSwitch2-specific comments/fixes (a handful of
lines each — see below), suggesting the vendored base is fairly recent relative to HEAD, but this is
inference, not a confirmed fact.

## Registry & driver set

**Confirmed identical, still current:** upstream's `bthid_registry.c` (last touched 2026-07-08,
commit `f5c4f0c`) still contains the exact same comment this project originally had before
2026-07-12's re-registration decision: *"xbox_bt.h and xbox_ble.h no longer registered — generic
driver handles all Xbox."* Both files still exist upstream (compiled, unregistered) but are not
wired in. **This is a live, current, deliberate upstream design choice, not stale/abandoned code.**

This directly bears on this project's 2026-07-12 decision (made after the "Xbox fix landed in dead
code" incident) to re-register `xbox_bt.c`/`xbox_ble.c` locally. That decision was made explicitly
per user instruction after the dead-code discovery, with a documented, acknowledged risk
(`xbox_bt.c`'s Classic BT input parsing is unverified — see `driver-reachability-audit.md`). Upstream
staying on the generic driver for Xbox is **new evidence that cuts the same direction as that
risk** — worth weighing, but not grounds to silently revert a decision the user already made with
the risk explained. Flagged prominently; recommend prioritizing the "Xbox transport used and
whether buttons/sticks remained correct" hardware check already listed in DATA.md's pending items.

**New upstream driver not present locally:** `devices/generic/sinput_ble.c` — matches joypad-os's
own native "SInput" BLE protocol (VID:PID `2E8A:10C6`), registered before the generic fallback.
This is joypad-os's own device-to-device output protocol for daisy-chaining joypad-os units; not
relevant to bridging third-party controllers to a Switch 2, and not ported.

**New upstream infrastructure not present locally, worth adopting selectively:**
`src/bt/btstack/bt_device_db.c`/`.h` — a centralized, priority-ordered device-profile lookup
(`bt_device_lookup(name, company_id)`: BLE manufacturer-data company ID checked first, then a
single ordered name-substring table, e.g. `{"Pro Controller", &BT_PROFILE_SWITCH}`) that
consolidates identification for Classic-connection-strategy selection (HID Host vs. direct L2CAP
for Wiimote-family), BLE strategy, HID protocol mode, and pairing PIN type. **Decision: not ported
wholesale.** It's a different architectural pattern (centralized table vs. this project's per-driver
`match()` callbacks) and porting it would mean re-deriving every PicoSwitch2-specific fix already
layered onto the callback architecture (rumble format fixes, `find_player_index`, the new
`bthid_transport_mask_t` guard, etc.) against a new structure — high risk, not clearly justified by
the value. **Notable finding while reading it, though:** upstream's own name table has a *single*
shared `"Pro Controller"` entry covering both Switch generations, disambiguated only by checking
company ID *first*. That means upstream has the same theoretical residual gap this project just
closed structurally: if company-ID resolution ever failed for a real Switch 2 device, upstream's own
lookup would misclassify it via the shared name entry too, same as PicoSwitch2's `switch_pro_bt.c`
bug did before the 2026-07-12 `bthid_transport_mask_t` fix. This project's fix (structural
per-transport rejection, independent of whether identity resolution succeeds) is arguably **more
robust than upstream's current design** for this specific case — worth noting as a genuine local
improvement, not just parity work.

## Xbox rumble: PicoSwitch2 has a fix upstream lacks

Upstream's `bthid_gamepad.c` (the actually-reachable Xbox rumble path in both projects, per the
registry finding above) still sends `loop_count = 0x00` — grepped upstream for `loop_count`/`0xEB`/
`pulse_sustain`: zero hits anywhere in `devices/`. **This project's 2026-07-12 fix (byte 7 = `0xEB`,
verified against the Linux `xpadneo` reference driver) is a genuine improvement beyond upstream**,
not something to revert or reconcile. Candidate to eventually contribute upstream, not done this
pass (out of scope — this is a vendored-fork audit, not an upstream contribution task).

## PicoSwitch2-only local improvements found in `bthid_gamepad.c` — do not lose these

Diffing local vs. upstream surfaced two substantial, hardware-validated local features **upstream
does not have at all**:

1. **Xbox Elite Series 2 back-paddle support** (`is_elite2`, 20-byte report byte 19 bit-parsing:
   `R4=0x01, R5=0x02, L4=0x04, L5=0x08`) — comment cites a real hardware capture ("Captured on
   hardware in byte 19 of a 20-byte report"). Upstream's generic driver has no Elite-2-specific
   logic at all.
2. **Xbox name-based VID fallback** (`is_xbox = (vendor_id == 0x045E) || strstr(name, "Xbox")`) —
   comment: "BLE PnP VID/PID often doesn't resolve, so also match Xbox by name (the driver already
   relies on the name; button output is correct, which proves is_xbox holds)." Upstream's equivalent
   is VID-only (`is_xbox = (vendor_id == 0x045E)`), meaning upstream would fail to apply Xbox-specific
   button-map fixups whenever BLE PnP VID/PID doesn't resolve — exactly the class of problem Gate 2 is
   chartered to fix, and this project already has a working, hardware-validated mitigation for the
   button-mapping half of it (though not for driver *selection*, which is a separate mechanism — see
   the identity-architecture doc).

These predate this session (not something introduced today) and were found only by diffing against
upstream — they are not documented elsewhere as "PicoSwitch2 improvements" as far as this audit
found. Recorded here so a future upstream sync doesn't silently overwrite them with upstream's
simpler versions.

## DS5/DualSense: divergence is upstream scope creep, not a missed fix

Upstream's `ds5_bt.c` is 2227 lines vs. this project's 689 — a 1588-line diff. Read upstream's added
functions (`ds5_voice_play/stop/quip`, `ds5_companion_mic_capture/push_speak/get_ctx/get_ctx2/fx`, a
`comp_ring` audio buffer, `DS5_BTN_NAMES`): this is a DualSense speaker/mic-based "companion"
feature (state names like `ds5_comp_enter_listen`/`enter_think`, context fields like
`pets`/`flipped`/`idle_min` suggest a virtual-pet/voice-assistant feature using the controller's
built-in audio hardware) — entirely outside this project's scope. **The actual core rumble/LED
function, `ds5_send_output()`, is byte-for-byte identical between the two projects** (same CRC
function, same comments, same layout) — confirming this project's independent DS4/DS5 CRC fixes
this session were not missing anything upstream already had. **Not ported. Do not chase this size
gap** — it reflects upstream scope, not a gap in this project's actual target feature set.

## 8BitDo: real-world matching lessons, no NGC-specific code

Upstream has driver logic for the 8BitDo **M30** (digital-triggers-exposed-as-analog quirk, matched
by name "since some units never resolve VID/PID over BT... and others report different PIDs") and a
button map for the 8BitDo **Ultimate** (BLE, VID `0x2DC8`, >14 buttons) — neither is the 8BitDo NGC
DIY controller this project's Gate 3 target cares about, and neither was ported (different hardware,
different report shapes, would need independent verification). **The real value is the confirmed
real-world lesson**: 8BitDo controllers are known, upstream-hardware-tested to be unreliable for
VID/PID-based identification over BT — some units never resolve it at all, others report different
PIDs across firmware/mode variants. This directly validates DATA.md's own name-plus-report-shape
approach for the 8BitDo NGC DIY profiling work (Gate 2 §"Immediate profiling target") as the right
strategy, not an assumption to second-guess.

## GameCube / Switch 2 NSO GameCube controller

`switch2_ble.c` is essentially unchanged (3-line diff, matching this session's `.transports`
addition only) — this project's existing `SW2_GC_PID`/axis-range constants are already in sync with
upstream's input-side NSO GameCube controller support. CHANGELOG entries reference GameCube
*output*-side work (`gc2usb`, GC adapter emulation, GameCube profile system) that lives in
`src/apps/` — out of scope for this project's vendored subset and Gate 3 (USB output personality)
hasn't started per DATA.md's explicit sequencing.

## Identity architecture: PnP-ID / DIS handling is identical, confirming a shared design tradeoff

`dis_client_handler()` (`btstack_host.c`) is functionally identical between projects (same BTstack
`device_information_service_client` accessor calls, same event codes) — the PnP-ID *parsing* itself
is not a divergence and not a bug (BTstack's own library decodes the raw 7-byte characteristic in
both projects identically). **What both projects share, confirmed from code, is the actual root
cause of "VID/PID often reads 0000:0000" at match time**: `device_information_service_client_query()`
is only started after `GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION` (i.e., *after* HID
notifications are already enabled) — deliberately, per an existing comment explaining that running
DIS concurrently with HID notification setup previously starved the notification enable on
devices with many report characteristics and dropped the link. **Driver binding happens earlier**
(`bt_on_hid_ready()`, fired at `GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED`), so VID/PID is
correctly, unavoidably `0` at the moment of first driver selection for any BLE device that doesn't
supply identity pre-connection (i.e., anything without Switch 2's manufacturer-data company-ID
shortcut). Full detail and the resulting fix: see "Gate 2: identity and driver-binding architecture"
in `docs/bluetooth/btstack-implementation.md`.

## Summary: port / reject / defer

| Item | Decision | Reason |
|---|---|---|
| Xbox unregistered, generic driver handles it all | **Consistent with upstream — flag risk, don't silently revert** | Upstream still does this deliberately; local re-registration was a user decision made with the risk already explained |
| `sinput_ble.c` | Reject | joypad-os's own device-to-device protocol, not relevant here |
| `bt_device_db.c` centralized profile table | Defer (documented, not ported) | Different architecture; local per-driver `transports` fix achieves the safety goal already, arguably more robustly for the one case checked |
| Xbox `loop_count` fix | N/A — local is ahead | Not present upstream; keep, candidate to contribute upstream later |
| `bthid_gamepad.c` Elite Series 2 + Xbox name-fallback | **Preserve — do not overwrite with upstream's simpler version** | Hardware-validated local improvements upstream lacks |
| DS5 companion/voice features | Reject | Out of scope; core rumble path already matches upstream exactly |
| 8BitDo M30/Ultimate drivers | Reject (not applicable hardware) | Different 8BitDo models; NGC DIY needs its own profiling per DATA.md |
| DIS-after-notifications timing | Confirmed shared design, not a bug to "fix" by reordering | Reordering risks reintroducing the GATT-contention regression that timing was built to avoid; mitigate via re-evaluation robustness instead (see identity architecture doc) |
