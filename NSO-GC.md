# Claude Code Handoff: Implement NSO GameCube Stage B and Runtime USB Mode Cycling

> **SUPERSEDED 2026-07-13.** This handoff's Stage B/mode-cycling work is complete and
> hardware-validated. Stage C (report `0x0A` construction, 8BitDo NGC Modkit GameCube mapping),
> Stage D (minimum streaming gate), and a provisional Stage E were implemented in the follow-up
> pass driven by `PROMPT.md`. Do **not** treat this file's Stage B instructions as describing
> current/pending work — they describe what has already been done. For current status, evidence,
> and the next recommended action, read `DATA.md` (the live handoff), `STATUS.md`, `PLAN.md`, and
> `docs/switch2-gc/*` instead. This file is preserved for its historical evidence and
> product-decision record (the USB mode-cycle UX spec below is still the governing design), not as
> an active task list.

Read this file completely, then reassess the current repository and dirty worktree before changing anything. Preserve all existing work. Do not commit, reset, clean, discard, or overwrite unrelated changes.

The repository is authoritative for implementation details, but the project owner's latest hardware observations and product decisions in this handoff supersede stale statements elsewhere. Where documents conflict, reconcile them explicitly rather than silently choosing one.

## Current starting point

The preceding pass completed NSO GameCube Stage A research and architecture. `DATA.md` contains the detailed handoff. The most important current facts are:

- No NSO GameCube firmware personality has been implemented yet.
- Stage B is no longer evidence-blocked.
- The genuine device/configuration descriptor bytes are Confirmed against two independent physical NSO GameCube controllers.
- Input report `0x0A` framing and field layout are Confirmed from real traffic.
- Output report `0x03` framing is Confirmed; its exact rumble intensity encoding remains unresolved.
- Part of the genuine initialization/factory handshake uses EP0 vendor control requests, including Confirmed `bRequest=3` factory-data behavior.
- `ndeadly/switch2_controller_research` is the primary native-NSO protocol source.
- SoulCalDan's repository implements the older Wii U GameCube Adapter (`057E:0337`), not the native NSO GameCube Controller (`057E:2073`). Use it only as secondary evidence for physical GameCube trigger/stick behavior.
- The genuine NSO GameCube Controller physically has ZL, C/GameChat, Home, and Capture.
- The genuine NSO GameCube Controller physically does **not** have L3 or R3.
- The 8BitDo NGC Modkit's L3-labelled/R3-labelled extra inputs remain source-side aliases for Capture/Home; they are not native NSO GameCube L3/R3 outputs.
- The Bluetooth pairing-window fix is already implemented and build-verified but remains hardware-validation-pending. Do not reopen or refactor it during this task.

Primary evidence and design documents:

- `DATA.md`
- `docs/switch2-gc/protocol.md`
- `docs/switch2-gc/usb-personality.md`
- `docs/switch2-gc/mapping.md`
- `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md`
- `docs/experiments/nso-gc-reference-repo-audit-2026-07-13.md`
- `docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md`
- `docs/experiments/nso-gc-captures/`

Fresh reference clones already exist outside the worktree at `E:\nso-gc-refs\`. Verify their recorded SHAs before relying on them; do not re-clone merely for ceremony if the existing clones still match the documented commits.

## First action: reconcile stale documentation

Before implementation, make a small evidence-preserving documentation correction pass. Do not rewrite unrelated history.

Correct at least these known contradictions:

1. `CLAUDE.md` still says NSO GameCube work is documentation-only and should wait for Pro Controller 2 maturity. That policy has been explicitly superseded by the project owner. Update it so native NSO GameCube implementation is the active highest-priority feature, with Stage A complete and Stage B authorized.
2. `STATUS.md` has a heading that still says Stage B is evidence-blocked even though the same section later records that the blocker was resolved. Make its heading and current-state summary internally consistent.
3. `PLAN.md` must describe Stage B as ready/active rather than blocked.
4. `docs/switch2-gc/mapping.md` incorrectly says physical ZL is absent or always zero. The owner has physically confirmed that the genuine NSO GameCube Controller has ZL. Correct the capability table and any derived statements. Keep the confirmed absence of L3/R3.
5. `docs/switch2-gc/usb-personality.md` currently recommends persisted, reboot-required personality selection. Replace that product decision with the volatile runtime cycle specified below. Preserve useful architecture audit material and clearly mark the old recommendation as superseded rather than leaving contradictory instructions.

Also audit the directly related GameCube documents for the same contradictions. Do not turn this into a repository-wide prose cleanup before Stage B.

## Product decision: USB mode behavior

Implement the following first-cut UX exactly unless a concrete TinyUSB/hardware constraint makes it unsafe. If such a constraint exists, demonstrate it from code or documentation before proposing the smallest alternative.

### Boot behavior

- Every fresh power-on or reset starts in **Switch 2 Pro Controller 2** mode.
- The runtime-selected personality is **not persisted to flash** in this first cut.
- Do not add a `usb_personality` field to `pico_config_t` and do not bump `CONFIG_VERSION` solely for mode selection.
- Power-cycling is the reliable recovery path back to Pro Controller 2.
- Existing saved Bluetooth bonds, mappings, and other configuration remain persistent and unaffected.

### BOOTSEL mode cycle

A single BOOTSEL hold of approximately five seconds advances to the next available runtime USB personality and live re-enumerates:

Current implemented cycle:

```text
Switch 2 Pro Controller 2
    -> NSO GameCube Controller
    -> CDC Config Mode
    -> power-cycle/reset returns to Switch 2 Pro Controller 2
