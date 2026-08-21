# PicoSwitch2 Status

Current-state snapshot: what is true about PicoSwitch2 right now.

Accepted future work belongs in [`PLAN.md`](PLAN.md). Evidence, protocol detail, and experiment
records belong under [`docs/`](docs/README.md). User-visible release history belongs in
[`CHANGELOG.md`](CHANGELOG.md). Narrative history through 2026-07-15 is archived in
[`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).

- **Last software verification:** 2026-08-21 — Android/Bluetooth reliability pass. Both board
  builds, 78/78 compiled host tests, the 25-test in-band management suite, four Bluetooth/trace
  Python suites, descriptor parity, 494 Android JVM tests (app debug 167, app release 167,
  bridge-core 115, management-core 45), `lintDebug` + `lintRelease`, both APKs, and both
  install-reset markers.
- **Last hardware validation:** 2026-08-21 — fresh LE management pairing, repeated Refresh,
  personality switching with post-transition management recovery, DualSense Edge + BLE management
  coexistence, Controller Link ↔ physical-controller source switching, and a ≥75-minute mixed
  soak with continuous controller audio and no drops. Record:
  [`docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md`](docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md).
- **Current release:** v2.0.0, published 2026-08-15 from commit `a1491b2`.
- **Development branch:** `ns2-testing`; v2.0.0 is the last tag on it.
- **Bridge contract:** 3 (`ANDROID_BRIDGE_CONTRACT_VERSION` / `BridgeContract.VERSION`) — unchanged.
- **Settings schema:** 11 (was 10; adds KB/M configuration, migrates v10 in place).

## Release baseline

v2.0.0 is a major-generation release, not a point update on v1.5.0. It establishes the current
product shape: native Switch 2 controller personalities, Bluetooth physical-controller input, a
native Android companion with a host-controller bridge, a platform-neutral bridge core, translated
motion and rumble, battery passthrough, C/GameChat, bonded/encrypted in-band Bluetooth management,
Virtual Amiibo, and runtime firmware/application contract validation. The embedded USB web disk is
gone: the portal is served locally and reaches the adapter through the two supported management
transports — USB CDC/Web Serial in the Config USB personality, and bonded/encrypted BLE GATT in
Config or a normal controller personality.

Board profiles are deliberately different and should not be unified without a task and hardware
evidence:

- **Pico 2 W** — production build at 300 MHz with the hardware-confirmed floating-point/SRAM audio
  path.
- **Pico W** — validated non-audio profile.

Release notes and the release validation record: [`CHANGELOG.md`](CHANGELOG.md) §2.0.0.

## Current architecture

```
physical controller / Android handheld
      |  vendored joypad-os bthid driver (src/bt_hid/bt/bthid/devices/...)
input_event_t                       shared interchange units
      |  src/bt_hid/ns2_seam.c      the one bridge into this project's state
switch_pro_input_t                  cross-core seam (src/report.c seqlock)
      |  src/switch_pro2/           console-facing personality, USB on core 0
Switch 2 report
```

Core 1 runs BTstack plus the vendored joypad-os HID layer; core 0 owns USB and the command parser.
Controller-specific knowledge lives in the driver and in `ns2_motion_seam.c`; console-protocol
knowledge lives in `src/switch_pro2/` and `src/bt_hid/motion/`.

The companion mirrors that boundary: `:bridge-core` is plain Kotlin/JVM with no Android SDK on its
compile classpath (normalized controller state, motion convention, capabilities, HID descriptor and
codecs, `BridgeSession`), and `:app` holds the Android backends. The module boundary is the
architecture guard — a platform leak is a build failure.

Details: [`docs/architecture/overview.md`](docs/architecture/overview.md),
[`docs/bridge/PROTOCOL.md`](docs/bridge/PROTOCOL.md),
[`docs/bridge/PLATFORM_BACKEND.md`](docs/bridge/PLATFORM_BACKEND.md).

## USB personalities

Every boot starts in Pro Controller 2 mode. With a controller HID-ready, a single BOOTSEL tap
advances a volatile controller-only cycle:

1. Switch 2 Pro Controller 2 (`057E:2069`)
2. NSO GameCube Controller (`057E:2073`)
3. Joy-Con 2 Left (`057E:2067`, experimental)
4. Joy-Con 2 Right (`057E:2066`, experimental)

A two-second hold enters CDC configuration mode (`CAFE:4012`) from any controller personality and
returns directly to Pro2. Config is never part of the single-tap cycle, and the selection is not
persisted across power cycles. Host-visible identity changes (colors) require the management
`reenumerate` command; they are not picked up without a re-enumeration.

## Input sources

The console-facing stream has exactly one active logical owner. The firmware keeps a bounded
registry of HID-ready sources keyed by stable Bluetooth identity plus a monotonic connection
generation, never by a reusable BTstack connection index. A logical source is normally one peer;
Keyboard + Mouse is the one case where it is two, joined by an opaque composite handle rather than
by loosening the connection limit (see [Keyboard and mouse input](#keyboard-and-mouse-input)).

- Automatic selection ranks by source class — DIRECT (a controller paired to the adapter) over
  BRIDGE (the companion) over UNKNOWN — and applies only while the user has made no explicit
  choice. An explicit selection is final in both directions.
- Taking the console from a live owner neutralizes the complete slot (sticks, buttons, motion,
  mouse, identity, raw debug, rumble, LEDs) and then requires one fresh complete report from the
  new owner, so a held button cannot survive the handover. Claiming an unowned console does not
  wait for a fresh report — there is no previous stream to flush.
- **Disconnect of the active source always drops ownership**, neutralizes slot 0, and clears
  retained native motion. What happens next depends on how it was owned: if the source
  was the user's explicit choice — including an explicit selection still in flight at the report
  boundary — the console is deliberately left unowned and no other source takes over. If ownership
  was automatic, the policy re-runs and the best remaining source by class takes over, which is what
  returns the console to the companion when a directly paired controller disconnects or runs flat.
- Stale disconnects cannot remove a source after connection-index reuse: the source key carries the
  stable address and connection generation, not just the reusable index.
- Surfaces: UART `input sources` / `input active <id|none>`, the same bounded query over
  bonded management, and the companion's **Active controller** selector.

## Keyboard and mouse input — Complete, hardware validated

Validated on hardware with an ASUS ROG FALCHION RX keyboard and ROG KERIS II ACE mouse.

2026-08-16 — both peers connected simultaneously as one logical source with distinct connections;
either role powered off leaves the survivor working; either role returns and rejoins automatically
**without re-pairing and without touching the surviving peer**.

2026-08-17 — bounded partial-source discovery, end to end: zero peers restores normal discovery
(`ble_conns=0`, `scan_active=true`); the first role joining keeps discovery active (`ble_conns=1`,
`scan_active=true`); ~30 s later the completion window has expired and discovery retires
(`scan_active=false`) with the keyboard still connected and working; a BOOTSEL double-tap then
re-arms discovery (`scan_active=true`) even though the background window is long gone; and with that
pairing window open, powering the mouse on joined it as the second role — `keyboard=true mouse=true`,
`keyboardConn=4 mouseConn=5`, `ble_conns=2`, mouse input confirmed — after which discovery retired
because the source was complete. No reboot, no bond clearing, no disconnecting the keyboard, no
manual mode change.

The adapter **infers** what to be from what is actually admitted: pair a keyboard and it becomes a
keyboard; add a mouse and the two become one controller. The persisted setting is an *override*
(default `auto`), not a mode you must select before pairing. A disconnected mouse never turns the
Keyboard + Mouse profile back into the Keyboard profile when the override pinned it.

- **Recognized controllers are unchanged**, including the existing Bluetooth-mouse path that feeds Joy-Con 2
  mouse mode. Keyboards are simply not registered as sources there.
- **Capability is not role ownership.** An ASUS ROG KERIS II gaming mouse reports `kbcap` *and*
  `mousecap` — macro buttons put a keyboard collection in its descriptor — and bthid binds it to the
  keyboard driver. It is still a mouse. A peer's *primary* kind decides which role it takes;
  capabilities only say what reports it can emit. For an unresolved peer, pointer capability wins,
  and COMBO is never inferred from "has both" — only a Class-of-Device combo declaration grants it.
  A single-primary peer whose role is taken is a duplicate and never falls back to the other role.
- **Role assignment is symmetric for genuine combos.** A declared combo peer takes both roles when
  both are free and whichever one is free otherwise. An earlier rule rejected such a peer outright
  when the keyboard role was taken, which is what made keyboard + mouse impossible to establish
  (1547 refusals with both peers connected).
- **Keyboard + Mouse is one logical source over two peers.** Role binding happens above the source
  arbiter; the arbiter itself gained a `group_id` so members of one composite share ownership, and
  losing one member hands the token to the survivor instead of surrendering the console. Standalone
  sources (`group_id == 0`) behave exactly as before. A second keyboard, a second mouse, or an
  unrelated gamepad is rejected and counted.
- **Classification is structural** — Class of Device on Classic, report descriptor on BLE, keyboard
  tested before mouse so a combo peer can fill both roles from one connection. Names are never used.
- **Mapping lives outside the Bluetooth parsers** as sparse user overrides on immutable canonical
  defaults, one profile per mode, independently resettable. Output is recomputed from the held
  source set every publish, which is what makes duplicate bindings safe and makes a stuck
  destination impossible. Opposing digital directions neutralize.
- **Mouse movement** feeds the existing Joy-Con 2 native pointer where the personality has one, and
  otherwise translates to the right stick from a **velocity estimate** — deflection tracks how fast
  the mouse is currently moving, so continuous movement holds a continuous level — with an
  inactivity deadline driven by the existing 3 ms core-1 tick, so it can never latch off-centre. The
  original constant-friction accumulator is disproven and documented as such in
  [`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md): it imposed a
  hidden 8.53 counts/ms threshold and emitted pulses below it. Only the translator is configurable;
  the validated native wire path is not. **Hardware validated 2026-08-18 in Splatoon**: continuous
  mouse motion holds a continuous stick level and the pulse defect is gone. No mouse-to-stick blocker
  remains.
