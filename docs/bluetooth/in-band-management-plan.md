# In-Band Management — Implementation Plan (proposal, pending approval)

**Status:** 🔵 PLAN ONLY — no code changes until explicitly approved. Investigation complete
2026-08-12. Supersedes the config-mode-only model in
[config-transports.md](../architecture/config-transports.md) *if approved*.

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
- `g_mgmt_enabled` (a build/user setting, default off during rollout) replaces the config-mode gate
  in `config_ble_service_task` and the RX/advertising guards: `g_usb_config_mode` →
  `(g_usb_config_mode || g_mgmt_enabled)`. When disabled, byte-identical to today (zero-cost return).

### C4 — Authenticated authorization (the mandated gate, now the *sole* gate)
Without the physical Config gesture, **bonding is the only access control** — so it must be real:
- Raise RX/TX from `ATT_SECURITY_NONE` → **`ATT_SECURITY_AUTHENTICATED`** (LESC; SM already
  `SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION`). Only a bonded phone can write commands.
- **First-bond is the one remaining security decision (§7.1).** With `IO_CAPABILITY_NO_INPUT_NO_OUTPUT`
  the SM does Just-Works pairing (no user confirmation), so "always advertising + open first-bond"
  would let *any* nearby phone bond and manage. Recommended: **first pairing happens once through the
  kept CDC Config fallback (C-dep) or a one-time pairing window; thereafter the bonded phone connects
  anytime with no gesture.** A one-time setup step is *not* the per-session re-arm that was rejected.
- Keep single-client acceptance and the LE-Peripheral-role classification before any slot/SM/GATT use
  (config-transports.md:33-34).

### C5 — Wake-from-sleep is never broken (hard invariant)
Wake advertising **strictly outranks** management. Concretely:
- Management advertises **only while the console is awake** (`!tud_suspended()`), and **suppresses +
  yields the advertiser** whenever `wake_adv` is active or pending (the `if (wake_adv.active) return;`
  guard already exists; extend it so a running management advert is *stopped*, not just not-started,
  when wake needs the radio).
- When the console sleeps, management stops advertising and disconnects any client so wake owns the
  single LE advertiser exactly as today. (A phone therefore cannot manage while the console sleeps —
  acceptable; wake the console first.)
- This makes "don't break wake" a checkable invariant, validated in HW check 6a below.

### C6 — Flash-op timing during gameplay
`save`, `amiibo commit`, `amiibo persist` write flash, which parks core0 (brief input/audio hitch).
The deferred-save machinery already exists (`save_not_before_ms`, `save_requested`, core1 control
tick is "the only flash writer" — config-transports.md:76-78). Route wireless-initiated flash ops
through the **deferred** path (not the 2 s busy-wait in the current `save` handler, config.c:1010-1018)
so the write lands at a safe point. In practice amiibo/config changes happen in menus (stationary),
so the hitch is a non-issue, but the deferred path makes it correct regardless.

---

## 3. Per-command safe-live classification (full audit of `handle_line`)

| Command | Effect | Wireless today | Live-safe during gameplay? |
|---|---|---|---|
| `info`, `ping` | static / noop | ✅ allow | ✅ trivial |
| `get` | read config (cfg_lock) | ✅ allow | ✅ read |
| `device` | read connected-controller summary | ✅ allow | ✅ read |
| `body`/`jcl`/`jcr`/`lb` | set colors in RAM | ✅ allow | ✅ RAM write under cfg_lock |
| `amiibo status/read/select/present/eject/downloaded/cancel` | RAM/state only | ✅ allow | ✅ no flash; select swaps active tag |
| `amiibo begin/chunk` | buffer upload in RAM | ✅ allow | ✅ RAM (bounded) |
| `amiibo commit`, `amiibo persist`, `save` | **flash write** | ✅ allow | ⚠ via **deferred** path (C5) |
| `amiibo clear` | clears pending / flash? | ✅ allow | ⚠ verify flash vs RAM |
| `state`, `raw`, `audiostat`, `imu`, `imuanom`, `fwreads`, `sw2cap`, `btid` | developer/diagnostic reads | ❌ CDC/UART only | n/a (stay off wireless) |

The allowlist boundary (user config vs developer diagnostics) is already correct for in-band use.
**Only the flash-writing subset needs the deferred path.** No handler assumes exclusivity beyond
flash timing; all run on core0 under the same parser.

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

### Host tests (extend the existing pattern — all currently green)
- `test_config_wireless_bridge.c` already covers fragmented commands, busy rejection, oversized-line
  recovery, response chunking, disconnect session invalidation, stale-response rejection
  (config-transports.md:136-137). **Add:**
  - **Enable gate logic (C1/C3):** RX writes rejected when `!(g_usb_config_mode || g_mgmt_enabled)`;
    accepted when enabled.
  - **Allowlist in normal mode:** every diagnostic command rejected over wireless while enabled;
    every user command accepted.
  - **Deferred-flash routing (C6):** a wireless `save`/`amiibo commit` sets the deferred request
    rather than busy-waiting; a mock core1 tick performs it; response is published after.
  - **Wake priority (C5):** a mock "console asleep / wake active" input suppresses management
    advertising and drops any client — pure state-logic, no radio.
- **New:** a small state-machine test for the management enable/advertise lifecycle (enabled + awake +
  no scan → advertise; client connect → stop advertising; console sleep or wake-active → suppress +
  drop client), mirroring `test_bthid_late_identity.c`'s mock-transport style.
- **Security note:** the encryption-required-before-write property (C3) is an SM/ATT runtime
  behavior; pin it in **hardware validation** (below) rather than a host mock.

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
   - **6a. Wake gate:** with the feature enabled, put the console to sleep → management advertising
     **stops**, `wake_adv` owns the advertiser, and **wake-from-sleep still works**. Then wake → the
     bonded phone can reconnect. (Directly proves "don't break wake.")
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
   - **Sub-decision still open (§7a): first-bond mechanism.** Just-Works pairing means "always
     advertising + open first-bond" lets any nearby phone bond. Recommended: first pairing via the
     kept CDC Config fallback (or a one-time pairing window); afterward the bonded phone connects with
     no gesture. Needs an explicit pick before build.
2. **CDC fallback — RESOLVED: keep it, default-off build flag.** Owner: "Keep it gated default off
   until we're sure it can be removed." Also serves as the one-time first-pairing path.
3. **Personality switch — DEFERRED pending a dedicated re-enumeration-safety investigation.** Owner:
   "check the plan and plan accordingly before implementing forced personality change, we don't want
   to break anything." Ships **not** in the first cut. Before it is ever added, a separate
   investigation must prove a *forced* runtime re-enumeration is safe: does `usb.c`'s existing
   controller-cycle re-enumeration (`usb_apply_controller_cycle`, already used by the BOOTSEL
   mode-cycle) drop and re-add cleanly on a live console without hanging a game; how it interacts with
   an active BT controller link, audio streaming, wake state, and bonds; and whether the console
   re-pairs the "new" controller gracefully. Amiibo + config ship first without it.
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
| Forced personality re-enumeration is safe on a live console | **Separate investigation (§7.3) before that op ships** |

No firmware change is proposed until (a) the host tests above are written and green, (b) the §7a
first-bond mechanism is chosen, and (c) the audio/gyro/wake HW gates pass. This document is the
go/no-go reference. Personality switch is out of the first cut pending its own investigation.
