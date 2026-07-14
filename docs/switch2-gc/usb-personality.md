# USB Output Personality Architecture

> Status: 🟢 Stage B (implementation) — in progress. This document defines the architecture for a
> native NSO GameCube Controller output personality alongside the existing Switch 2 Pro Controller 2
> personality, without coupling GameCube logic into `switch_pro2.c` and without breaking the existing
> Switch 1 Pro Controller build path.
>
> **2026-07-13 product decision, superseding the "Live switching mechanism"/"Persistence" sections
> below**: personality selection is a **volatile runtime BOOTSEL-hold cycle**, not a persisted,
> reboot-required config-mode setting. The two sections below are kept for their architecture
> reasoning (the disconnect/reconnect mechanism itself, the callback-dispatch design) but their
> *product* conclusions (persist to flash, reboot to switch) are superseded — see "Runtime mode
> cycle" further down for the current design.

## Implementation status (2026-07-13)

**Stage B is implemented, build-verified, and hardware-validated for its actual scope (USB
enumeration).** Confirmed on real hardware, with a genuine NSO GameCube Controller simultaneously
connected for direct comparison: the Pico enumerates cleanly under "Universal Serial Bus devices" as
`Nintendo GameCube Controller`, VID:PID `057E:2073`, with no Windows driver error — and the genuine
controller's own operation was unaffected (see the `bcdDevice` fix below, which was specifically what
made this coexistence possible). Two real bugs were found and fixed via this hardware round — see the
two subsections immediately below.

Steam recognizes the Pico as a device named "Nintendo GameCube Controller" but offers a **"Begin
Setup"** prompt rather than treating it as a fully-supported native controller (the genuine unit gets
no such prompt). This is the expected, already-documented Stage B boundary, not a new bug: Steam's
native (no-manual-setup) recognition depends on the device actually responding to an initialization
handshake over the vendor interface, which Stage B intentionally does not implement
(`switch_gc_vendor_control_xfer()` stalls everything except the WinUSB probe; `switch_gc_task()`
deliberately stays silent — see each function's own comment). That handshake is Stage D's job. This
is real, hardware-confirmed evidence for exactly where Stage B's boundary sits, not a regression.

### Microsoft OS 1.0 WinUSB auto-bind (found + fixed via first hardware round, 2026-07-13)

The owner's first hardware test found GameCube mode enumerated correctly (device/config descriptors
parsed fine, showed as "Nintendo GameCube Controller") but Windows reported error code 28 ("no
compatible drivers") on the whole composite device — while a genuine controller enumerates cleanly.
Root cause: the vendor-specific interface (IF1) has no standard Windows driver, and Stage B hadn't
implemented the same MS OS 1.0 WinUSB auto-bind mechanism this project's Pro2 personality already
relies on for *its* vendor interface (Pro2's own code comments note the retail unit exposes
`USB\MS_COMP_WINUSB`) — the owner's observation that genuine hardware enumerates cleanly is strong
evidence the real GameCube controller ships the same mechanism. Fixed by mirroring Pro2's
implementation exactly for GameCube's vendor interface (`switch_gc_ms_os_string_descriptor()` +
Extended Compat ID response in `switch_gc_vendor_control_xfer()`). Explicitly within Stage B's scope
per NSO-GC.md ("implement a command only in Stage B if Windows enumeration requires it") — a standard
Windows-only mechanism, not the Nintendo-specific identity handshake (`bRequest` 0x02/0x03), which
remains Stage D and untouched.

### `bcdDevice` WinUSB-cache collision with a genuine controller (found + fixed via the SAME hardware round, 2026-07-13)

The WinUSB fix above alone did not resolve the owner's symptom — re-testing with a **genuine NSO
GameCube Controller simultaneously connected** for comparison surfaced a second, more fundamental
issue: intermittent misrecognition (briefly showing as `"If_Hid"`), Code 28 persisting, and — critically
— **the genuine controller's own cached Steam mapping breaking too**, despite this project never
writing anything to that device. Root cause: `switch_gc_device_desc`'s `bcdDevice` used the exact
raw-captured value (`01,01`/BCD 1.01), making the Pico's GameCube identity byte-for-byte identical to
the genuine controller's (VID, PID, bcdDevice, **and** serial — the serial string is literally `"00"`
on both, not a real per-unit value). Windows keys its WinUSB driver-binding cache on
VID+PID+bcdDevice, not a true unique ID, so the two devices became indistinguishable to that cache —
explaining every symptom, including the genuine unit's mapping breaking (pure Windows-side driver
bookkeeping cross-contamination, not any actual write to the real controller).