- **Mouse amplitude at the low end** is compensated by an optional radial `antideadzone` (0..50 %,
  default **0** = the validated linear response). A linear velocity map loses the slowest N % of the
  speed range to a game's N % stick deadzone *at every sensitivity*, so sensitivity alone cannot fix
  it. Applied to a resolved output copy only — the velocity estimator never sees it. Radial rather
  than per-axis because independent floors rotate small vectors; magnitude is carried in sixteenths
  of a stick unit, without which a tiny diagonal overshot its configured floor by up to 41 %.
  Hardware validated: too little compensation reproduced the invisible-slow-sweep failure, and
  raising it restored slow camera movement.
- **The two mouse knobs have separate jobs.** `antideadzone` recovers the destination's dead low end;
  `sensitivity` sets the velocity-to-stick gain and therefore how soon full-stick saturation arrives
  (full stick at 8.53 / 5.69 / 4.27 counts/ms for 512 / 768 / 1024). Once the translated stick reads
  full deflection the destination owns the maximum turn rate, which is why realistic fast flicks
  cannot be made to "snap" harder from this side.
- **Live tuning without a management client**: `kbm mouse [field] [value]` and `save` on the UART
  diagnostic channel, sharing one parser/formatter with the management surface. Settings live-apply
  in RAM and persist only on an explicit `save`. Splatoon-tested example tuning (sensitivity 768,
  anti-deadzone 25, in-game right-stick +5) is recorded in the KB/M document as **game-specific
  evidence, not a default** — firmware defaults remain sensitivity 512 and anti-deadzone 0.
- **Pairing never disconnects a KB/M role.** Historical "opening pairing replaces the connected
  device" semantics apply to a standalone controller only. Replacing a KB/M role means powering that
  device off; the BOOTSEL gesture cannot say which role is meant.
