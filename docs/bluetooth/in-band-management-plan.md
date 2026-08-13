# In-Band Management — Implementation Plan + Status

**Status:** 🟡 HOST/BUILD COMPLETE, HARDWARE GATES OPEN (production-default-on 2026-08-13).
Supersedes the config-mode-only model
in [config-transports.md](../architecture/config-transports.md) as the transport is now gated by
`g_mgmt_enabled`, not `g_usb_config_mode`.

## Implementation status (2026-08-13)

| Slice | Scope | State |
|---|---|---|
| — | Host-test scaffold (bridge, edge, concurrency, access spec, session, bonds, bounded-page serializer, Android HID contracts) | ✅ green, 11/11 |
| S1 | Production `src/mgmt_access.{c,h}` lifted from the spec; test links the real header | ✅ landed |
| S2 | `g_mgmt_enabled` flag (usb.h/usb.c, production default on) + `mgmt status/on/off` command + allowlist | ✅ landed |
| C1/C3 (S3) | `config_ble_authorized()` = `g_usb_config_mode \|\| g_mgmt_enabled`; re-gate can_send / RX+CCC write / advertising / accept-connection / service-task | ✅ landed |
| C2 (S4) | Extract `config_wireless_task()`; pump it unconditionally from the core0 main loop | ✅ landed |
| C6 (partial) | Wireless `save` / `amiibo clear` / `amiibo persist` no longer busy-wait core0 (deferred, ack "queued") | ✅ landed |
| S5 | Web portal Management panel (`mgmt on/off/status`); Connect Bluetooth now works in normal mode once enabled | ✅ landed |
| **C4** | **Bonded 16-byte ATT encryption + first-bond pairing-window gate; wire the `mgmt_access` predicates into ATT/SM** | 🟡 **LANDED, HW PENDING** — no-display Just Works has no MITM and is deliberately not mislabeled `AUTHENTICATED` |
| C6 (rest) | Remove the wireless `bonds` core-0 wait; defer its session-bound reply and retain bridge back-pressure | 🟡 landed; BTstack DB mutation/cadence HW check remains |
| C5 | Wake outranks a running management advertiser: stop, 100 ms quiesce, replay, restore public address, resume service | 🟡 landed; verify on HW |

**Built clean on both boards (pico_w + pico2_w), all host tests green.** Standard builds now boot
with management on. Disabling it remains a RAM-only current-boot escape hatch; the disabled path is
still the proven zero-cost early return. See the HW test procedure in §6 and the handoff notes at
the end of this section.

### Hardware test 1 (2026-08-12) — workflow works, coexistence failure found

Owner hardware pass: the **entire management + Amiibo workflow succeeded** over BLE in normal Pro2
mode (portal → adapter → Amiibo upload → Switch 2 read/write → Sync back to portal, with correct
metadata). No noticeable input latency navigating the UI. **But** after a short period, the
management link *and* the controller link both dropped and could not recover without a power cycle;
gyro/audio were never reached and stay untested.

**Root cause (controller half) — CONFIRMED from code:** `btstack_host_start_scan()` early-returns
while `config_ble.mode_active` is set, which is latched true for the whole session when management is
enabled. So once the controller drops, its reconnect scan is permanently suppressed until a power
cycle (which clears the RAM-only flag). Same class as the CDC/personality-switch resemblance the
owner noted. **The management half** (advertising also stops and stays stopped) is not yet isolated
and needs a hardware trace. Full write-up:
[`../experiments/in-band-mgmt-coexistence-failure-2026-08-12.md`](../experiments/in-band-mgmt-coexistence-failure-2026-08-12.md).

**Added this pass (diagnostics, not a fix):** UART `btstate` (live BLE/management snapshot +
scan-suppression cause counters) and `btlife read <N>` (48-entry lifecycle event ring with HCI
disconnect reasons and ordering), plus the former `-MgmtOn` (`NS2_MGMT_DEFAULT_ON`) diagnostic
build used to reproduce the failure right after a power cycle (this is now the production default).
The coexistence fix (do **not** suppress
controller discovery under in-band management) is designed in the experiment doc but deliberately
**not** applied until the trace confirms both halves — instrument-and-isolate first, per owner.

### Hardware follow-up (2026-08-13) — recovery fixed