**This is the identical problem `switch_pro2.c`'s own `ns2_device_desc` already solved** — its
`bcdDevice` is deliberately `2.10`, not the genuine Pro Controller 2's real `2.00`, for exactly this
reason (see that file's own comment, which also established — via real hardware A/B testing on the
Switch 2 console itself — that the console does not gate on `bcdDevice`, only Windows' cache cares).
**Fixed** by applying the identical pattern to GameCube: `bcdDevice` changed to `1.11` (bytes `11,01`),
distinguishable from the real `1.01` in Windows' cache while remaining a plausible-looking version
number. The console-neutrality finding is carried over **by analogy, not independently re-verified
for GameCube** — flag this for the Stage G real-console validation pass. `tools/verify_gc_descriptors.py`
updated to expect the new intentional value, with its own comment explaining the deliberate deviation
from the raw capture. Re-verified across the full build matrix; not yet re-tested on hardware.

Files: `include/usb.h` (`usb_personality_t` enum, cross-core request/ack state — see "Runtime mode
cycle" below), `include/usb_mode_cycle.h` + `src/usb_mode_cycle.c` (pure enum-walking logic, no
pico-sdk dependency, host-testable — `tools/test_usb_mode_cycle.c`), `include/switch_gc.h` +
`src/switch_gc/switch_gc.c` (the GameCube personality module: Confirmed byte-exact descriptors,
Stage-B-scope stubs for everything else — see each function's own doc comment for its exact evidence
tier and Stage boundary), `src/usb.c` (the transition sequence + main-loop dispatch),
`src/usb_descriptors.c` (centralized TinyUSB callback dispatch — descriptors, EP0 vendor control,
mount, HID get/set report), `src/switch_pro2/switch_pro2.c` (its own TinyUSB callbacks renamed to
plain functions so they can be dispatched to, plus a personality guard added to the audio-stub driver
hook and the MS-OS-1.0 WinUSB string), `src/bt_hid/ns2_bt_host.c` (BOOTSEL_HOLD now requests a mode
cycle instead of unconditionally entering config; new highest-priority LED tier renders the
transition acknowledgement), `include/bootsel.h` (comment only — the gesture itself needed no code
change, see "Gesture reuse" below), `tools/verify_gc_descriptors.py` (independent byte-comparison
tool for the GameCube descriptors).

### Gesture reuse — no changes needed to bootsel.c

`bootsel.c`'s existing gesture state machine already satisfied every hold-specific requirement this
task specified, unmodified: `hold_fired` is a latch that only lets `BOOTSEL_HOLD` fire once per
qualifying press (continuing to hold does not re-fire it), and its release-edge handler only
increments the tap counter `if (!hold_fired)` (a release following a completed hold is never counted
as a tap). This was verified by reading the existing code, not assumed — see the "Boot behavior"
audit this document's earlier revision already did before any implementation began.

### Validation performed this pass

- **Descriptor byte-exactness**: `tools/verify_gc_descriptors.py` independently transcribes the
  expected device/config/HID-report descriptor bytes (from the raw capture and ndeadly's repo,
  *not* copied from `switch_gc.c` itself) and diffs them against the actual C source; also checks
  `wTotalLength` self-consistency, interface count, and that the HID report descriptor declares the
  expected report IDs (5, 0x0A, 0x03). All pass.
- **Mode-cycle logic**: `tools/test_usb_mode_cycle.c`, a host-compilable test (`gcc -DNS2_PRO=1
  -I include ...`, no pico-sdk needed) exercising the extracted `usb_next_personality()`/
  `usb_personality_available()` functions directly. Confirms: `USB_PERSONALITY_SWITCH2_PRO2` is enum
  value 0 (so plain zero-initialization already yields the correct boot default); the cycle order is
  Pro2 → GameCube → Config with `USB_PERSONALITY_JOYCON2` transparently skipped (both as the "current"
  value and as an intermediate step); Config is a genuine fixed point (`next(Config) == Config`, no
  wraparound). All pass.
- **`NS2_PRO=OFF` (Switch 1) unchanged behavior**: verified by code inspection, not a host test — the
  `#else` branch of `usb.c`'s main loop (the BOOTSEL-hold-enters-config-directly path) and
  `include/usb.h`'s `#else` branch (the original `g_usb_enter_config`/`g_usb_config_mode` variable
  pair) are both byte-for-byte the pre-existing implementation, untouched by this pass — confirmed via
  diff review, and via a clean `NS2_PRO=OFF` build.