- **A freed role is never absorbed by a surviving peer.** Once a peer holds a role, that is its role
  for the connection generation — only a positively-declared COMBO may hold both. Without that
  invariant a KERIS II mouse took the keyboard role the moment the real keyboard powered off
  (`keyboardConn == mouseConn`), the source looked complete, and the keyboard could never return.
  Supporting fixes: a peer's classification record is keyed by generation and never wildcard-wiped
  by connection index (indexes are reused); capabilities accumulate and are never narrowed; and a
  BLE peer on the keyboard driver waits for its descriptor classification instead of latching the
  driver binding's partial keyboard-only view.
- **Reconnect targets a bonded identity that is actually absent.** Selection runs over BTstack's LE
  device DB through `ns2_ble_reconnect_select()` (pure, host-tested) at all three reconnect sites —
  the disconnect handler, the connection-failure retry cascade, and the periodic reconnect — and
  **never returns an identity that is already connected**. Only an identity with stored metadata is
  direct-connected; any other absent peer is reached by discovery, which carries its name and profile.
  Legacy single-controller reconnect is unchanged. A bonded management/companion identity lives in the
  same LE DB but can never be dialled: a direct connect requires the stored-target flag, which only a
  central-role HID connection can set.
- **Discovery lifetime follows logical-source completeness.** A complete source — one standalone
  controller, or both KB/M roles — retires discovery. A partial KB/M source keeps it available so the
  missing role can rejoin, which is what makes bonded rejoin work without re-pairing. The rule is the
  pure, host-tested `ns2_kbm_logical_source_complete()`, deliberately independent of the AUTO-derived
  effective mode: AUTO describes the roles currently present, so keying completeness off it would
  report "complete" the moment one peer arrived. `ns2_bt_host.c` owns the policy;
  `btstack_host_scan_for_additional_peer()` executes the mechanics.
- **Discovery ownership is re-asserted every tick, by two independent reasons.** Every BLE HID peer
  reaching ready calls `btstack_host_stop_scan()` (three sites), and the idle safety-net cannot
  restore it while any link is up, so whatever wants discovery must re-assert it continuously rather
  than rely on nobody stopping it. The matrix is the pure, host-tested `ns2_kbm_discovery_policy()`:
  an explicit pairing window is authoritative and keeps discovery armed until the source is complete
  (then leaves the scan alone, so controller replacement still works); outside a pairing window the
  bounded completion window decides. The two used to be mutually exclusive — the completion window
  was evaluated only inside `if (pairing_until_ms == 0)` — so the first peer to finish connecting
  *inside* an explicit pairing window stopped the scan and nothing re-armed it for the rest of that
  window. Hardware showed exactly that: keyboard connected, source still partial, `hid_state=0`,
  `scan_active=false`, `scan starts == stops`.
- **Partial KB/M discovery is bounded by a completion window.** A partial source holds discovery open
  for 10 s (`ns2_kbm_completion_update()`, pure and host-tested) so the missing role can join, then
  settles as intentional keyboard-only or mouse-only. The window is keyed to logical-source
  transitions — entering partial from empty, from complete after a role loss, or from the other
  partial state — so no amount of keyboard or mouse traffic extends it. Expiry changes discovery
  policy only; input, effective mode, source ownership, and the surviving link are untouched, and the
  complement may still join later when discovery is re-opened. It is not a persisted choice — nothing
  records a keyboard-only or mouse-only preference.
- **An open pairing window outranks a speculative direct reconnect.** Exposed by the completion
  window: settling calls `btstack_host_stop_scan()`, which clears `hid_state.scan_start_time`; the
  next `btstack_host_start_scan()` (from explicit pairing) therefore takes the "first scan with a
  bonded device" fast path and backdates that timestamp so the periodic reconnect becomes eligible
  ~3 s in. It then DIRECT-targeted the absent peer, and `btstack_host_connect_ble()` stops the scan
  for the whole attempt (10 s timeout, then retries) while nothing re-arms discovery — the app-layer
  re-arm is gated on `pairing_until_ms == 0`. The user's pairing window was consumed with the radio
  not scanning. `ns2_ble_reconnect_select()` now takes `pairing_window_open` and never returns DIRECT
  while it is set: discovery is strictly better there, because the advertising path admits bonded and
  unbonded peers alike and resolves identity from the advertisement. Background direct reconnect
  outside a pairing window is unchanged, so peers that stop advertising after bonding still work.
- **Stale-bond deletion is scoped to the peer that dropped**, not to the stored target — with two
  bonded peers it could otherwise delete the bond of the peer still connected and working.
- **Surfaces:** `kbm` on management and UART (mode, status, paged effective map, bind, reset, mouse
  settings), plus an input-source card in the web portal. The wire format is what UX_PASS's
  remapping editor is meant to build on.
- **Resource impact:** two HID peers fit inside the existing BTstack capacities on both boards; no
  limit was raised. Measured build delta against a clean build of `505a0c8`: Pico W +20 472 B flash
  / +4 012 B RAM, Pico 2 W +18 600 B / +4 020 B.