The subsequent decoupling change removed management as a controller-scan suppression cause. A
Classic controller and management client then held for 5.4 hours across ten USB re-enumerations;
three controller disconnects all recovered automatically while management stayed connected. The
original zero-disconnect-event wedge did not recur, so the fix is strong recovery evidence but not
a direct reproduction of that unseen management-half failure. Active console use with audio, gyro,
wake, and latency observation remains open.

---

### Original proposal (below) — retained as the design reference

Investigation complete 2026-08-12.

**Goal.** Let a phone (or any Web-Bluetooth browser) manage the adapter — swap Virtual Amiibo,
change colors/config, and optionally switch output personality — **over BLE while a normal
controller drives gameplay**, instead of the current out-of-band Config mode that re-enumerates USB
to CDC and drops the console. Then **deprecate the old Config personality**. On-demand only; **zero
impact on input/audio/motion when not in active use.**

---

## 1. Headline finding: ~95% already exists, gated to Config mode

The management path is fully built and shipping today; it is only ever *pumped* while
`g_usb_config_mode` is true. Evidence (file:line):

| Component | Where | State |
|---|---|---|
| BLE GATT service (RX write / TX notify, project UUIDs) | `btstack_host.c` `setup_att_server()` ~793 | In static ATT DB always |
| Cross-core command bridge (SPSC, atomic acquire/release, session gen, MTU chunking) | `config_wireless_bridge.c` (full) | Production-quality |
| Wireless command **allowlist** | `config_wireless_bridge.c:194` `config_wireless_command_allowed()` | Enforced at `config.c:945` |
| Full command surface (info/ping/get/device/save/amiibo/body/jcl/jcr/lb) | `config.c` `handle_line()` ~943 | Works over both transports |
| Command execution on **core0** (single-threaded, same as CDC) | `config.c` `config_cdc_task()` 1024-1057 | Cross-core safe |
| Coexistence-aware advertising (waits out connecting/scan/inquiry/wake) | `btstack_host.c` `config_ble_service_task()` ~1741 | Good radio citizen |
| **Zero idle cost** | same, `return; // zero radio work in every normal controller personality` | Proven |
| **Web Bluetooth client** (Connect Bluetooth, RX/TX, write-with-response) | `web/index.html` ~3623-3633, 3431 | Already written |
| **Bonding infrastructure** (LESC + flash-persistent `le_device_db_tlv`) | `btstack_host.c` `sm_init()` 840-849, `le_device_db_tlv_configure` 974 | Configured (used for HOME wake) |
| Existing architecture doc | `docs/architecture/config-transports.md` | Documents the config-mode-only model |

**The only things tying it to Config mode:**
1. `config_ble_service_task(g_usb_config_mode)` — arming keyed to the config flag (`btstack_host.c:2119`).
2. The wireless-command pump lives *inside* `config_cdc_task()` (`config.c:1045-1053`), which the
   main loop calls **only** in the config branch (`usb.c` ~240, `if (g_usb_config_mode) { … config_cdc_task(); continue; }`).
3. `!g_usb_config_mode` early-returns in the RX-write / advertising handlers
   (`btstack_host.c:700,747,769,1683-1684,1704-1705`).
4. Characteristics are `ATT_SECURITY_NONE` (`btstack_host.c:822,824`) — safe *only because* Config
   is physically gated and USB-exclusive.

**The previous designer named the exact prerequisite** (config-transports.md:36-38): *"Do not extend
this service into normal mode without adding a separate authenticated authorization design."* That
authorization design is the core new work below; nearly everything else is re-gating.

---

## 2. Required changes (minimal, contained)

### C1 — Decouple arming from `g_usb_config_mode`
Introduce an on-demand management state, e.g. `volatile bool g_mgmt_armed` (core-shared). Replace the
gate the BLE path checks: `g_usb_config_mode` → `(g_usb_config_mode || g_mgmt_armed)` in
`config_ble_service_task`, the RX-write handler, and the advertising start/stop guards. When
`g_mgmt_armed` is false and not in config mode, behavior is byte-identical to today (the proven
zero-cost early return).

### C2 — Pump the wireless bridge in normal operation
Extract the wireless half of `config_cdc_task()` (config.c:1045-1053) into a standalone
`config_wireless_task()` and call it from the main core0 loop **unconditionally** (it self-gates on a
ready command, which only appears when armed + a client has written). CDC reading stays in
`config_cdc_task()` for the (possibly deprecated) wired path. Command execution stays on core0 — no
new concurrency.