- **No L3/R3 GameCube output destination / ZL/C/Home/Capture remain real capabilities**: Stage B
  introduces no button-mapping/destination code at all (that's Stage C+) — the only place this
  property could currently be violated is the HID report descriptor's own declared button range,
  which `verify_gc_descriptors.py`'s byte-exact check covers; `docs/switch2-gc/mapping.md`'s
  capability table (corrected this pass for the ZL finding) is the policy document Stage C must
  follow.
- **Full build matrix**: both boards' default `NS2_PRO=ON` config, plus `NS2_AUDIO=OFF` and
  `NS2_PRO=OFF` scratch builds — all four green.

## Why this document exists

PicoSwitch2 currently supports exactly one non-CDC USB identity per build, selected at
**compile time** via `#ifdef NS2_PRO` (Switch 2 Pro Controller 2 if defined, Switch 1 Pro Controller
otherwise). Adding a third identity — the native NSO GameCube Controller — the same way (a third
compile-time branch, more `#ifdef` chains scattered through shared files) would directly violate
CLAUDE.md's standing instruction to avoid scattering conditionals and to prefer clean abstractions
over accreted special cases. This document designs the alternative: a small, explicit
personality-selection layer that both the existing two identities and the new one plug into,
informed by a full audit of exactly how tightly today's code is already coupled to Pro Controller 2
(see "Audit findings" below — every claim here cites a specific file:line from that audit, not
assumption).

## Audit findings (source: codebase trace, 2026-07-13)

Full detail preserved for reference; summarized by topic:

1. **Descriptor selection is already a clean, mirrorable pattern.** All four TinyUSB descriptor
   callbacks (`tud_descriptor_device_cb`, `tud_descriptor_configuration_cb`,
   `tud_hid_descriptor_report_cb`, `tud_descriptor_string_cb`, all in `src/usb_descriptors.c`)
   already branch on a single runtime boolean (`g_usb_config_mode`) plus a compile-time flag
   (`#ifdef NS2_PRO`) to select between static descriptor-byte arrays. The Pro2-specific bytes
   themselves are already isolated behind accessor functions in `src/switch_pro2/switch_pro2.c`,
   declared in `include/switch_pro2.h` — this is exactly the shape a new `switch_gc.h`/`switch_gc.c`
   pair should mirror.
2. **TinyUSB already proves live re-enumeration works, with no reflash.** `tud_init()` re-queries the
   descriptor callbacks on every enumeration; the existing CDC-config-mode entry path
   (`src/usb.c`: `tud_disconnect()` → `sleep_ms(100)` → set a selector variable → `tud_connect()`,
   triggered by a BOOTSEL-hold gesture routed from the Bluetooth core) is direct, working, in-tree
   proof that changing the active descriptor set and forcing a host re-enumeration is sufficient —
   **no reflash required**, and this is the mechanism a Pro2⇄GameCube switch should reuse.
3. **Some TinyUSB callbacks are currently "one implementation per link," a real constraint.**
   `tud_vendor_control_xfer_cb`, `tud_mount_cb`, and `usbd_app_driver_get_cb` each have exactly one
   definition in the whole binary today (inside `switch_pro2.c`, gated by `#ifdef NS2_PRO`). A
   runtime-selectable personality needs these to become a small dispatch (`if (active_personality ==
   ...)`) rather than assuming only one non-CDC personality is ever compiled in.
4. **Report scheduling and output/command routing are 100% separate, non-shared implementations
   already** (Pro2's `ns2_task()`/`ns2_dispatch()` vs. Switch 1's inline loop and
   `switch_pro_receive()`) — this is good news: a GameCube personality doesn't need to retrofit into
   either existing implementation, it gets to be its own from-scratch third implementation, following
   whichever shape its own (still partially unconfirmed) protocol needs.
5. **Rumble/feedback routing is already fully personality-agnostic below the USB-decode layer.**
   `report_set_rumble`/`report_get_rumble` (`include/report.h`) and `ns2_seam.c`'s Bluetooth-side
   feedback bridge don't know or care which USB personality is active. Only the USB-side *decode*
   (Pro2's `ns2_hid_out_report()`) is personality-specific — a GameCube decode function is a small,
   additive, low-risk piece.
6. **No persisted "USB personality" field exists yet.** `pico_config_t` (`src/config.c`) has a
   versioned migration path already (`CONFIG_VERSION`, graceful old→new upgrade) that a new
   `usb_personality` byte can follow without disruption.
7. **The Bluetooth/input side (`src/bt_hid/`) has zero coupling to Pro Controller 2 identity** — no
   changes expected there. `NS2_SLOTS` already assumes up to 4 output slots; Pro2 already only uses
   slot 0, so a single-slot GameCube personality introduces no new constraint.

Full audit detail (all ten requested topics, exact file:line citations for every claim above) is
preserved in this session's working notes; the summary above is sufficient to drive the design below.
If deeper citation is needed later, re-run the same trace against current `src/usb_descriptors.c`,
`src/usb.c`, `src/switch_pro2/switch_pro2.c`, `src/switch_pro/switch_pro.c`, `src/config.c` — the
file set is small and stable.

## Design: a runtime personality selector, reusing the CDC-mode precedent exactly

### The selector

Replace the two-state `g_usb_config_mode` boolean (`include/usb.h`) with a multi-state enum. Naming
and ordering per the 2026-07-13 product decision (`USB_PERSONALITY_SWITCH2_PRO2 = 0` so a zero-valued/
freshly-booted default is correct without any explicit initialization):

```c
typedef enum {
    USB_PERSONALITY_SWITCH2_PRO2 = 0,  // boot default
    USB_PERSONALITY_NSO_GAMECUBE,
    USB_PERSONALITY_JOYCON2,           // reserved, not yet implemented -- skipped at runtime
    USB_PERSONALITY_CDC_CONFIG,
} usb_personality_t;
```

One authoritative active-personality value, owned by the USB core (core0) alone. The Bluetooth/gesture
core (core1) may only *request* a transition; it must not write descriptor-visible state directly —
see "Runtime mode cycle" below for the exact request/acknowledge handoff.

`g_usb_config_mode` becomes a derived convenience (`g_usb_personality == USB_PERSONALITY_CDC_CONFIG`)
rather than disappearing outright, to minimize churn in call sites that only care about "am I in
config mode."

The Switch-1-Pro-vs-Switch-2-Pro2 choice **stays a compile-time flag** (`NS2_PRO`) — nothing in
NSO-GC.md asks for that to become runtime-selectable too, and doing so would be scope creep. The new
runtime axis is specifically **Switch2-Pro2 vs. NSO-GameCube**, orthogonal to the existing
compile-time `NS2_PRO` choice. Concretely: a `NS2_PRO=ON` build supports live-switching between
Pro2 and GameCube; a `NS2_PRO=OFF` build remains Switch-1-Pro-only, with no GameCube option (matches
NSO-GC.md's silence on Switch-1+GameCube coexistence, and avoids inventing scope).

### Descriptor callback dispatch

Each of the four descriptor callbacks becomes a `switch (g_usb_personality)` over three arms (CDC /
Pro2 / GameCube), each arm calling into that personality's own accessor module
(`switch_pro2_*`/`switch_gc_*`). This directly generalizes today's two-way ternary — same pattern,
one more arm, no new abstraction invented.

### The "one callback per link" constraint

`tud_vendor_control_xfer_cb`, `tud_mount_cb`, `usbd_app_driver_get_cb` each become a small dispatch
function that forwards to the active personality's own handler (or a no-op, if that personality
doesn't need the hook — e.g. if GameCube turns out not to need a vendor bulk interface at all, or
needs a different EP0 handshake than Pro2's WinUSB MS-OS-1.0 compat-ID bind). **Do not generalize this
into a full vtable/interface struct prematurely** — three concrete `if`/`switch` arms is simpler,
more debuggable, and exactly matches CLAUDE.md's "do not over-generalize; share only what's truly
common" instruction, given there are only three personalities and their protocols are almost entirely
disjoint (per audit finding 4).

### TinyUSB static resource sizing

`tusb_config.h`'s `CFG_TUD_HID`/`CFG_TUD_VENDOR`/`CFG_TUD_CDC`/endpoint-buffer-size `#define`s are
compile-time constants sized for whichever personalities are compiled into a given build — TinyUSB
allocates for the **union** of all compiled-in personalities' needs, not just the currently-active
one. This means a build with GameCube support compiled in pays a small fixed RAM cost for GameCube's
endpoint buffers even while running as Pro2, and vice versa — acceptable (the Pico has ample RAM
headroom for a few extra 64-byte endpoint buffers) and consistent with how Pro2 already pays for
`CFG_TUD_HID=4` (a Switch-1-multi-controller concept) despite only ever using HID instance 0.

### ~~Live switching mechanism~~ / ~~Persistence~~ — SUPERSEDED 2026-07-13, see "Runtime mode cycle" below

*(Original text preserved for its architecture reasoning; the product conclusions below — reboot-only
switching, and persisting the selection to flash — are no longer the plan. Do not implement either.)*

> Reuse `src/usb.c`'s existing CDC-mode entry sequence verbatim for Pro2⇄GameCube switching:
> `tud_disconnect(); sleep_ms(100); g_usb_personality = new_personality; tud_connect();` — **Open
> question, not yet decided**: should switching support a live return path, or require a reboot?
> **Recommendation for Stage F: implement reboot-required switching first**... Add
> `uint8_t usb_personality;` to `pico_config_t`, bump `CONFIG_VERSION`... A new CDC config-mode command
> lets the value be changed from config mode.

**Resolved by explicit product decision (2026-07-13, `NSO-GC.md`)**: the disconnect/reconnect
*mechanism* quoted above is correct and reused as-is. What's superseded is *when* it fires and
*whether it persists*:

- **Not persisted.** No `usb_personality` field in `pico_config_t`, no `CONFIG_VERSION` bump. Every
  power-on/reset boots `USB_PERSONALITY_SWITCH2_PRO2`, unconditionally.
- **Live cycling, not reboot-required.** A single ~5-second BOOTSEL hold advances to the *next
  available* personality and live re-enumerates immediately, no reboot. Full cycle and exact gesture
  semantics: "Runtime mode cycle" below.
- **No new CDC config-mode command** for personality selection — there's nothing to persist or query
  from config mode; the only way to change personality is the BOOTSEL hold gesture.

### Recovery path

Per NSO-GC.md's explicit requirement ("no accidental permanent lockout," "reliable configuration-mode
recovery"): CDC config mode remains reachable from *any* powered non-CDC personality via the same
~5-second hold gesture (it's the terminal state of the cycle — see below), and is **always** reachable
from a cold boot regardless of what was active when the board was last powered off, since nothing
persists. Power-cycling is therefore *also* a full, unconditional reset back to Pro2 — a second,
independent recovery path that requires no gesture at all. Verify both in the Stage B hardware pass:
(a) cycle to GameCube, hold again to reach Config, confirm Config works normally; (b) from GameCube
mode, power-cycle (not hold), confirm the board returns to Pro2 on the next boot.

## Runtime mode cycle (2026-07-13 product decision — current design, supersedes the sections above)

### Cycle definition

```text
Switch 2 Pro Controller 2  (boot default)
    -> NSO GameCube Controller
    -> CDC Config Mode                    (terminal for this powered session)
    -> [power-cycle/reset] -> back to Switch 2 Pro Controller 2
```

`USB_PERSONALITY_JOYCON2` is a reserved enum value for forward compatibility (a stable identity for a
future Joy-Con 2 output personality) but is **skipped at runtime** — no placeholder descriptor is
enumerated, no dead USB device appears. Once Joy-Con 2 output exists, the cycle becomes Pro2 → GameCube
→ Joy-Con2 → Config; until then, one hold from GameCube goes directly to Config. The "next available
personality" logic must walk the enum and skip anything not actually implemented, not hard-code the
two-personality cycle as a special case — this is what makes the future Joy-Con2 insertion a one-line
change (mark it available) rather than a cycle-logic rewrite.

**Device identity for a future implementation — Confirmed, sourced 2026-07-14 from the real Linux
kernel "HID: nintendo" driver** (Vicki Pfau, linux-input mailing list v11 patch series,
`https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b`): all Switch 2 controllers share
VID `0x057E` (Nintendo). PID per type — `USB_DEVICE_ID_NINTENDO_NS2_JOYCONL` (Joy-Con 2 Left) =
`0x2067`, `USB_DEVICE_ID_NINTENDO_NS2_JOYCONR` (Joy-Con 2 Right) = `0x2066`,
`USB_DEVICE_ID_NINTENDO_NS2_PROCON` (Pro Controller 2) = `0x2069`, `USB_DEVICE_ID_NINTENDO_NS2_GCCON`
(GameCube) = `0x2073`. The Pro2/GC values independently confirm this project's own already-Confirmed
PIDs (cross-validated from a second, authoritative source); `0x2067`/`0x2066` for Joy-Con 2 L/R are
new information this project had not independently obtained — useful as a starting point once this
personality is actually built, though `bcdDevice` and full descriptor layout remain unconfirmed (the
kernel driver, being a generic PC host driver, has no reason to distinguish "genuine vs. emulated" the
way this project's own `bcdDevice` discriminator does for GameCube — see "NSO GameCube Controller
output personality" further down for why that collision-avoidance step will likely be needed again).
Also notable by its **absence**: the kernel driver uses bulk endpoints exclusively with no EP0 vendor
control identity handshake (`bRequest` 0x02/0x03/0x04) anywhere — independently supporting this
project's own hard-won finding that this handshake is specifically required by a real Switch 2
*console*, invisible to generic PC/Linux hosts (which is presumably also true of Joy-Con 2 and Pro2,
by the same reasoning that led to the GameCube fix — not yet independently confirmed for those types,
since this project's own EP0 handshake work has so far only been validated for GameCube/Pro2 against a
real console, not against this kernel driver specifically).

Config mode is terminal for the current powered session — no live Config→Pro2 exit path in this pass
(exiting requires unplug/replug or reset, both of which land back on Pro2 per the boot default). For
`NS2_PRO=OFF` builds, the existing Switch-1 behavior is preserved exactly: a ~5-second hold goes
directly to Config, with no GameCube (or any) cycling exposed — the mode-cycle enum/logic may still
compile in that build if the architecture requires it, but it must not become user-visible there.

### Gesture ownership and the double-tap interaction risk

Existing gestures are unchanged: double-tap = pairing window, triple-tap = wipe bonds, both owned by
the Bluetooth/gesture core exactly as today. The ~5-second hold is the *new* gesture, distinct from
"hold to enter config" as it existed before this change (that behavior is now the terminal step of the
cycle, not a separate gesture). **Known risk, explicitly not solved by adding a new combined gesture
in this pass**: the existing gesture state machine can classify two released taps as
`BOOTSEL_DOUBLE_TAP` after its 500ms window while a third press is still ongoing, then later emit
`BOOTSEL_HOLD` — a naive "double-tap-then-hold" shortcut could therefore fire both pairing-window-open
*and* a mode cycle from one physical gesture sequence. This is **not implemented or fixed in this
pass** — documented here as a known future gesture-grammar redesign, not an active requirement. Do not
let this risk motivate changing the proven pairing/wipe gesture classification as a side effect of
adding the hold-to-cycle feature.

### Request/acknowledge handoff between cores

The gesture/BOOTSEL-hold detection stays on the Bluetooth core (core1, per the existing `bootsel.c`/
`ns2_bt_host.c` pattern that already routes `g_usb_enter_config` across cores). Core1 must not write
`g_usb_personality` directly — descriptor-visible state belongs to core0 alone (the same ownership
principle "The selector" above establishes). Instead: core1 sets a request flag (e.g.
`g_usb_mode_cycle_requested = true`) when the hold gesture fires; core0's main USB loop polls that
flag, and when set, performs the full transition (compute next available personality, disconnect,
reset personality state, update `g_usb_personality`, reconnect) and clears the request. This mirrors
the existing `g_usb_enter_config` flag's cross-core contract exactly — no new synchronization pattern
invented.

### Transition sequence

Generalizing the existing Config-entry sequence to cover *any* personality transition, with the
additional requirements NSO-GC.md specifies:

```c
tud_disconnect();
/* bounded detach delay -- see below for why 100ms (the existing precedent's value) is kept */
new_personality = next_available_personality(g_usb_personality);
reset_personality_state(g_usb_personality);   // quiesce the OUTGOING personality's runtime state
g_usb_personality = new_personality;
log_transition(old, new, reason);             // over CDC where available; bounded diagnostic otherwise
led_ack_transition();                          // short, nonblocking, documented pattern
tud_connect();
```

Requirements carried over verbatim from NSO-GC.md (not to be diluted in implementation):

- The hold fires exactly once at the ~5s threshold; continuing to hold past that must not cycle
  repeatedly (needs an edge-triggered "already fired for this press" latch in the gesture detector,
  reset only on release).
- Releasing after a completed hold must not additionally count as a tap (the hold's own release must
  not feed back into the tap-counting state machine).
- Must never fire while a Bluetooth connection/pairing/bond operation is in flight in a way that could
  corrupt it — the transition only touches USB-side state (core0), and per the "What IS shared"
  section below, nothing Bluetooth-side is touched by a personality change, so this should hold
  naturally; verify it explicitly in the Stage B hardware pass rather than assuming.
- The **detach delay stays 100ms**, matching the existing, already-proven-reliable CDC-mode-entry
  precedent (`src/usb.c`) — no evidence was found this session that a different value is needed for
  Pro2⇄GameCube⇄Config, and NSO-GC.md asks for a *documented* bounded value, not a new one invented
  without cause.
- Personality-specific runtime state (report counters, pending transfers, init-handshake stage,
  command buffers, endpoint flags) must be reset on the *outgoing* personality before the switch — no
  leaking Pro2's `ns2_streaming`/init-stage counters into a fresh GameCube session or vice versa.
- Descriptor callbacks must observe one coherent `g_usb_personality` value for the *entire* duration
  of a single enumeration — do not mutate it mid-enumeration (the request/ack handoff above already
  ensures this, since core0 only applies a pending request between `tud_disconnect()` and
  `tud_connect()`, never while TinyUSB is actively enumerating).
- No flash write of any kind occurs merely from cycling modes (this falls out naturally from "not
  persisted," but stated explicitly since it's a hard requirement, not just an implementation detail).

## Directory/module layout

Following the existing `src/switch_pro2/` precedent:

```
src/switch_gc/
    switch_gc.c        # descriptor accessors, tud_* callback implementations, ns2_gc_task()-equivalent
    switch_gc.h
include/switch_gc.h     # or keep the split pattern switch_pro2.h already uses — match whichever
                         # switch_pro2.c/.h actually do once this is implemented, for consistency
```

A small shared dispatch layer (new or added to `src/usb.c`/`src/usb_descriptors.c`) owns the
`g_usb_personality` switch statements themselves — it is not a new abstraction module, just the
existing files' existing `#ifdef`/ternary sites widened to a third arm.

## What must NOT be shared (per audit + explicit NSO-GC.md instruction)

- Report/command byte layouts (Pro2's 8-byte vendor-bulk command protocol vs. GameCube's — still
  partially unconfirmed — protocol) are not to be cross-called; a from-scratch implementation
  following `docs/switch2-gc/protocol.md`'s evidence is correct, not a refactor of `ns2_dispatch()`.
- Factory/SPI tables: Pro2's `factory[]` array and GameCube's equivalent are different memory maps
  entirely (see `docs/switch2-gc/protocol.md` "SPI/factory-data memory map") — separate tables,
  separate address-range dispatch functions, following the *pattern* `ns2_mem_read()` establishes,
  not its *code*.
- Input-report scheduling loops stay separate (per audit finding 4) — do not try to unify
  `ns2_task()` and a hypothetical `switch_gc_task()` into one shared loop; they serve genuinely
  different protocols on genuinely different endpoint sets.

## What IS shared, unconditionally, no new work needed

- `report_set_rumble`/`report_get_rumble` and everything in `ns2_seam.c` (audit finding 5).
- `switch_pro_input_t` (`include/switch_pro.h`) as the cross-core input struct — GameCube's own
  decode function reads from the same struct Pro2 already reads from; it does not need its own
  parallel Bluetooth-side input pipeline.
- The disconnect/reconnect re-enumeration mechanism itself (audit finding 2).
- `config.c`'s versioned-migration persistence pattern (audit finding 6).

## Next steps (Stage B done — see "Implementation status" at the top)

1. ~~Stage B: implement `switch_gc.c`'s descriptor accessors~~ **Done 2026-07-13.** Descriptors are
   Confirmed byte-exact (device/config from the `rumble-procon-gccon.pcapng.gz` decode; HID report
   descriptor from this project's own live USBPcap replug capture, closing the last Stage B evidence
   gap — see `docs/switch2-gc/protocol.md`).
2. ~~Hardware gate: validate Stage B's enumeration and mode cycle on the actual Pico~~ **Done
   2026-07-13.** Confirmed on real hardware with a genuine controller connected simultaneously; see
   STATUS.md "NSO GameCube Controller: Stage B... hardware-validated."
3. ~~Stage C: implement report `0x0A` construction~~ **Implemented 2026-07-13, second pass
   (`PROMPT.md`).** `switch_gc_build_report()`/`switch_gc_encode_report()`
   (`src/switch_gc/switch_gc.c`, `src/switch_gc/switch_gc_encode.c` — the latter extracted as a
   pure, host-testable module mirroring `usb_mode_cycle.c`'s pattern) construct the full 63-byte
   body for **every** documented field: counter, power-info default, A/B/X/Y, D-pad,
   Plus/Minus/Home/Capture, C, ZL, both analog sticks, **and** native GameCube Z, independent L/R
   trigger detents, and continuous analog L/R trigger — the shared `switch_pro_input_t` struct was
   extended with dedicated `gc_extra`/`left_trigger`/`right_trigger` fields
   (`include/switch_pro.h`), populated only for the 8BitDo NGC Modkit (`gc_has_native_layout`-gated,
   `bthid_gamepad.c`) and forwarded unconditionally through `ns2_seam.c`'s `router_submit_input()`.
   Ten golden host tests (`tools/test_switch_gc_report.c`) cover neutral/each-button/native-Z/
   ZL-non-aliasing/analog-range/detent-independence/no-synthesis/simultaneity/L3-R3-unsynthesizable/
   counter cases — all pass. Construction is now **wired into live streaming**, gated behind Stage
   D below (not unconditional). Both boards + `NS2_PRO=OFF` build clean.
4. ~~Stage D (init/factory)~~ **Minimum streaming gate implemented 2026-07-13.** Re-mining this
   project's own already-obtained `rumble-procon-gccon.pcapng.gz` for **bulk vendor-interface (IF1)**
   traffic (previously only its EP0 control requests had been analyzed) found the *actual* USB init
   command sequence, **Confirmed byte-exact** — promoting command 0x03/sub 0x0D ("Initialise USB")
   and 0x03/sub 0x0A ("Select Input Report") from the previous BLE-derived Strong tier to Confirmed
   for USB specifically (the "outer transport framing differs" caveat turned out not to apply to
   these two commands). `switch_gc_vendor_dispatch()` implements exactly this minimum pair (plus a
   defensive ACK for the documented-but-never-observed 0x03/sub 0x03), with a rate-limited
   diagnostic for anything else — see `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`
   "Bulk vendor-interface (IF1) init sequence" for the full frame-numbered evidence.
   `switch_gc_task()` now streams report `0x0A` via `tud_hid_n_report()` only once the console has
   sent the Select-Input-Report command with value `0x0A` — mirrors `switch_pro2.c`'s own
   `ns2_streaming` gate exactly. `bRequest=2`'s EP0 vendor request remains Hypothesis (unrelated to
   this bulk-channel gate) and was not touched this pass.
5. ~~Stage E (rumble)~~ **Provisional implementation, not a permanent protocol conclusion**, per
   explicit 2026-07-13 project-owner direction: `switch_gc_hid_out_report()` preserves the Confirmed
   zero-length-packet idle behavior exactly, corrects the report to 4 rumble-data bytes + 59 reserved
   bytes, and treats any nonzero rumble data opaquely — mapping it to one conservative fixed motor
   level rather than decoding intensity. Also fixed a real, previously-latent bug in the *plumbing*
   this pass: the shared `tud_hid_set_report_cb()` dispatcher ignored TinyUSB's separate `report_id`
   parameter and always treated `buffer[0]` as the report ID — correct by coincidence for interrupt
   OUT (the transport this device actually uses) but wrong for control `SET_REPORT` (which strips
   the ID into that parameter instead). Fixed centrally via a new pure `hid_out_normalize()` helper
   (`src/hid_out_normalize.c`, host-tested), shared by both Pro2's and GameCube's rumble handlers.
   Byte-level rumble semantics stay explicitly unresolved until a deliberate multi-level hardware
   sweep exists; do not treat the fixed-default behavior as evidence of the real encoding.
6. Stage F (persistence/selection) is **done** as of the 2026-07-13 product decision (volatile
   runtime cycle, not persisted) — superseding the original Stage F plan referenced elsewhere in this
   document.
7. **Hardware checkpoint, not yet run.** Everything above is host-tested and build-verified but
   **not hardware-validated** — see `DATA.md` for the exact test procedure and its one honest gap:
   Steam/Windows has no reason to ever send the Stage D command sequence on its own (that's
   Nintendo-console-specific vendor protocol, not something a generic PC host issues), so the
   streaming gate can only be exercised by a real Switch 2 console or a purpose-built raw-bulk-write
   test tool, neither of which exists yet for this project. Do not claim Stage C/D
   hardware-validated until one of those actually happens.
