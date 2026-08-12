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

### C3 — Authenticated authorization (the mandated new design)
- Raise the RX/TX characteristics to require encryption + bonding:
  `ATT_SECURITY_NONE` → `ATT_SECURITY_AUTHENTICATED` (LE Secure Connections; SM is already
  `SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION`). A random nearby phone then cannot write
  commands — only a bonded phone can.
- First bond happens inside a deliberate **arming window** (C4). After that, the bonded phone may
  reconnect and manage whenever the user re-arms (or, if we choose, silently — a decision in §7).
- Keep the existing single-client acceptance and per-write state checks.
- Keep management on the **LE Peripheral role**, classified before it can consume a controller/HID
  slot or enter controller SM/GATT discovery (config-transports.md:33-34 already does this).

### C4 — Arm gesture (on-demand, auto-disarm)
Because we are deprecating Config, **repurpose the current 2 s BOOTSEL hold**: instead of
re-enumerating to CDC, it **arms in-band management** (sets `g_mgmt_armed`, starts low-duty
advertising) while staying in the Pro2/GameCube/Joy-Con personality. Auto-disarm on a timeout
(e.g. 2–3 min) if no client connects, and on management-client disconnect (configurable). A short
LED pattern indicates "armed/advertising," matching the existing config-mode LED convention.

### C5 — Flash-op timing during gameplay
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

**What must be re-homed before the CDC personality can be removed:** the **developer/diagnostic
commands** (`state/raw/audiostat/imu/imuanom/fwreads/sw2cap/btid`) are USB/UART-only today and live
in config mode. They already have a natural home in the **UART diag link** (`ns2_uart_diag`, which
runs during normal operation and is what this project uses for tracing). Move/confirm them there (or
behind a debug build) so removing CDC loses no capability.

**Deprecation stages:**
1. Ship in-band management alongside the existing CDC config (both work). Gain hardware confidence.
2. Repurpose the 2 s-hold gesture to arm in-band management (C4). CDC config becomes a build-flag
   fallback (`-DCONFIG_CDC_FALLBACK`), default off.
3. Once diagnostics are confirmed on UART, remove `USB_PERSONALITY_CDC_CONFIG`, its descriptors, the
   CDC half of `config_cdc_task`, and the `Connect USB` path in `index.html`.

**Decision point (needs owner):** Web Bluetooth requires Chrome/Edge + a BLE-capable host + secure
context. Removing CDC removes the *wired, browserless* fallback. Recommendation: keep CDC behind a
default-off build flag for one release rather than deleting outright. (See §7.)

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

---

## 6. Test & tool strategy

### Host tests (extend the existing pattern — all currently green)
- `test_config_wireless_bridge.c` already covers fragmented commands, busy rejection, oversized-line
  recovery, response chunking, disconnect session invalidation, stale-response rejection
  (config-transports.md:136-137). **Add:**
  - **Arming gate logic (C1):** RX writes rejected when `!(g_usb_config_mode || g_mgmt_armed)`;
    accepted when armed; auto-disarm timeout transitions.
  - **Allowlist in normal mode:** every diagnostic command rejected over wireless while armed;
    every user command accepted.
  - **Deferred-flash routing (C5):** a wireless `save`/`amiibo commit` sets the deferred request
    rather than busy-waiting; a mock core1 tick performs it; response is published after.
- **New:** a small state-machine test for `g_mgmt_armed` (arm gesture edge → advertise → client
  connect → disarm on disconnect/timeout), mirroring `test_bthid_late_identity.c`'s mock-transport
  style.
- **Security note:** the encryption-required-before-write property (C3) is an SM/ATT runtime
  behavior; pin it in **hardware validation** (below) rather than a host mock.

### Built-in measurement tool (already present)
The config `state`/telemetry surface already exposes **`core1MaxGapUs` / `core1GapsOver10ms`**
(config.c:602) — core1 loop-cadence gap counters. Use them as the coexistence meter: capture the
counters (a) idle, (b) with a management client connected and actively swapping amiibo, (c) during a
flash `persist`. A management session must not increase `core1GapsOver10ms` beyond the flash-write
window. This gives an objective, on-device latency/jitter signal without external gear.

### Hardware validation matrix (extends config-transports.md:144-153 to normal mode)
1. Disarmed: phone scan shows **no** management service in Pro2/GC/Joy-Con (idle invisibility).
2. Arm gesture → service advertises; only a **bonded** phone can write (unbonded write rejected).
3. Swap amiibo from phone **while a game is running** (menu context): tag changes, no input drop.
4. Change colors from phone live; persist; confirm after continuing gameplay (no re-enumeration).
5. Deferred flash `persist` during gameplay: measure `core1GapsOver10ms` delta = only the write.
6. Pico 2 W **audio** playing + management client active: confirm no stutter (the empirical gate).
7. Motion (gyro) uninterrupted during an active management session.
8. Personality switch from phone (if included): console shows the expected reconnect, comes back
   correctly on the new personality.
9. Auto-disarm after timeout / on client disconnect; service disappears; back to zero-cost idle.
10. Regression: input, rumble, wake, LED, BOOTSEL, reconnect unchanged with the feature present but
    idle.

---

## 7. Open decisions (need owner input before build)

1. **Re-arm model:** must the user re-arm (BOOTSEL) every management session, or may a *bonded* phone
   reconnect-and-manage silently? (Silent = convenient; armed-only = strictly safer + lower idle
   airtime. Recommend: armed window for first bond; afterward allow bonded reconnect but only accept
   *writes* while armed — a middle path.)
2. **CDC fallback lifetime:** delete the CDC config personality outright, or keep it behind a
   default-off build flag for one release for browserless/wired recovery? (Recommend: keep one
   release.)
3. **Personality switch scope:** include "change output personality from phone" in v1 (accepting the
   console reconnect), or defer it and ship amiibo+config first? (Recommend: defer; it's the only
   disruptive op and the least frequent.)
4. **Diagnostics home:** confirm the developer commands move to UART diag (vs a debug build) before
   CDC removal.

---

## 8. Path to "100% certain"

Everything structural is proven from the existing code; the residual unknowns are all runtime
behaviors with a concrete way to close each:

| Unknown | Closure |
|---|---|
| Encryption-required-before-write actually rejects unbonded writes | HW check 2 (SM/ATT is standard BTstack; high confidence) |
| Active session doesn't disturb core1 cadence | `core1GapsOver10ms` meter, HW checks 5-7 |
| Pico 2 W audio survives an active session | HW check 6 (the one true gate) |
| Deferred-flash path lands writes safely under gameplay | Host test (C5) + HW check 5 |
| Idle truly invisible/zero-cost | Already proven in code + HW check 1 |

No firmware change is proposed until (a) these host tests are written and green and (b) the owner
resolves §7. This document is the go/no-go reference.