### C3 — Connect/disconnect anytime (no per-session gesture) [owner: 2026-08-12]
The service is **always connectable while the console is awake** — a paired phone opens the app and
connects whenever, with no BOOTSEL gesture per session. Model = **pair once, then reconnect freely**
(like any Bluetooth device):
- **Steady state:** while the console is awake and no controller scan/inquiry/connect is in flight,
  the management service advertises **connectably at the existing low duty (100–150 ms)**. On connect,
  advertising stops; on disconnect, it resumes. A bonded phone reconnects at will.
- `g_mgmt_enabled` (production default on; runtime-off until reboot) replaces the config-mode gate
  in `config_ble_service_task` and the RX/advertising guards: `g_usb_config_mode` →
  `(g_usb_config_mode || g_mgmt_enabled)`. When disabled, byte-identical to today (zero-cost return).

### C4 — Bonded encrypted authorization (landed; runtime validation pending)
Without the physical Config gesture, **bonding is the access-control identity**:
- RX and TX-CCCD writes require `ATT_SECURITY_ENCRYPTED`; BTstack's generated ATT database encodes
  a 16-byte minimum key for these attributes. The dynamic callbacks independently require
  `gap_bonded(handle)` plus an active 16-byte encryption key before accepting a command or enabling
  replies.
- `IO_CAPABILITY_NO_INPUT_NO_OUTPUT` Just Works cannot provide MITM authentication. The shared SM
  requests bonding plus LE Secure Connections, but controller compatibility permits legacy fallback;
  therefore this path deliberately does not use or claim `ATT_SECURITY_AUTHENTICATED`.
- **First bond is physically gated (§7.1).** A new management Just-Works request is confirmed only
  while `g_mgmt_enabled` and the existing double-tap pairing window are both active. A stored phone
  bond reconnects outside the window. This is a one-time setup step, not a per-session re-arm.
- Keep single-client acceptance and the LE-Peripheral-role classification before any slot/SM/GATT use
  (config-transports.md:33-34).

### C5 — Wake-from-sleep is never broken (invariant) — REVISED 2026-08-12
Wake advertising **strictly outranks** management for the *advertiser*, but management **may run while
the console is asleep** (an earlier draft suppressed it entirely — too conservative, and it would kill
the phone-wake feature, G8 of the interface audit). Corrected model:
- **`wake_adv` is on-demand, not continuous** — it only runs when a wake is requested. So while the
  console is merely asleep and no wake is in progress, the single LE advertiser is **free**, and
  management may advertise and accept a phone connection (the BT core runs while suspended).
- Management **yields the advertiser to a wake burst**: the existing `if (wake_adv.active) return;`
  guard already prevents starting a management advert during a burst; extend it so a *running*
  management advert is stopped for the (brief, ~1.2 s) burst, then resumed.
- A management **connection** persists across sleep and across wake bursts (a connection is not the
  advertiser). This is what lets a phone connect while asleep and send `wake` (G8), which triggers the
  existing `ns2_wake_request()` (via a core1 request flag — wake state is core1-owned).
- **Hardware gate:** confirm the CYW43 can run a wake burst while a management connection is active
  (concurrent advertise + peripheral connection). If not, the `wake` command briefly drops the link,
  fires the burst, and the phone reconnects on wake. Validated in HW check 6a.
- "Don't break wake" therefore means *wake always gets the advertiser when it needs it*, **not**
  *management is off while asleep*.

### C6 — Flash-op timing during gameplay
`save`, `amiibo commit`, `amiibo persist` write flash, which parks core0 (brief input/audio hitch).
The deferred-save machinery already exists (`save_not_before_ms`, `save_requested`, core1 control
tick is "the only flash writer" — config-transports.md:76-78). Route wireless-initiated flash ops
through the **deferred** path (not the 2 s busy-wait in the current `save` handler, config.c:1010-1018)
so the write lands at a safe point. In practice amiibo/config changes happen in menus (stationary),
so the hitch is a non-issue, but the deferred path makes it correct regardless.

---

## 2b. Pairing & bond-management model (resolves §7a)

Reuses the existing, hardware-proven controller pairing machinery — no new gesture. The gesture map
(`bootsel_action.c`, a pure function already unit-tested) is unchanged:

| Gesture | Existing action | Extension for management |
|---|---|---|
| Double-tap | `OPEN_PAIRING` (controller pairing window; adapter discoverable/connectable) | A management phone bonds **inside this same window** — the window is the first-bond gate |
| Triple-tap | `WIPE_DEVICES` (`gap_delete_bonding` all bonds) | Also clears the management-phone bond = "unpair everything" |
| Single-tap | cycle personality | (unchanged) |
| 2 s hold | toggle Config | (becomes the kept default-off CDC fallback / first-pair path) |

**First pairing.** Double-tap → the pairing window opens. A phone connects as an LE-peripheral
management client and bonds *within the window*. `mgmt_accept_bonding()` is true **only while the
window is open** → same trust model as adding a controller. Outside the window an unbonded phone may
connect but **cannot bond**, therefore **cannot write** (`mgmt_allow_write` requires bonded). No
drive-by hijack. This is bonded encrypted authorization; it is not MITM-authenticated.

**Reconnect (connect/disconnect anytime).** After bonding, the phone reconnects whenever the console
is awake — the service advertises at low duty and encryption uses the stored bond, no window needed.

**Unpair.** Triple-tap wipes all bonds (emergency "forget everything"). For granular control the
app/portal uses new **bonded-only** commands `bonds list` / `bonds remove <index>` (backed by
`le_device_db` / `gap_delete_bonding`) so a single saved phone can be dropped without nuking
controllers. `bonds` joins the wireless allowlist; `bonds remove` is a small flash op (deferred path).

**Clients — web portal *and* native app.** Both speak the same GATT service + JSON-line protocol.
`web/index.html`'s Web Bluetooth path already works; a native app is a future client needing **no
protocol change**. Saved-pairing management (`bonds`) is the shared "Paired devices" surface.

**Personality change (future, via app).** A `personality <pro2|gc|jcl|jcr>` command would trigger the
**deferred** forced re-enumeration (§7.3) — behind its own safety investigation, not in the first cut.

The access rules above are encoded as the pure spec in `tools/test_mgmt_access.c`
(`mgmt_should_advertise` / `_accept_connection` / `_accept_bonding` / `_allow_write` /
`_should_drop_client`) and tested exhaustively; production `src/mgmt_access.{c,h}` lifts them verbatim.

## 3. Per-command safe-live classification (full audit of `handle_line`)

| Command | Effect | Wireless today | Live-safe during gameplay? |
|---|---|---|---|
| `info`, `ping` | static / noop | ✅ allow | ✅ trivial |
| `get` | read config (cfg_lock) | ✅ allow | ✅ read |
| `device` | read connected-controller summary | ✅ allow | ✅ read |
| `body`/`jcl`/`jcr`/`lb` | set colors in RAM | ✅ allow | ✅ RAM write under cfg_lock |
| `reenumerate` | apply host-visible identity changes | ✅ allow | ✅ queues existing core-0 same-personality USB reset; bonded-only |
| `amiibo status/read/select/present/eject/downloaded/cancel` | RAM/state only | ✅ allow | ✅ no flash; select swaps active tag |
| `amiibo begin/chunk` | buffer upload in RAM | ✅ allow | ✅ RAM (bounded) |
| `amiibo commit`, `amiibo persist`, `save` | **flash write** | ✅ allow | ⚠ via **deferred** path (C6) |
| `amiibo clear` | clears pending / flash? | ✅ allow | ⚠ verify flash vs RAM |
| `bonds list` / `bonds list v2 [cursor]` (new) | read saved bonds | add to allowlist | ✅ bounded complete/page (bonded-only) |
| `bonds remove <i>` (new) | delete one bond | add to allowlist | ⚠ small flash op (deferred) |
| `state`, `raw`, `audiostat`, `imu`, `imuanom`, `fwreads`, `sw2cap`, `btid` | developer/diagnostic reads | ❌ CDC/UART only | n/a (stay off wireless) |

The allowlist boundary (user config vs developer diagnostics) is already correct for in-band use.
**Only the flash-writing subset needs the deferred path.** No handler assumes exclusivity beyond
flash timing; all run on core0 under the same parser.

### 3a. Amiibo commands in non-amiibo personalities (GC / Joy-Con 2) — SAFE (verified 2026-08-12)