```

Reserved future cycle, once Joy-Con 2 output exists:

```text
Switch 2 Pro Controller 2
    -> NSO GameCube Controller
    -> Joy-Con 2
    -> CDC Config Mode
    -> power-cycle/reset returns to Switch 2 Pro Controller 2
```

Define a stable `USB_PERSONALITY_JOYCON2` enum/reserved identity if that improves forward compatibility, but **skip unavailable personalities at runtime**. Do not enumerate a placeholder/dead Joy-Con 2 USB device and do not add fake descriptors. Today one hold from GameCube mode must go directly to Config mode.

Config mode remains terminal for the current powered session. Existing gesture polling is suppressed in config mode, and exiting config mode by unplug/replug or reset returns to the default Pro Controller 2 mode. Do not add a live Config-to-Pro2 exit path in this pass.

For `NS2_PRO=OFF`, preserve existing Switch 1 behavior: a five-second hold enters Config mode directly. Do not silently add NSO GameCube cycling to the Switch 1 build axis unless the build architecture absolutely requires compiling shared code; it must not become a user-visible mode there.

### Preserve existing gestures

- Double-tap BOOTSEL: pairing window, unchanged.
- Triple-tap BOOTSEL: wipe saved Bluetooth devices, unchanged.
- Hold approximately five seconds: runtime USB personality cycle as specified above.

Do **not** implement “double-tap then hold for five seconds” as a Config shortcut in this pass.

The current gesture state machine can classify the first two released taps as `BOOTSEL_DOUBLE_TAP` after its 500 ms window while a third press is still being held, then later emit `BOOTSEL_HOLD`. A naive implementation would therefore open pairing and switch USB modes from one gesture. Avoid changing proven pairing/wipe semantics for a convenience shortcut that is not necessary. Document this as a possible future gesture-grammar redesign, not an active requirement.

### Transition semantics

Generalize the existing working Config entry path:

```text
tud_disconnect()
bounded detach delay
change active personality
reset personality-specific runtime/endpoint state
tud_connect()
```

Requirements:

- The hold fires once at the threshold; continuing to hold must not cycle repeatedly.
- Releasing after a completed hold must not count as a tap.
- A transition must not corrupt Bluetooth connection/bond state.
- Quiesce or reset old-personality USB state so counters, pending transfers, initialization stage, command buffers, and endpoint state cannot leak into the new personality.
- The descriptor callbacks must observe one coherent active personality throughout each enumeration.
- Do not change the active personality while TinyUSB is still logically connected.
- Use a bounded detach interval appropriate for reliable host re-enumeration; document why the chosen value is sufficient.
- Log old personality, new personality, transition reason, and success/failure over CDC where available and through bounded normal diagnostics where useful.
- Add a short, nonblocking, documented LED acknowledgement for mode changes if it can be done without obscuring pairing/wipe/connected status. Prefer a small explicit transition indication over permanently overloading existing Bluetooth status patterns.
- No flash write should occur merely because a user cycles modes.

## Stage B objective

Implement the clean runtime personality boundary and make the Pico enumerate on Windows as the genuine native NSO GameCube Controller using the Confirmed identity/topology.

Stage B is an architecture and exact-enumeration milestone. Do not expand it into speculative full protocol emulation, but build the correct seams for Stages C-E rather than creating a throwaway descriptor-only hack.

### Required personality model

Introduce an explicit runtime type, with names adapted to repository conventions if needed:

```c
typedef enum {
    USB_PERSONALITY_SWITCH2_PRO2 = 0,
    USB_PERSONALITY_NSO_GAMECUBE,
    USB_PERSONALITY_JOYCON2,       // reserved/unavailable for now
    USB_PERSONALITY_CDC_CONFIG,
} usb_personality_t;
```

Expose one authoritative active-personality value owned by the USB core. Other cores may request a transition, but must not mutate descriptor-visible state directly.

Keep `g_usb_config_mode` only as a derived compatibility helper if that meaningfully reduces risky churn. Do not allow it and the enum to become two independently writable sources of truth.

Use an explicit request/acknowledgement handoff between the Bluetooth/gesture core and USB core. The gesture core requests “advance to next available personality”; the USB core owns disconnect, state reset, selector update, and reconnect.

Do not over-engineer a generic plugin framework, but do not scatter `if (gamecube)` throughout `switch_pro2.c`. A small centralized dispatch in `usb.c`/`usb_descriptors.c` plus dedicated personality modules is appropriate.

### NSO GameCube module

Add a dedicated module following current layout conventions, for example:

```text
src/switch_gc/
    switch_gc.c
    switch_gc.h