**Hardware validation is pending** — this is implementation plus host tests only. Reference:
[`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md).

## Android companion

No-root Android app using the public API-28+ `BluetoothHidDevice` profile; PicoSwitch2 remains the
console-facing protocol owner. Reference hardware is an AYN Thor (Android 13 / API 33).

- **Wire contract:** single source of truth `tools/fixtures/android_controller_hid.h`; 161-byte
  descriptor, 26-byte input report (15 buttons within the original two bytes), 5-byte output
  report. The firmware identifies the bridge by an exact descriptor match, never by VID/PID.
- **Compatibility:** both ends declare the bridge contract and compare it on connect; the firmware
  also reports its git build identity. A skew is reported plainly instead of silently disabling
  battery, motion and rumble. Firmware that answers without a contract is reported unverified,
  never compatible. Descriptor parity and a per-contract SHA-256 descriptor digest are build gates.
- **Console buttons:** Home, Capture and C/GameChat are available as on-screen controls, routed
  through the ordinary button path rather than a side channel. Home also retains its normal
  `KEYCODE_BUTTON_MODE` physical mapping; Capture and C/GameChat have no default physical-key
  mapping, which matches both audited handhelds. `KEYCODE_BUTTON_C` and `KEYCODE_BUTTON_Z` are
  deliberately unmapped and reserved for a future mapping system.
- **Hardware state:** the v2.0.0 sanity pass on an AYN Thor confirmed buttons, sticks, triggers,
  D-pad, C/GameChat, battery, motion and rumble with the adapter reporting `v2-bridge`
  identification; the bridge is also confirmed on an Odin 2.
- **Management surface:** the app is organized around five destinations — Adapter, Keyboard,
  Amiibo, Gamepad, Settings — with Diagnostics and Amiibo settings as pushed screens rather than
  permanent navigation. Keyboard & Mouse is now a first-class product area covering the complete
  `kbm` command surface: device/role status with names resolved from the source registry, input
  mode, both mapping profiles with a per-input editor, and mouse tuning. KB/M changes apply to
  adapter RAM immediately and are persisted by an explicit Save; the app models that rather than
  implying storage. Implementation complete; hardware validation of the mutation paths pending.
- **Relationship lifecycle:** source/JVM-tested 2026-08-20. One Android-side generation coordinator
  owns CDM association, Bluetooth bond wait, foreground/manual reconnect, and identity-verified GATT
  progression. API-33 duplicate association completion is idempotent; missing bonds become Repair
  pairing; ordinary Disconnect retains the relationship and touches management only. GATT teardown
  is callback-or-timeout bounded and close-once; 133/timeout/congestion receive one clean retry and
  one saved-address-pinned scan fallback. The controller-drop/solid-LED report remains **Unknown**
  pending the focused UART + ADB physical matrix; firmware/controller architecture was not changed.
- **Identity color UX:** source/JVM-tested 2026-08-20. One commit now performs mutation, readback,
  persistence completion when identified, and automatic same-personality re-enumeration. Only a
  partial USB-refresh failure leaves Retry. Physical Switch 2 validation remains pending.
- **Relationship terminology:** Settings separately reports Saved adapter, Android companion
  association, Android Bluetooth pairing, and Adapter Bluetooth LE bonds. The adapter list is not a
  phone directory and is not name-deduplicated because firmware exposes no proof that two entries
  represent one physical phone.
- **Known limitations:** input is delivered through Activity dispatch, so the companion must be the
  foreground window while playing (backgrounding releases held input; the connected-device
  foreground service keeps the link and rumble path alive). Android permits one HID Device
  application system-wide.
- **Blocked on firmware, not on the client:** controller remapping and adapter renaming have no
  management command at all. `NS2_BASE_BUTTON_MAP` is a compile-time table with no runtime override
  storage, and the advertised name is a compile-time constant locked to 11 bytes by a
  `_Static_assert` and written into fixed-length LE scan-response, Classic EIR and ATT Device Name
  data. Both need firmware work before any client can offer them.

Briefs: [`docs/agents/ANDROID.md`](docs/agents/ANDROID.md),
[`docs/bluetooth/android-controller-bridge.md`](docs/bluetooth/android-controller-bridge.md),
[`docs/android-companion.md`](docs/android-companion.md).

## Bluetooth, pairing, and wake

- Exactly one logical input source owns the console stream at a time (see
  [Input sources](#input-sources)). For ordinary direct physical-controller operation, background
  BLE scan and Classic inquiry idle once the selected logical source is complete — the pairing
  window closes, the LED goes solid, and the radio is freed. "Complete" is one HID-ready controller
  in Controller and Keyboard modes, exactly as before; in Keyboard + Mouse mode it is both roles,
  so pairing a keyboard does not close the window the mouse still needs. Classic discovery admits
  keyboard/pointing Class-of-Device peripherals only while a KB/M mode is still missing that role.
  The host stays connectable outside the pairing window but becomes discoverable only inside it,
  so a bonded Classic controller still reconnects by paging in and a bonded BLE controller
  reconnects once discovery resumes at zero connections. An Android companion connection is
  app-initiated, so it does not depend on the adapter's own discovery being active.
- Switch 2 controllers use a custom ATT pairing handshake, so wipe policy cannot depend only on
  BTstack's LE bond database. Successful pairing persists the normalized LTK in both the reconnect
  record and BTstack's LE database; HOME reconnect must run through `sm_request_pairing()` so
  BTstack restores bonded security state, after which the dongle restores ACK/input CCCs, reasserts
  P1, and reruns the native-motion feature sequence.
- A persistent global pairing lock is installed before triple-tap disconnect/erase; only an explicit
  double-tap pairing window reopens admission. Fresh Classic keys, standard LE Just Works, and the
  Switch 2 custom path now carry per-attempt admission state; erased or stale trust cannot silently
  recreate itself while the window is closed. Wipe traverses sparse LE slots by capacity, uses
  public GAP deletion, clears all project-owned reconnect/key material, and includes the shared
  management bond. A new UF2 erases the full six-sector reserved persistence region and recreates
  the lock before discovery starts. The install-reset boot fact is consumed exactly once, so an HCI
  restart cannot re-lock pairing after the user explicitly reopens it.
- Classic link-key notifications are held as in-RAM candidates and committed only after matching
  successful authentication. Existing trust survives generic authentication failure; only the
  missing-key status removes that peer's stale key. LE RPAs can reach SM only as bounded reconnect
  candidates and cannot grant fresh trust. Classic global SSP auto-accept remains disabled;
  confirmation is granted only to the matching per-attempt fresh latch, so window expiry blocks new
  candidates without revoking an admitted in-flight attempt. An HCI state loss retires live input
  generations, cancels any registered wake timer, and clears transient radio state while preserving
  durable bonds.
- Owner-LED policy now uses HID/protocol-ready counts rather than raw radio slots and uses elapsed
  wall time for every cadence. Idle is a 90 ms pulse every 10 seconds; solid means controller-ready
  unless a higher-priority acknowledgement/configuration state owns the LED. `btstate` exposes raw
  versus ready counts, the selected LED reason/timing and HCI state-loss count without key material.
  `owner_led.last_transition_ms` is the last actual on/off edge, not the reason start time.
- The first physical Xbox Elite Series 2 test showed that triple tap stopped input but left the
  controller presenting as connected. That confirmed failure refutes project-slot disconnect as a
  complete wipe boundary. `c6d53e7` moved teardown to BTstack's complete HCI registry; its strict
  Elite 2 retest passed, forcing the controller disconnected and preventing another session until
  pairing was explicitly reopened. Other controller families and flash-path cases remain separate
  validation gaps.
- BLE HID binds from the best identity available and enables notifications before querying DIS; a
  later DIS VID/PID is handed back for idempotent re-evaluation, so contradictory name matches
  cannot pin the wrong parser while input streams.
- BOOTSEL sampling, gesture recognition, and `bthid_task()` are serviced at incoming HID report
  boundaries, with timers as the quiet/disconnected fallback.
- Console wake from sleep uses the learned wake identity and is hardware-confirmed.
- **Android fresh pairing bonds on the LE transport explicitly — Complete, hardware validated
  (2026-08-21).** The adapter is dual-mode on one public BD_ADDR, so once a phone has observed its
  Classic identity Android caches `DEVICE_TYPE_DUAL` and keeps that across *Forget*.
  `createBond()` is `createBond(TRANSPORT_AUTO)`, which then prefers BR/EDR and runs SSP against the
  controller admission gate; that gate correctly refuses an unbonded Classic ACL and Android reports
  it as "Couldn't pair because of incorrect PIN or passkey". The companion now always bonds on
  TRANSPORT_LE and never falls back to TRANSPORT_AUTO. A Classic link key could not satisfy
  `mgmt_session_authorized()` anyway, so BR/EDR was never a slower-but-valid path here.
- **Management fresh-bond admission is per-attempt.** `config_ble.fresh_bond_admitted` latches
  `mgmt_accept_bonding()` when the management connection is *accepted*, matching what controller BLE
  candidates already do via `conn->fresh_pairing_admitted`. SM confirmation sits after Android's own
  human-paced pairing dialog, so re-reading the live 30 s window there could expire authorization the
  user had already given. Who may bond is unchanged: `mgmt off` still revokes a latched attempt, an
  attempt never admitted cannot become admitted, and the latch is cleared on disconnect and on the
  HCI-loss transient reset.
- `admission.reject_window` is a refusal odometer, not a fault counter. Measured on hardware: ≈1/min
  while the adapter has no controller and is running BLE scan + Classic inquiry, and **0 in 3
  minutes** once a controller is connected and discovery is idle. All seven increment sites refuse an
  unbonded peer trying to form trust outside the pairing window. It records no transport or peer;
  that is a known diagnostics gap, not a defect.

See [`docs/bluetooth/README.md`](docs/bluetooth/README.md) and
[`docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md`](docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md).

## Management

One bonded/encrypted BLE management transport serves both the local web portal and the Android
companion, in Config **or** a normal controller personality, so the adapter can be managed while a
controller drives the console and without a CDC re-enumeration that would drop the console.

- Gated by the RAM-only runtime flag `g_mgmt_enabled`. Production builds boot on; `mgmt off` lasts
  until reboot, and the disabled path is the proven zero-cost early return.
- RX and notification-subscription writes require ATT encryption with a 16-byte key; callbacks
  additionally require a durable LE bond; a new Just-Works bond is accepted only inside the existing
  double-tap pairing window. No-display Just Works cannot provide MITM authentication, so this is
  described as bonded and encrypted, never as `ATT_SECURITY_AUTHENTICATED`.
- Commands run through the existing core-0 parser behind an allowlist. Flash operations (`save`,
  `amiibo clear`, `amiibo persist`) and bond list/remove are deferred rather than busy-waited, with
  session-bound replies. General `save` now returns a monotonic request identity and `save status`
  exposes authoritative core-1 flash completion without blocking the BLE command path.
- The Android-free `:management-core` module now owns logical commands, typed replies, paging,
  mutation/readback, Amiibo transfer workflows, and BLE session serialization. Android owns only
  connection/pairing/lifecycle and presentation. The language-neutral contract and conformance
  vectors start at [`docs/management/README.md`](docs/management/README.md).
- Optional capability probing treats only explicit firmware unsupported replies as Unsupported;
  broken sessions and malformed/pagination failures propagate. KB/M capability is explicit, and
  portable wake status preserves the firmware's `lastAttemptMs` field.
- Management advertising no longer suppresses controller discovery. A 5.4-hour soak held a Classic
  controller plus a management client through ten USB re-enumerations and three controller
  disconnect/reconnect cycles, with automatic controller recovery and no management disconnects.

Remaining gates are listed under [Open validation gates](#open-validation-gates). Reference:
[`docs/bluetooth/in-band-management-plan.md`](docs/bluetooth/in-band-management-plan.md),
[`docs/architecture/config-transports.md`](docs/architecture/config-transports.md),
[`docs/management/README.md`](docs/management/README.md).

## Motion

There is exactly one Switch 2 motion encoder for translated sources,
`src/bt_hid/motion/ns2_ds5_motion.c` (misleadingly named; the rename is a PLAN.md task).
`ns2_build_report()` has two motion branches — opaque genuine Pro Controller 2 passthrough and the
translator — with no fallback and no per-source whitelist. Per-source frame differences live in one
place, `ns2_motion_seam.c`, one determinant-+1 row per source, each landing that source's face-up
gravity on carrier slot 2 at +4096. Production translated motion is the validated length-`0x1E`
carrier; the encoder integrates a source's own IMU clock when it authors one, with the unit carried
alongside the sample.

**Motion is frozen by maintainer decision (2026-08-15).** Do not change the shared motion path
without new hardware evidence.

Current state by source:

| Source | State |
|---|---|
| Genuine Pro Controller 2 | Confirmed — opaque native passthrough |
| DualSense / DualSense Edge | Confirmed — translated `0x1E` |
| Switch 1 Pro Controller | Confirmed at 98–100% parity with a native connection |
| Android companion | Confirmed direction and scale; a residual smoothness artifact was reported on the AYN Thor only and was not reproduced on an Odin 2, Odin 3, or Odin 2 Mini |
| Wii Remote | Needs re-confirmation — its 2026-07-27 pass predates the deleted encoder (see below) |
| Switch 1 Joy-Con L/R | Unverified — they share the Pro's seam row, and the halves mount the IMU mirrored |
| Other IMU families | Not supported without a verified layout, axes, scale, timestamp, and bias model |

Known deferred limitation: very slow smooth rotation below the current stillness threshold may be
partially absorbed by the bias estimator. Production motion is frozen; measurements and the
evidence-backed candidate correction are in [`docs/agents/MOTION.md`](docs/agents/MOTION.md).

Synthesized translated length-`0x28` is **deferred**. Production translated motion stays on the
validated `0x1E` carrier, genuine Pro2 native forms remain passthrough, and the generator and
genuine-base hybrid harness remain default-off research infrastructure. Do not resume it without a
concrete `0x1E` production deficiency or a materially better observation point. Evidence:
[`docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md`](docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md),
[`docs/experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md`](docs/experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md),
[`docs/experiments/ds5-motion-hybrid-harness-2026-08-01.md`](docs/experiments/ds5-motion-hybrid-harness-2026-08-01.md).

## Rumble and haptics

The firmware chain (console report `0x02` decode → slot-0 feedback with generation-based change
detection → per-family driver output) is confirmed across driver families, including native
DualSense PCM haptics on Pico 2 W and the capture-derived NSO GameCube and 8BitDo NGC Modkit
formats.

On the companion, console rumble reaches the handheld's actuator: the app binds the vibrator
belonging to the selected `InputDevice` rather than the phone's system vibrator, declares the effect
as media vibration, repeats a sustained value under a watchdog (both ends suppress unchanged
values, so nothing else refreshes it), bounds retriggers to about 25 Hz with amplitude quantization
and a hysteretic deadband, and holds a connected-device foreground service for the life of the link.
Confirmed physically in the v2.0.0 Thor pass.

Durable warning: `Settings.System.VIBRATE_ON = 0` makes AOSP discard every vibration from every app
on the system-vibrator path. Check `adb shell settings get system vibrate_on` and the app's
`haptics bound` line before changing any code. Never veto a source on `isExternal` — the Thor's
built-in controller is classified EXTERNAL. Brief: [`docs/agents/RUMBLE.md`](docs/agents/RUMBLE.md).

## Audio

- **Pico 2 W (production):** DualSense Bluetooth internal-speaker playback, physical headset
  insertion/removal, console audio during input/motion/rumble, and audio after a bonded reconnect
  are all hardware-confirmed. Genuine Pro Controller 2 headphone output through its own jack is
  confirmed, including the 240-byte Opus/CELT framing.
- **Pico W:** intentionally non-audio. The fixed-point/XIP 300 MHz experiment was rejected on
  hardware.
- **Not implemented:** DualSense microphone decode and USB return. Headset presence exists; the
  return path does not.
- **Audio sink ownership is independent of console input ownership — intentional as of 2026-08-21.**
  `ds5_audio_bridge` claims its sink in `ds5_connect()` and releases it in `ds5_disconnect()`, keyed
  on the audio-capable link's own connection index; it never consults `ns2_input_arbiter` /
  `ns2_active_input`. So Controller Link may be the active console input source while a connected
  DualSense remains the audio sink — hardware-observed, and the behaviour the product wants, because
  the Android bridge cannot transport controller audio. Pinned by
  `tools/test_bluetooth_closeout_wiring.py`; do not couple these two ownership domains.
- The only remaining audio interruption during normal use is USB personality re-enumeration, which
  is a deliberate simulated USB disconnect/reconnect. Refresh no longer causes a meaningful gap
  (hardware-observed 2026-08-21).

Reference: [`docs/switch2/audio-passthrough-research.md`](docs/switch2/audio-passthrough-research.md).

## Virtual Amiibo and NFC

Virtual Amiibo is a production subsystem and is always available; a blank adapter presents no
virtual tag and can still fall through to a real reader source. The board stores exactly one amiibo;
the two flash banks are persistence generations of that one image, not two active tags. A newly
flashed UF2 performs a one-shot erase of all six reserved persistence sectors; ordinary power
cycles retain state.

Hardware-confirmed: 540/572-byte tags and the complete 2048-byte figure-v3 (NTAG I2C Plus 2K / Kirby
Air Riders) path, including the `0x14`/`0x21` device command, the two-stage Air Riders extended
write, the sector-aware `0x1E` extended read, dynamic sector-1 capability generation across power
cycles, and envelope-derived allocation with no figure or UID whitelist. All 16 available v3 dumps
completed real-console reads and writes.

The library is **import-only**: the Switch 2 validates amiibo cryptography, so key-free generated
images are rejected and a runtime UID swap invalidates the UID-bound HMAC. Local Initialize/re-sign
uses the user's own `key_retail.bin`, stays browser- or app-local, and never enters firmware.

Open work is listed under [Open validation gates](#open-validation-gates). Authorities:
[`docs/Amiibo-v3.md`](docs/Amiibo-v3.md),
[`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md),
[`docs/switch2/amiibo-identity-and-generation.md`](docs/switch2/amiibo-identity-and-generation.md).