Concern: could an `amiibo present`/`select` sent over management while the adapter is in a **GameCube
or Joy-Con 2** personality (neither has native amiibo/NFC) break something? **No — it is inert.**
- The amiibo *serving* path (`ns2_virtual_nfc_runtime`, `ns2_nfc_mirror`) is wired **only** into
  `switch_pro2.c`. GC (`switch_gc.c`) and Joy-Con 2 (`switch_joycon2.c`) include no amiibo/NFC serving
  code at all.
- Both GC's and Joy-Con 2's NFC command (`0x01`) handler is a **pure bare-ack** (`r[1]=0x04; dl=0;`)
  that never reads `virtual_amiibo_store`. It matches the real controllers, which also just ack.
- `virtual_amiibo_store` is a personality-agnostic data structure on core0. A management command
  updates it, but in GC/Joy-Con nothing *reads* it — so there is no report corruption, no crash, and
  the console (which knows those controllers lack NFC) does not query it anyway.
- Flash-touching amiibo ops (`commit`/`persist`) are personality-independent and use the deferred
  path (§C6), so no new timing risk either.

Worst case is purely cosmetic: state set in GC/Joy-Con mode simply applies when you next switch to
Pro2. **Recommendation (app-side, not firmware):** the portal/app should gray out amiibo controls in
non-Pro2 personalities for clarity — but there is no breakage to guard against in firmware.

---

## 4. Deprecating the old Config mode

"Old Config mode" = the `USB_PERSONALITY_CDC_CONFIG` re-enumeration (drops the console) + Web Serial
+ the 2 s-hold gesture that triggers it.

**What in-band management replaces:** all *user-facing* config — settings, colors, and the entire
Amiibo workflow — with no console disconnect. The Web Bluetooth client already exists.