include/switch_gc.h        # only if consistent with current include layout
```

It should own GameCube-specific:

- device/configuration/HID report/string descriptor accessors
- runtime reset/init state
- task/report scheduling entry point, even if Stage B only provides safe neutral behavior
- output-report entry point
- command/vendor handling entry points
- factory-data access entry point
- future report `0x0A` construction
- future report `0x03` rumble decode

Stubs must be explicit, bounded, and documented. Do not silently reuse Pro2 byte layouts for unknown GameCube behavior.

### Exact Stage B USB identity/topology

Implement the evidence-backed normal-mode identity:

- VID `0x057E`
- PID `0x2073`
- `bcdUSB = 0x0200`
- raw `bcdDevice` bytes exactly as captured (`01 01`); audit the prose representation because `0x0101` is BCD 1.01 even though an older comment calls it “2.01”
- composite class/subclass/protocol `0xEF/0x02/0x01`
- EP0 max packet size 64
- manufacturer `Nintendo`
- product `Nintendo GameCube Controller`
- serial string `00`
- exact Confirmed 80-byte configuration descriptor tree
- HID interface with its exact interrupt IN/OUT endpoints, packet sizes, and intervals
- vendor-specific interface with its exact bulk IN/OUT endpoints and packet sizes
- exact interface association descriptors
- correct configuration attributes, including `bmAttributes = 0xC0`

Use the preserved raw capture as the byte-level source of truth. Add compile-time size checks and focused descriptor byte-comparison tests or a reproducible verification tool so future edits cannot silently drift.

### HID report descriptor evidence gap

The raw 97-byte HID report descriptor payload has not been captured independently as a standalone transfer, but ndeadly documents a complete payload and real report traffic confirms its effective report structure.

For Stage B:

- use the documented descriptor payload if necessary to enumerate;
- label its exact byte encoding Strong rather than Confirmed;
- verify its length is exactly 97 bytes;
- decode it with an independent HID descriptor parser or test and compare the declared report IDs/sizes against the Confirmed live traffic;
- do not claim the raw descriptor bytes are Confirmed until captured from hardware;
- do not block Stage B solely on this remaining evidence distinction.

### TinyUSB dispatch and resource constraints

Audit and centralize every one-definition TinyUSB callback currently owned by Pro2, including at least:

- descriptor callbacks
- `tud_vendor_control_xfer_cb`
- `tud_mount_cb`
- `usbd_app_driver_get_cb`
- HID get/set report callbacks if shared globally
- any endpoint or vendor-driver callback that assumes Pro2

Dispatch to the active personality. Avoid duplicate global callback definitions.

Size TinyUSB's compile-time resources for the union of all personalities compiled into the `NS2_PRO=ON` firmware. Confirm endpoint numbers and class-driver resources do not collide. Add static assertions or comments where TinyUSB constraints are non-obvious.

The CDC Config personality must continue to enumerate and serve the existing configuration UI after cycling through GameCube mode.

### Safe Stage B runtime behavior

Stage B must not send malformed active reports merely to appear busy.

- Provide a clearly documented neutral/no-input behavior sufficient for safe Windows enumeration.
- Keep unknown command and output-report behavior bounded.
- Stall or reject unsupported requests only when that matches TinyUSB expectations and cannot wedge enumeration.
- Preserve raw/unknown request diagnostics with rate limiting where useful for Stage D research.
- Do not fabricate per-unit serials, Bluetooth keys, calibration, or SPI identity from the supplied dumps.
- Do not copy the genuine unit's private/per-unit data into firmware.

EP0 `bRequest=3` and other initialization/factory behavior belong primarily to Stage D. Build the dispatch seam now, but implement a command only in Stage B if Windows enumeration requires it or the Confirmed response is small and safe. Keep evidence and implementation status explicit.

## Validation required this pass

### Offline tests

Add focused tests or reproducible checks for:

- exact GameCube device descriptor bytes
- exact GameCube configuration descriptor bytes
- descriptor total lengths and interface/endpoint counts
- 97-byte HID report descriptor length and decoded report declarations
- mode-cycle next-available logic
- Joy-Con 2 placeholder is skipped while unavailable
- default personality is always Pro2 after boot/reset initialization
- Config is terminal until reset in this first cut
- `NS2_PRO=OFF` hold behavior remains direct-to-Config
- no L3/R3 native GameCube output destination is introduced
- ZL/C/Home/Capture remain real native GameCube capabilities

### Build matrix

Run the established complete build matrix, including both supported boards and all previously required feature combinations. At minimum preserve:

- default `NS2_PRO=ON`
- `NS2_AUDIO=ON` if still part of the supported matrix
- `NS2_PRO=OFF`
- Pico W
- Pico 2 W

GameCube support must be present in the normal `NS2_PRO=ON` artifact and must not require a separate special UF2 unless a demonstrated flash/RAM/endpoint constraint forces that decision.

Report final binary/RAM/flash-size changes if the build tooling exposes them.

### Hardware handoff

After Stage B implementation and clean builds, stop at the first meaningful hardware gate and provide:

1. Exact UF2 path for the owner's board.
2. Exact flash steps only if they differ from the established process.
3. A short test sequence:
   - boot and confirm Pro Controller 2 is still the default;
   - hold BOOTSEL for about five seconds;
   - confirm disconnect/re-enumeration as `Nintendo GameCube Controller`, VID:PID `057E:2073`;
   - compare Windows descriptor topology against the genuine controller;
   - hold BOOTSEL again;
   - confirm CDC Config mode still enumerates and works;
   - power-cycle;
   - confirm return to Pro Controller 2;
   - confirm double-tap pairing and triple-tap wipe gestures were not accidentally changed (do not require destructive triple-tap testing unless the owner explicitly agrees to erase bonds).
4. Expected LED/log evidence for each transition.
5. Any known Stage B limitation, especially that full input/init/rumble compatibility is not yet claimed.

Do not call Stage B hardware-validated until this sequence is run on the Pico. Do not call NSO GameCube mode complete until it is accepted and exercised by a real Switch 2.

## Scope boundaries for this pass

Do not let unresolved Stage C/E questions block Stage B:

- exact per-button bit validation
- exact report `0x03` intensity curve
- full console initialization
- complete factory/SPI emulation
- Joy-Con 2 output implementation

Do not silently implement speculative answers to those questions either.

If Stage B compiles and offline tests pass, the next highest-value action is hardware enumeration/mode-cycle validation. Do not bury that gate under a large Stage C implementation before testing the new architecture.

## Documentation and handoff requirements

Update relevant documentation to reflect what was actually implemented and tested:

- `docs/switch2-gc/usb-personality.md`
- `docs/switch2-gc/protocol.md` only where implementation/evidence status changes
- `docs/switch2-gc/mapping.md` for the ZL correction and any model decisions
- `STATUS.md`
- `PLAN.md`
- `SESSION.md` if this repository still uses it for chronological handoffs
- `README.md` only after user-visible behavior exists in a build
- `DATA.md`

At the end, replace `DATA.md` with a concise handoff containing:

1. Outcome and current stage.
2. Exact architecture implemented.
3. USB mode-cycle semantics.
4. Files changed.
5. Tests and full build results.
6. Hardware-validation status.
7. Known limitations/evidence gaps.
8. Exact hardware test requested from the owner.
9. Single highest-value next action.

Nothing should be committed. Preserve the dirty worktree and distinguish pre-existing changes from changes made in this pass.

## Priority order

1. Reassess current code/docs and preserve the worktree.
2. Correct the known stale documentation contradictions.
3. Implement the centralized runtime personality enum/request/transition path.
4. Implement the dedicated NSO GameCube Stage B descriptor module.
5. Generalize TinyUSB callback dispatch safely.
6. Implement the volatile five-second-hold mode cycle, skipping unavailable Joy-Con 2.
7. Preserve double-tap pairing, triple-tap wipe, Config recovery, and `NS2_PRO=OFF` behavior.
8. Add focused offline descriptor/mode-cycle validation.
9. Run the full build matrix.
10. Update documentation and `DATA.md`.
11. Hand off the exact UF2 and minimal Stage B hardware-validation procedure.

Lead with evidence and implementation. Do not spend another pass repeating Stage A research that has already been completed, and do not start broad Stage C work before the Stage B personality switch has been tested on hardware.