## Compatibility

[`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) is the authoritative
record of what has physically been validated, per personality, controller family, and subsystem. It
is not duplicated here.

Summary: all four Switch 2 output personalities enumerate and stream input on real hardware with
rumble; the Sony, Xbox, Nintendo (Switch 1 and Switch 2), Wii, 8BitDo, and Retro Fighters families
are confirmed as input sources; and the Android bridge is confirmed on two handhelds. Two rows are
knowingly behind: the Android-bridge row still describes the pre-v2.0.0 audit rather than that
release's hardware pass, and the two keyboard/mouse rows are source-tested only.

## Open validation gates

These are the genuinely open items. Bluetooth software closeout is complete and the subsystem is
frozen; remaining Bluetooth entries are targeted physical validation, not an open architecture pass.

1. **Management active-use coexistence — mostly closed 2026-08-21.** Confirmed: a console-awake
   session with a DualSense Edge connected, continuous controller audio, and a connected management
   client, held ≥75 minutes with zero Bluetooth lifecycle events, plus Controller Link ↔
   physical-controller source switching with no drops. Still uncovered: gyro under that load, wake
   from sleep during a management session, and latency measurement.
2. **Management bonded-security physical pass — partially closed 2026-08-21.** Confirmed: fresh LE
   pair inside the pairing window, and the SM decline when the connection was not admitted. Still
   uncovered: bonded reconnect after a deliberate teardown, unbonded/plaintext write rejection,
   reboot restoring management on, and the wake-burst advertiser handoff.
3. **`reenumerate` on hardware — partially closed 2026-08-21.** Confirmed: personality switching
   re-enumerates without dropping the management link, and Refresh still works after the transition;
   the only audio interruption is the deliberate USB disconnect/reconnect. Still uncovered: the
   console picking up refreshed controller colors.
4. **Web portal Amiibo Sync refresh.** The `amiiboInfoCache` invalidation fix is in `web/index.html`
   but is frontend-only and needs a browser + adapter check.
5. **Virtual Amiibo portal Sync of a retained dirty v3 generation**, with firmware acknowledgement
   only after IndexedDB succeeds.
6. **Wii Remote motion re-confirmation.** Its 2026-07-27 confirmation predates deletion of the
   refuted phase encoder, which its report-`0x09` motion reached at the time; either that pass was
   report `0x05` (unaffected) or "working" meant the console merely responded. It now routes through
   the validated encoder, so one console session closes this.
7. **Companion adapter-relationship lifecycle — partially closed 2026-08-21.** Confirmed on a debug
   APK: first-run discovery, forced-LE bond, Connected, and repeated Refresh. Still uncovered:
   returning-session reconnect after the app is closed, teardown, latency, and the same pass on a
   release APK.
8. **Native physical NFC writes and native reader gating**, including Joy-Con 2 Right, which has
   confirmed NFC hardware but an undocumented command protocol.
9. **Bluetooth Keyboard / Keyboard + Mouse on hardware.** The complete pass is implemented and
   host-validated but has never met a real keyboard, mouse, or console. The specific checklist —
   pairing and role binding on both transports, mapped input, remapping and reset through the
   management path, per-role disconnect/reconnect, partial-source safety, duplicate rejection,
   native versus translated mouse output, persistence across reboot, and Controller-mode regression
   — is in
   [`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md#hardware-validation).
10. **Remaining Bluetooth wipe/flash matrix.** The strict Xbox Elite Series 2 corrected-wipe retest
   passed. Run the still-uncovered powered-off/reboot/release-UF2 and other-family cases in
   [`docs/bluetooth/VALIDATION.md`](docs/bluetooth/VALIDATION.md). Record bond state before the remote
   returns so old trust and automatic replacement trust cannot be confused.

## Known technical debt

- **`NS2_PRO=OFF` does not build.** Verified 2026-08-16 by building `build/pico_w_switch1`: the
  `personality` and `reenumerate` command block in `src/config.c` is outside the `NS2_PRO` guard and
  references `g_usb_personality`, `g_usb_requested_personality`,
  `g_usb_personality_request_pending`, `g_usb_reenumerate_request_pending`, and the personality enum,
  which exist only under `NS2_PRO`. Repair is a PLAN.md task. Note the v2.0.0 CHANGELOG validation
  entry claiming legacy Switch 1 builds succeed is wrong.
- **`ns2_ds5_motion.*` is misnamed.** It is the shared translated encoder. Rename is a mechanical
  PLAN.md task, deliberately not bundled with behavior changes.
- **Feedback is not fully source-aware.** `find_player_index()` returns −1 for an inactive source,
  but a few legacy vendor initialization paths still fall back to slot 0, so the arbiter does not
  claim complete rumble/LED isolation between sources.
- **Compatibility matrix drift.** Its Android-bridge row still describes the pre-v2.0.0 ADB audit
  rather than that release's hardware pass, so it understates what is confirmed.

## Negative knowledge — settled, do not rediscover

- **Chart/state transitions in the motion carrier cost nothing.** Measured: the worst orientation
  step across a chart change equals the worst step anywhere else, to four decimals, across all four
  charts. Do not add hysteresis to the chart selector.
- **The per-axis "angular phase" encoder was refuted and deleted.** Those twelve bytes are one
  packed quaternion, so an int32 angle straddles field and state boundaries. It could never have
  produced correct motion. Do not add a second "generic" encoder.
  [`docs/experiments/refuted-hypotheses.md`](docs/experiments/refuted-hypotheses.md)
- **The v2 bridge regression was firmware/APK descriptor-contract skew, not the bridge-core split.**
  Buttons and sticks kept working while battery, motion and rumble vanished together because the
  exact 161-byte identification failed and the firmware fell back to the v1 generic profile. Source
  parity tests cannot see what is flashed; read `bridge` over UART.
- **`NS2_MOTION30_ACCEL_Q16_PER_COUNT = 68963` is correct and universal.** Genuine Pro2 captures put
  the wire scale at ~4310 counts/g, not 4096; the older "5.2% high, open" note used the wrong
  reference.
- **Synthesized translated `0x28` was rejected on hardware** even with a fully coherent recipe
  (correct gyro binary point, shared PDU clock, tick-weighted gyro, matched acceleration gain). The
  unresolved boundary is controller-private FIFO/filter state. Deferred deliberately.
- **A determinant of +1 is not sufficient for a seam row.** A 90° error is still a proper rotation;
  that is how a wrong row shipped twice. Rows are now pinned by a gravity-anchor test as well.
- **The Switch 2 validates amiibo cryptography.** Key-free generated images and random-UID modes are
  rejected on the console; the portal identifying them proves nothing.
- **The quirk table, not a descriptor heuristic, decides whether a peer is a supported controller.**
  `gamepad_quirks_identify()` is name-driven as well as VID/PID-driven precisely because BLE PnP
  often fails to resolve VID/PID: an Xbox pad reporting `vid=0 pid=0` still resolves to `QUIRK_XBOX`
  by name at `gamepad_init()`, which is what keeps the Elite's "Xbox + 20-byte report" paddle
  fallback reachable. Keyboard/mouse descriptor classification therefore applies to **unresolved**
  generic peers only. Ignoring that cost an Xbox Elite regression during the KB/M pass — the pad was
  reclassified off the generic driver before its descriptor was parsed, so it owned the console and
  published nothing, and `vid/pid 0/0` was a symptom rather than the cause. Rule, layering, and
  fixture-backed tests: [`docs/bluetooth/keyboard-mouse-input.md`](docs/bluetooth/keyboard-mouse-input.md).

## Research tooling

`tools/PicoSwitch2Lab.psm1` provides one manifest/provenance contract shared by the motion, audio,
NFC, and firmware-update runners: Git and build identity, safe UART discovery, hashed artifacts, and
fail-closed loss metadata. `capture_to_fixture.py` turns zero-loss captures into deterministic
JSON/C fixtures, `ns2_command_atlas.py` aggregates console-side `trace` and controller-side `blecap`
request/response shapes with full provenance, and the amiibo corpus/semantic tools answer structural
NFC questions offline before any hardware action. Firmware-side diagnostics cover the same
boundaries: `bridge`, `rumble`, `imu`, `input sources`, `btstate`, `btlife`, `trace`, `blecap`,
`audiostat`, and `amiibo v3diag`.

The major capability this buys is that most protocol questions can be answered from retained
evidence rather than by another flash-and-observe cycle. Workflow rules:
[`docs/re-methodology/controller-protocol-lab.md`](docs/re-methodology/controller-protocol-lab.md),
[`docs/re-methodology/nfc-investigation-workflow.md`](docs/re-methodology/nfc-investigation-workflow.md).

Known gap: console-side trace coverage is broad, but controller-side command coverage is thin and
concentrated on initialization. The corpus counts and the ranked capture gaps are maintained in
[`docs/switch2/controller-command-atlas.md`](docs/switch2/controller-command-atlas.md).

## Automated validation

Standard families, with commands and the current inventory in [`AGENTS.md`](AGENTS.md):

- compiled C host tests (`build/host-tests`), including protocol codecs, report encoders, motion PDU
  and seam invariants, BOOTSEL policy, wake identity, battery, audio packet/resampler, virtual-tag
  store and v3 runtime replay, the keyboard/mouse mapping model, keyboard HID report decoding, and
  settings-schema migration, plus the management/bridge suites via `tools/run_mgmt_tests.ps1`
- Python suites for trace decoding, NFC semantics, the amiibo corpus, and motion analysis
- JVM tests for `:bridge-core` and the Android backends, plus the architecture guard
- contract guards: Android descriptor parity across C and Kotlin, the per-contract descriptor
  digest, and the bridge contract version pin
- board builds for `pico_w` and `pico2_w` plus install-reset marker verification

Never bypass a parity, digest, or contract guard to make a build pass. Build success is not hardware
validation; state the level performed.

## Documentation map

- [`docs/README.md`](docs/README.md) — documentation index and authority rules
- [`docs/status/compatibility-matrix.md`](docs/status/compatibility-matrix.md) — validated behavior
- [`docs/architecture/overview.md`](docs/architecture/overview.md) — runtime architecture
- [`docs/agents/`](docs/agents/) — short specialist briefs (common, motion, rumble, Android)
- [`docs/bridge/`](docs/bridge/) — bridge contract and platform backend guide
- [`docs/bluetooth/`](docs/bluetooth/) — host, identity, pairing, and controller profiles
- [`docs/switch2/`](docs/switch2/), [`docs/switch2-gc/`](docs/switch2-gc/),
  [`docs/switch2-joycon2/`](docs/switch2-joycon2/) — console-facing protocol
- [`docs/re-methodology/`](docs/re-methodology/) — evidence standards and laboratory workflows
- [`docs/experiments/`](docs/experiments/) — dated experiment records and refuted hypotheses

## Immediate status

No known release-blocking regression. v2.0.0 remains the released baseline; the branch now carries
the Bluetooth Keyboard / Keyboard + Mouse input pass and the 2026-08-21 Android/Bluetooth
reliability pass on top of it.

The reliability pass is physically accepted. Confirmed on hardware: fresh LE management pairing,
repeated Refresh, personality switching with post-transition management recovery, DualSense Edge +
BLE management coexistence, Controller Link ↔ physical-controller source switching, and a ≥75-minute
mixed soak with continuous controller audio and no controller, management, or bond loss. The bounded
HCI/CYW43 OFF/ON recovery it added has still never fired on hardware — its logic is host-tested, but
the recovery path itself remains unvalidated in the field.

The open items above are hardware gates to close opportunistically when the relevant hardware is in
front of the maintainer. The newest one (gate 9) is the only one blocking a claim about a shipped
feature: KB/M is implementation-complete and host-validated, but nothing about it is
hardware-confirmed. The next accepted engineering work is the current development priority in
[`PLAN.md`](PLAN.md).