**Diagnostics are NOT a blocker (corrected 2026-08-12).** The developer/diagnostic surface already
runs in **every personality** over the **UART diag link** — `ns2_uart_diag_task()` is called
unconditionally in the main loop (its own comment: "remains available while USB is owned by the
console"), gated by `-DNS2_UART_DIAG`. It is a *richer* surface than the config.c diagnostic commands
(motion/PDU/audio/NFC-mirror/BT-version/traces). The owner confirmed using UART diagnostics in Pro2
mode. So removing the CDC Config personality loses **no** diagnostic capability; the config.c
`state/raw/audiostat/imu/…` handlers are a convenience duplicate that can be dropped with CDC or kept
UART/debug-only.

**Deprecation stages (owner decisions applied):**
1. Ship in-band management alongside the existing CDC Config (both work). Gain hardware confidence.
   CDC Config **stays behind a default-off build flag** (owner: "keep it gated default off until we're
   sure it can be removed"), and doubles as the **one-time first-pairing path** (C4).
2. Once in-band management is hardware-proven, the CDC flag stays off by default; the wired path is a
   recovery-only fallback.
3. Only after confidence: remove `USB_PERSONALITY_CDC_CONFIG`, its descriptors, the CDC half of
   `config_cdc_task`, and the `Connect USB` path in `index.html`. Diagnostics remain on UART — nothing
   to re-home.

---

## 5. Coexistence safety proof (why input/audio/motion are untouched)

1. **Idle = provably zero cost.** Disarmed path is a boolean check + `return` (btstack_host.c:1748).
   No advertising, ACL, notifications, connection attempts, or polling — same as today's normal mode
   (config-transports.md:27-28).
2. **BLE-peripheral-during-operation is already shipped.** `wake_adv` advertises in phased bursts
   (1200/100 ms, PREPARE→BROADCAST→BETWEEN→RESTORE) alongside the BT host stack. Management reuses the
   same advertiser and **the two never own it simultaneously** (config-transports.md:41; guard at
   btstack_host.c `if (wake_adv.active) return;`).
3. **No new cross-core races.** Commands execute on core0 regardless of transport; config state is
   `cfg_lock`-protected; core1 is the only flash writer (config-transports.md:76-78).
4. **Only flash parks core0** — handled by the deferred-save path (C5).
5. **Low-duty management link** (100–150 ms advertising interval; browser writes split to 20-byte
   MTU-safe pieces) bounds active-session airtime (config-transports.md:39).

**The one empirical gate:** audio on Pico 2 W during an *active* management session (idle is proven
safe; active is menu/stationary but must be measured). See §6 tool.

**New continuous cost to prove: low-duty advertising while enabled/awake/no-client.** Unlike today
(advertising only in Config, i.e. no gameplay), the always-connectable model advertises continuously
during awake gameplay. This must be shown not to disturb input/audio/gyro (HW check 7b).
**Fallback if it does:** the DualSense audio bridge already knows when it is streaming
(`ds5_audio_bridge`), so management advertising can be **suppressed while audio actively streams** and
resumed otherwise — a phone then connects during any non-audio moment (menus, most gameplay). This
keeps "connect anytime" in practice while removing advertising airtime from the audio-critical window.
Prefer the simple always-advertise path; adopt this suppression only if 7b shows measurable impact.

---

## 6. Test & tool strategy

### Host tests — written & green (2026-08-12), build via each file's header command
| Test | Proves |
|---|---|
| `test_config_wireless_bridge.c` (pre-existing) | happy-path SPSC, in-progress/deferred-reply back-pressure, busy rejection, oversized recovery, response chunking, session invalidation, allowlist policy |
| `test_config_wireless_bridge_edge.c` **(new)** | adversarial inputs: embedded-NUL truncates safely (no allowlist bypass), CR/blank-line noise, exact 127/128 capacity boundary, two-commands-in-one-frame drop, too-small-buffer drop (no overflow), 511/512 response boundary, pending-response back-pressure, NULL guard |
| `test_config_wireless_bridge_concurrency.c` **(new, `-pthread`)** | the cross-core SPSC handshake under real producer/consumer threads: 20 000 commands, in order, no loss/dup/tear (logic-race proof; ARM ordering rests on the acquire/release atomics + HW) |
| `test_mgmt_access.c` **(new)** | the pure access-control spec, **exhaustive over all 256 states / 10 invariants** — disabled=inert, wake outranks mgmt, asleep=silent, controller discovery may coexist, writes need enabled+connected+bonded+encrypted+allowlisted, bond needs enabled+window, advertise implies single-client safety, denied commands are never writable, and unbonded or plaintext clients cannot write |
| `test_mgmt_session.c` **(new)** | end-to-end composition of real bridge + real allowlist + dispatch gate: bonded user command succeeds; diagnostic rejected; unbonded refused; back-pressure; disconnect drops in-flight reply; disabled overrides bond |
| `test_bonds_command.c` **(new)** | strict grammar for legacy `bonds list`, versioned `bonds list v2 [cursor]`, and `bonds remove <n>`, with a wall of hostile inputs rejected |
| `test_mgmt_bonds.c` **(new)** | version-2 envelope bounds, cursor progress across sparse device-DB slots, complete aggregation, and fail-closed overflow |
| `test_bthid_android_controller.c` (pre-existing) | the Android *controller* HID contract (separate feature, already green) |

Production `src/mgmt_access.{c,h}` is the canonical spec linked by `test_mgmt_access.c`; the host
ATT/SM wiring composes it with the real wireless allowlist and bonded/encrypted link checks.

**Runtime-only (hardware-validated, not host-mockable):** prove the ATT encryption trigger, durable-
bond callback rejection, first-bond window, and stored-bond reconnect on Android — HW check 2.

### Built-in measurement tool (already present)
The config `state`/telemetry surface already exposes **`core1MaxGapUs` / `core1GapsOver10ms`**
(config.c:602) — core1 loop-cadence gap counters. Use them as the coexistence meter: capture the
counters (a) idle, (b) with a management client connected and actively swapping amiibo, (c) during a
flash `persist`. A management session must not increase `core1GapsOver10ms` beyond the flash-write
window. This gives an objective, on-device latency/jitter signal without external gear.

### Hardware validation matrix (extends config-transports.md:144-153 to normal mode)
1. **Feature disabled** (`g_mgmt_enabled` off): phone scan shows **no** management service; behavior
   byte-identical to today (zero-cost invisibility).
2. **Enabled, console awake:** service advertises at low duty; a **bonded** phone connects; an
   **unbonded** phone's write is rejected (encryption/bond gate).
3. Swap amiibo from phone **while a game is running** (menu context): active tag changes, **no input
   drop, no gyro glitch**.
4. Change colors from phone live; persist; confirm after continuing gameplay (no re-enumeration).
5. Deferred flash `persist` during gameplay: `core1GapsOver10ms` delta = **only** the write window.
6. **Audio gate (Pico 2 W):** audio playing + management client connected + actively swapping amiibo →
   **no stutter**. The one true empirical gate.
   - **6a. Wake gate (revised):** (i) automatic wake — a controller button press while asleep still
     wakes the console with the feature enabled; (ii) manage-while-asleep — a bonded phone can connect
     while the console sleeps and issue `wake` (G8), which wakes the console; (iii) coexistence — a
     wake burst fires correctly whether or not a management connection is active (concurrent
     advertise + peripheral connection on the CYW43). Proves "wake always gets the advertiser," the
     revised C5.
   - **6b. Gyro gate:** genuine-Pro2/DualSense motion is uninterrupted during an active session (idle
     is proven; this covers active).
7. **Latency meter:** capture `core1MaxGapUs`/`core1GapsOver10ms` (a) disabled, (b) enabled+advertising
   no client, (c) client connected active. Advertising-no-client must be indistinguishable from
   disabled within noise; client-active must not exceed the flash-write window.
8. Personality switch — **deferred** (see §7.3); not in this matrix until its own investigation lands.
9. Console sleeps mid-session → advertising stops + client dropped cleanly; on wake, reconnect works.
10. Regression: input, rumble, wake, LED, BOOTSEL, reconnect, motion, audio all unchanged with the
    feature present but **disabled** (default), and unchanged when enabled but no client connected.

---

## 7. Decisions (owner input applied 2026-08-12)

1. **Session model — RESOLVED: connect/disconnect anytime, no per-session gesture.** Owner: "People
   should be able to connect disconnect at any time." Model is pair-once-then-reconnect-freely (C3).
   Hard constraints reaffirmed: **must not add input latency, must not disturb audio/gyro (BT-timing
   dependent), and must not break wake-from-sleep** — captured as C5 (wake invariant) + the coexistence
   proof (§5) + the audio/gyro/wake HW gates (§6).
   - **§7a first-bond mechanism — RESOLVED (owner):** reuse the existing **double-tap pairing
     window** as the first-bond gate, and **triple-tap** to unpair; the app/portal manages individual
     saved pairings via `bonds list`/`bonds remove`. Full model in §2b. A phone bonds only inside the
     deliberate window; afterward it reconnects with no gesture.
2. **CDC fallback — RESOLVED: keep it, default-off build flag.** Owner: "Keep it gated default off
   until we're sure it can be removed." Also serves as the one-time first-pairing path.
3. **Personality switch — DE-RISKED (investigation §9, owner-confirmed 2026-08-12).** Owner: "check
   the plan… we don't want to break anything." The §9 investigation found it safe *in firmware*, and
   the one hardware assumption — the console accepting a mid-session PID re-enumeration — is
   **owner-confirmed to already work** (they cycle personality via BOOTSEL while plugged into the
   Switch 2 and it swaps cleanly). So it is **not blocked**; it can be in the first cut or a
   fast-follow. A command-driven switch just changes the trigger on that validated mechanism
   (`personality <target>` → target-request flag → existing cycle path). Amiibo + config remain the
   priority; personality switch is a small, safe add-on.
4. **Diagnostics — RESOLVED: already on UART in all modes.** Owner: "I thought uart worked in all
   modes for diagnostics? I've had it in procon 2 mode…" Confirmed (`ns2_uart_diag_task` runs
   unconditionally). No re-homing needed; CDC removal loses no diagnostics.

---

## 8. Path to "100% certain"

Everything structural is proven from the existing code; the residual unknowns are all runtime
behaviors with a concrete way to close each:

| Unknown | Closure |
|---|---|
| Encryption-required-before-write actually rejects unbonded writes | HW check 2 (SM/ATT is standard BTstack; high confidence) |
| **Low-duty advertising (no client) doesn't disturb input/audio/gyro** | `core1` meter HW check 7(b); the "always-connectable" cost |
| Active session doesn't disturb core1 cadence | `core1GapsOver10ms` meter, HW checks 5, 7 |
| **Pico 2 W audio survives an active session** | HW check 6 (the one true gate) |
| **Wake-from-sleep still fires with the feature enabled** | HW check 6a (wake outranks management, C5) |
| Gyro uninterrupted during an active session | HW check 6b |
| Deferred-flash path lands writes safely under gameplay | Host test (C6) + HW check 5 |
| Idle truly invisible/zero-cost when disabled | Already proven in code + HW check 1 |
| Forced personality re-enumeration is safe on a live console | ✅ Owner-confirmed (§9) — the BOOTSEL single-tap cycle already swaps cleanly while plugged into the Switch 2 |

The implementation is now host/build complete. Audio/gyro/wake coexistence, authorization behavior,
and production-default reboot behavior remain hardware gates. This document is the go/no-go
reference for those checks.

---

## 9. Personality-switch re-enumeration safety investigation (read-only, 2026-08-12)

**Question.** Is a *command-driven* forced output-personality switch (app → "become a GameCube
controller now") safe to implement without breaking a live console session, audio, wake, or bonds?

**The mechanism already exists.** `usb_apply_personality(next, reason)` (`usb.c:107`) does the full
transition: `tud_disconnect()` → `sleep_ms(USB_DETACH_MS)` → `usb_reset_personality_state(next)` →
set `g_usb_personality` → `tud_connect()`. It is what the **BOOTSEL single-tap cycle** already uses
(`usb_apply_controller_cycle`). A command-driven switch is the *same operation* to a *chosen* target
instead of the next-in-cycle.

**What's safe in firmware (verified by reading the code):**
- **Reset scope is minimal + private.** `usb_reset_personality_state` only re-inits the *incoming*
  personality's own module state (`ns2_init` / `switch_gc_reset` / `switch_joycon2_reset`). It does
  **not** touch the BT core, bonds, config/settings, or amiibo state (usb.c:69-99 comment: each
  personality's state "is never read while a DIFFERENT personality is active").
- **BT link + management link survive.** The switch re-enumerates only the console-facing USB; the
  BT core (core1) is untouched, so the paired controller stays connected and a connected management
  phone (which triggered the switch and wants to see the result) stays connected.
- **Core0 coherency is a known constraint.** `usb_apply_personality` blocks (`sleep_ms`) and must run
  "between `tud_task()` iterations, never mid-enumeration" (usb.c:102-106). Therefore a command must
  **not** call it inline; it sets an **edge-triggered request flag** consumed at the safe loop point —
  exactly the existing `g_usb_mode_cycle_requested` pattern (usb.c:29, consumed 197-203). A
  `personality <pro2|gc|jcl|jcr>` command sets `g_usb_target_personality` + a request flag; the loop
  applies it. This is a ~10-line mirror of proven code.

**What is inherent / acceptable:**
- **Audio interrupts when leaving Pro2.** GameCube/Joy-Con personalities have no UAC1 audio, so the
  audio function tears down on switch and re-enumerates on return to Pro2. Unavoidable for a
  deliberate switch; the app must warn "changing output type briefly interrupts audio."
- **The console sees a brief controller disconnect/reconnect.** Expected for any re-enumeration.

**The one blocking unknown — RESOLVED by owner confirmation (2026-08-12).** Whether the **console
gracefully accepts a mid-session re-enumeration to a different controller PID** was the sole open
gate. The owner confirmed this **already works in regular use**: the adapter powers on as Pro2, a
BOOTSEL single-tap cycles the personality *while plugged into the Switch 2*, and the console detects
the new controller and drops the old one. So the console-acceptance gate is **closed empirically**;
`compatibility-matrix.md`'s "hardware pending" row for the single-tap cycle is **stale and should be
updated to hardware-confirmed** (deferred here because that file has unrelated uncommitted edits — do
not entangle).

**Consequence:** a command-driven switch is the *same* validated re-enumeration, differing only in
the **trigger** (a BLE command vs a finger). The underlying `usb_apply_personality(next, …)` already
accepts an arbitrary target (the cycle just computes next-in-sequence), so targeting a specific
personality adds no new mechanism. The feature is therefore **de-risked**, not blocked — it can move
into the first cut if desired, or stay a fast-follow.

**Guards for the eventual command:** idempotent target (`next==old` already ignored); set an
edge-triggered target-request flag consumed at the safe loop point (mirror `g_usb_mode_cycle_requested`),
never call the blocking switch inline; coalesce rapid requests via the single flag (last-wins) and let
the app debounce; refuse while the console is asleep (wake first); and have the app warn that leaving
Pro2 briefly interrupts audio.

**Conclusion:** low-risk in firmware **and** hardware-proven for the load-bearing behavior (owner
confirms the runtime PID switch works on a live console). A command-driven switch is a small, safe
addition — `personality <pro2|gc|jcl|jcr>` sets the target-request flag; everything else is the
existing, exercised cycle path.
