# Pico SDK 2.3.0 / BTstack 1.8.2 migration (2026-08-25)

## Post-build hardware status update

The maintainer subsequently flashed a Stage C candidate to a different Pico 2 W
and began an endurance run on a different Android device. At the latest report,
the run had exceeded three hours of continuous established BLE management with
five-second background requests still completing, no observed HCI `0x08`, and no
management disconnect. Approximately 20 Touch Gamepad reconnects also completed
without the familiar pre-C Classic establishment failure. The run remains in
progress and was not touched during the documentation closeout.

Deliberate pre-C retesting on the old firmware and old APK reproduced established
BLE `0x08` repeatedly after roughly 2-14 minutes. The Stage C observation is
therefore a material initial reliability improvement, not proof of zero failure
probability or attribution to one dependency change. See
[`established-management-gatt-failure-2026-08-25.md`](established-management-gatt-failure-2026-08-25.md)
for the preserved A/B evidence and Condition F closeout.

Read-only build inspection still resolves the repository candidate to the stock
SDK-bundled BTstack v1.8.2 commit `075a0780`; no later BTstack pin is configured.
The current Pico 2 W artifact has embedded identity `2272cb5e+dirty` and SHA-256
`750cc6fcdd137a2539be66442471ac80c1814094bcbfac0bbd2bc6489ba15227`, but the
active board was deliberately left untouched, so the host cannot prove that this
exact file is its running image.

## Build-only migration result

**Status: Stage C modernization candidate builds clean on both production
boards; no hardware validation performed. BLE LSTO reliability remains to be
validated on hardware.**

> **KNOWN RISK IN THE STOCK BTstack 1.8.2 CANDIDATE — BTstack 1.8.2 corrupts the Classic link key DB
> in
> the BR/EDR → LE cross-transport direction.** STATUS.md's 2026-08-22
> "stay on 2.2.0" audit called this, and it was **reproduced from source in the
> installed v1.8.2 tree** during this migration (see
> [Cross-transport key derivation regression](#cross-transport-key-derivation-regression)).
> It is not caused by anything in this migration and cannot be fixed by
> PicoSwitch configuration. A later exact upstream pin containing the confirmed
> CTKD fixes is planned but is not present in the currently configured build.

PicoSwitch now builds against Pico SDK 2.3.0, BTstack 1.8.2, cyw43-driver 1.1.1
and the Arm GNU Toolchain 15.2.Rel1. Four upstream changes needed deliberate
adaptation rather than compiler-error chasing, and three of them are silent —
they compile either way and change behaviour at runtime:

1. `hids_client` was renamed `hids_host`, **including the memory-pool macro**.
   `MAX_NR_HIDS_CLIENTS` is simply ignored by 1.8.2, which then builds a
   zero-entry HIDS Host pool.
2. `HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT` was removed. (Loud.)
3. `gap_disconnect()` no longer synthesises a disconnection-complete event for a
   handle the controller has already released.
4. `sm_init()` now enables **LE Secure Connections Only** by default and raises
   the minimum encryption key size to 16.

No Bluetooth workaround, timing change, keepalive, timeout change, or
connection-priority change was introduced. The migration is deliberately clean so
it can be measured against the unresolved established-BLE `0x08` failure
described in
[`established-management-gatt-failure-2026-08-25.md`](established-management-gatt-failure-2026-08-25.md).

## Scope and provenance

- Branch: `ns2-testing`
- Source HEAD at migration time: `2272cb5e6eae4dc79d3eb986ea923e8419f6ce3c`
- Working tree was dirty with an unrelated Bluetooth-diagnostics investigation
  (Android companion, `tools/mgmt_watch.ps1`, `dumps/`, `STATUS.md`). All of it
  was preserved; no Git state-changing command was run.
- No UF2 was flashed. No bonds were reset. No hardware was touched.
- Host: Windows 11, builds driven by `build.ps1`.

## Dependency change

| Component | Before | After |
| --- | --- | --- |
| Pico SDK | 2.2.0 (`a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`) | 2.3.0 (`98a542c1a62fb549ffb5d66a3e5892b06276b670`) |
| BTstack | v1.6.2-1-g501e6d2b8 (`501e6d2b86e6c92bfb9c390bcf55709938e25ac1`) | v1.8.2 (`075a0780f0fad7ff67d58ac19f46e8953656a752`) |
| cyw43-driver | v1.1.0 (`dd7568229f3bf7a37737b9e1ef250c26efe75b23`) | v1.1.1 (`055d64274b014dd7b1c2fc94d26e8a18face7124`) |
| Toolchain | Arm GNU 14_2_Rel1 | Arm GNU 15_2_Rel1 (GCC 15.2.1 20251203) |
| CMake | 3.31.5 | 4.3.4 |
| Ninja | 1.12.1 | 1.13.2 |
| picotool | 2.2.0-a4 | 2.3.0 |

All six dependency Git trees were verified clean at the recorded commits before
any edit.

### The CYW43439 Bluetooth firmware did not change

`lib/cyw43-driver/firmware/cyw43_btfw_43439.h` is byte-identical between the two
SDK trees:

```
2075e3be3b7e11734404351df68be47b2d71cce646845f484e8f6813159f8647
```

**This migration cannot have fixed a controller-side defect.** It replaces the
RP2040/RP2350 host-side integration, BTstack, and the cyw43-driver revision. The
PatchRAM image downloaded into the CYW43439 is the same one that was running
during every captured failure. Do not attribute any post-migration reliability
change to a newer Bluetooth controller firmware.

## Build system

### One canonical BTstack root

`CMakeLists.txt` previously hardcoded:

```cmake
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)
```

before `pico_sdk_import.cmake` ran. The SDK, however, resolves
`PICO_BTSTACK_PATH` (CMake variable, then environment, then the bundled copy)
inside `src/rp2_common/pico_btstack/CMakeLists.txt` — a subdirectory scope whose
value never reaches the top level. Supplying `PICO_BTSTACK_PATH` would therefore
have compiled the SDK's `pico_btstack_*` targets from one BTstack tree while
PicoSwitch's private includes, its bluedroid codec headers, and its substituted
`hid_host.c` came from another. Header/ABI skew that links.

`BTSTACK_ROOT` is now resolved after `pico_sdk_init()` with the SDK's own
precedence, and then checked: `${BTSTACK_ROOT}/src/hci.c` must appear in
`pico_btstack_base`'s `INTERFACE_SOURCES`, or configuration fails with
`BTstack root skew`.

### SDK version selection

The SDK version is declared twice and the two override each other in a
non-obvious order: `CMakeLists.txt`'s `sdkVersion` feeds `pico-vscode.cmake`,
which sets `PICO_SDK_PATH` **as a CMake variable** before
`pico_sdk_import.cmake` consults the environment — so `build.ps1`'s
`$env:PICO_SDK_PATH` loses silently whenever they disagree. Both were updated,
and two guards now make disagreement impossible to miss:

- `build.ps1` reads `PICO_SDK_PATH` back out of `CMakeCache.txt` after
  configuring and throws `SDK selection skew` if it is not what it asked for;
- `tools/test_btstack_dependency_contract.py` compares the declarations
  statically, before a build runs at all.

VS Code metadata (`settings.json`, `c_cpp_properties.json`, `launch.json`,
`tasks.json`) was moved off 2.2.0 / 14_2_Rel1 / picotool 2.2.0-a4 as well, so
IntelliSense, the debugger's SVD, and the flash task no longer point at the
superseded tree.

## BTstack API migration

### HIDS Client → HIDS Host

Mechanical rename with identical signatures across 13 call sites: `hids_client_*`
→ `hids_host_*`, `ble/gatt-service/hids_client.h` → `hids_host.h`, and the local
`hids_client_handler` / `start_hids_client` statics renamed to match. No
compatibility aliases were added; the build is Stage C only.

The load-bearing part is `btstack_config.h`: `MAX_NR_HIDS_CLIENTS` →
`MAX_NR_HIDS_HOSTS`. BTstack 1.8.2's `btstack_memory.c` keys the pool off the new
name and `#define`s it to 0 when absent. The old spelling produces a firmware
that builds, links, and then cannot allocate a single BLE HID connection.

SDK 2.3.0 handles the source-list side itself (it compiles `hids_host.c` plus
`gatt_service_client.c` when they exist, else `hids_client.c`), so no PicoSwitch
CMake change was needed for the rename.

### Removed HID protocol fallback mode

`HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT` is gone from `btstack_hid.h`.
Three PicoSwitch call sites used it, selected by a `bt_hid_mode_t` field on the
device profile, requested by exactly one profile (Xbox) and used as the default
for any incoming Classic peer whose name was not yet known.

Reading what BTstack 1.6.2 actually did with it decided the question:

- **Outgoing, SDP succeeded** — it took the `HID_PROTOCOL_MODE_BOOT` branch
  (`hid_host.c:1009-1015`): forced `interrupt_psm` to the well-known
  `BLUETOOTH_PSM_HID_INTERRUPT` instead of the SDP-advertised one, and
  transmitted `SET_PROTOCOL` with the requested mode as its low nibble — value
  `2`, which is not a defined HID protocol mode (0 = Boot, 1 = Report). A
  malformed control message.
- **Outgoing, SDP failed or carried no HID record** — it retried on the
  well-known PSMs in Boot protocol mode (`hid_host.c:701-717`). The only real
  capability.
- **Incoming, SDP succeeded** — identical to `REPORT`; 1.6.2 overwrote
  `requested_protocol_mode` with `HID_PROTOCOL_MODE_REPORT` regardless
  (`hid_host.c:722-724`).

Boot protocol delivers keyboard/mouse boot reports, and this firmware has no
boot-report parsing at any layer — `HID_PROTOCOL_MODE_BOOT` appears nowhere in
it. The single profile requesting the mode is inherited from the
Joypad-OS-derived original with no capture, experiment, or document behind the
choice, and Xbox pads reach this firmware over BLE HOGP.

**Decision:** all three sites request `HID_PROTOCOL_MODE_REPORT`, and
`bt_hid_mode_t` / `.hid_mode` were retired rather than left as a selector that
selects nothing. The reasoning is preserved in a `RETIRED FIELD` comment in
`bt_device_db.h` so it is not rediscovered as an omission.

**Compatibility loss on record:** a Classic device whose SDP HID record is
missing or unreadable is now refused, where 1.6.2 would have retried it in Boot
mode. No such device is known to this project, and had one connected, its boot
reports would not have been parsed.

### gap_disconnect semantics

BTstack 1.6.2:

```c
uint8_t gap_disconnect(hci_con_handle_t handle){
    hci_connection_t * conn = hci_connection_for_handle(handle);
    if (!conn){ hci_emit_disconnection_complete(handle, 0); return 0; }
```

BTstack 1.8.2 returns `ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER` and emits
nothing. Every teardown in `btstack_host.c` converges its own record from
`HCI_EVENT_DISCONNECTION_COMPLETE`, so a stale handle used to clean itself up.

All 24 `gap_disconnect()` call sites were audited and split in two:

**Event-driven (14 sites)** — they disconnect the very handle the event they are
handling just delivered (`HCI_EVENT_CONNECTION_COMPLETE`,
`HCI_SUBEVENT_LE_CONNECTION_COMPLETE`, `HCI_EVENT_AUTHENTICATION_COMPLETE`,
`HCI_EVENT_ENCRYPTION_CHANGE`, the SM events, `HID_SUBEVENT_CONNECTION_OPENED`),
or a handle just obtained from `hci_connection_for_bd_addr_and_type()`, which
answers `NULL` for a released link. That handle is live by construction. These
keep calling `gap_disconnect()` directly: giving them a second teardown mechanism
is exactly the parallel-path problem
`classic_companion_release_on_mgmt_loss()`'s comment warns about.

**Durable-record owners (10 sites)** — they name a handle held in local state
that outlives the call. These route through `btstack_host_request_disconnect()`,
which returns whether an event is actually coming. The pending-vs-converge rule
itself lives in `ns2_bt_disconnect_outcome()` (`src/ns2_bt_lifecycle.c`) so it is
pinned by a host test; note that `ERROR_CODE_COMMAND_DISALLOWED` is **not** a
convergence case — it means a disconnect is already in flight and converging
early would tear down twice.

| Call site | Record at risk | Convergence when no event is coming |
| --- | --- | --- |
| `btstack_host_uart_force_fresh_pairing` | BLE controller slot | `ble_connection_release_orphan()` |
| `classic_companion_release_on_mgmt_loss` | `classic_companion_acl_handle` | none needed — already cleared before the call; counter only |
| `config_ble_service_task` (disable) | `config_ble.closing` latch | `config_ble_handle_disconnect()` |
| `btstack_host_delete_all_bonds` | `config_ble.handle` | `config_ble_handle_disconnect()` |
| `btstack_host_forget_device_typed` (mgmt) | `config_ble.handle` | `config_ble_handle_disconnect()` |
| `btstack_host_forget_device_typed` (BLE) | BLE controller slot | `ble_connection_release_orphan()` |
| `btstack_host_forget_device_typed` (Classic) | live ACL only | counter only |
| `btstack_host_disconnect_all_devices` (Classic) | live ACL only | `hid_host_disconnect()` fallback |
| `btstack_host_disconnect_all_devices` (BLE) | BLE controller slot | `ble_connection_release_orphan()` |
| `switch2_init_retry_task` | `sw2_init_handle` | slot release, else `switch2_cleanup_on_disconnect()` |

The last one was a concrete regression, not a theoretical one. `sw2_init_handle`
is cleared only by `switch2_cleanup_on_disconnect()`, which the disconnect event
drives. With no event arriving, the retry-exhausted recovery path would re-run
every `SW2_INIT_RETRY_INTERVAL_MS` forever without ever rearming — the exact
permanently-stuck state that recovery exists to escape.

No fake HCI events are fabricated. `ble_connection_release_orphan()` releases
resources only; it deliberately does not make the reconnect, stale-bond, or
scan-resumption decisions the event path makes, because those are decisions about
a link that just died and this is not that situation. The host-global
GATT/battery scratch state is likewise left to the ordinary path, since it is
shared with any other live BLE connection.

**Diagnostics:** `disconnect_handle_already_gone` counts the condition, prints
the handle and status when it occurs, and is published as `disc.already_gone` in
the UART `btstate` JSON. Under 1.6.2 this situation was invisible. A non-zero
value means some record here outlived its ACL — worth knowing regardless of
whether the convergence papered over it.

## HID Host long-report shim

`cmake/btstack_hid_host_long_report.c` compiles upstream `hid_host.c` inside its
own translation unit (CMake removes the upstream one from
`pico_btstack_classic`) for live DualSense-audio builds. It was ported to
1.8.2's implementation rather than carried forward.

**What upstream now provides:** the 16-bit report length. 1.6.2 declared
`hid_host_send_report(..., uint8_t report_len)` while storing `report_len` as a
`uint16_t` internally, which is why the 547-byte DualSense audio report needed a
private entry point at all. Upstream widened the parameter in 1.8.2 (commit
`6f867fb49`), so the local accept sequence was deleted and the entry point now
delegates to `hid_host_send_report()`.

**What is still patched:**

1. *The pending-send guard.* Upstream accepts any state in
   `[HID_HOST_CONNECTION_ESTABLISHED, HID_HOST_W4_INTERRUPT_CONNECTION_DISCONNECTED)`,
   and `HID_HOST_W2_SEND_REPORT` sits inside that range. A second send arriving
   while the first is still queued overwrites `connection->report`,
   `connection->report_id` and `connection->report_len` before L2CAP has read
   them. The DS5 audio state machine retries rejected sends on every 2 ms service
   pass, so this window is ordinary, not theoretical.
   `ns2_hid_host_send_long_report()` accepts only from the exact idle established
   state and returns `ERROR_CODE_COMMAND_DISALLOWED` otherwise; the caller
   retries.
2. *The `l2cap_send_prepared` interposition*, so the audio diagnostics observe
   the real L2CAP submission rather than merely HID Host accepting the report
   into `W2_SEND_REPORT`. Bonded reconnects are owned by HID Host and reach that
   boundary only through this file.

`hid_host_get_connection_for_hid_cid()` remains `static` upstream, so the
textual `#include <classic/hid_host.c>` is still what makes the guard
implementable. The local delta is now three statements plus the diagnostic
wrapper.

The CMake `FATAL_ERROR` that fires if upstream `hid_host.c` cannot be located in
`pico_btstack_classic`'s sources was kept and its message made specific: without
the removal, both translation units compile and collide at link time.

Verified from the generated build graph: `pico2_w` (audio) references
`btstack/src/classic/hid_host.c` zero times and the shim three times; `pico_w`
(non-audio) is the exact inverse.

## Security

BTstack 1.8.2 changed three defaults. Two were already overridden explicitly.

| Setting | 1.6.2 default | 1.8.2 default | PicoSwitch |
| --- | --- | --- | --- |
| `sm_min_encryption_key_size` | 7 | 16 | explicit `sm_set_encryption_key_size_range(7, 16)` — unchanged |
| Classic `ssp_auto_accept` | 1 | 0 | explicit `gap_ssp_set_auto_accept(0)` — unchanged |
| LE `sm_sc_only_mode` | false | **true** | **now explicit `false`** |

### LE Secure Connections Only

1.8.2's `sm_init()` sets `sm_sc_only_mode = true` whenever
`ENABLE_LE_SECURE_CONNECTIONS` is defined (`sm.c:5230`), which
`src/btstack_config.h` does. SC-Only rejects any pairing whose peer does not
offer Secure Connections (`sm.c:1292`), rejects a remote max key size below 16
(`sm.c:1276`), and disconnects an encrypted link whose actual key size is below
16 (`sm.c:4956`, `sm.c:4994`).

That directly contradicts the documented intent at the call site, which predates
this migration:

> Configure SM — bonding + LE Secure Connections. Some HOGP devices (e.g.
> Augmental MouthPad) accept a legacy-paired connection and even accept the
> report CCC writes, but will NOT stream HID notifications unless the link is
> secured with LE Secure Connections. **Request SC (peers without SC fall back to
> legacy automatically, so other controllers are unaffected).**

Requesting Secure Connections and requiring it are different policies, and this
host has always meant the first. Inheriting the flipped default would also have
invalidated existing LE bonds formed under the old one — including the Android
management bond, which is the relationship the current failure investigation is
measuring.

`sm_set_secure_connections_only_mode(false)` is therefore called explicitly. The
point is not the value; it is that the policy is now stated in PicoSwitch source
rather than inherited from whichever BTstack version happens to be pinned.
Raising it to `true` is a deliberate product decision about which BLE peers
remain supported, and would need evidence that every required peer supports SC.

**Confidence: Confirmed** for the upstream default change (source-verified in
both trees). **Not hardware-validated** for pairing behaviour after migration —
that is item 3 in the hardware plan below.

The existing Android authentication stand-down
(`ns2_bt_defer_classic_authentication`, `ns2_bt_defer_classic_encryption`) is
untouched. No authentication policy changed.

## Cross-transport key derivation regression

**Confidence: Confirmed (source, both trees). Reachability in this product:
Strong Evidence. Observed on hardware: no.**

`src/btstack_config.h` defines `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION`, and the
Android companion depends on it: it bonds over LE for management, and its
Controller Link's Classic key is cross-transport-derived from that LE bond (see
`ns2_bt_classic_trust_present()` in `include/ns2_bt_lifecycle.h`).

CTKD runs in two directions, and BTstack keeps both in `setup->sm_link_key` —
but in **different byte orders**:

| Direction | `setup->sm_link_key` holds | Byte order |
| --- | --- | --- |
| LE → BR/EDR | `reverse_128(hash)` of the freshly derived key (`sm.c:1825`) | HCI order — correct for storage |
| BR/EDR → LE | `reverse_128(hci_connection->link_key)` of the **existing** key (`sm_ctkd_fetch_br_edr_link_key`, `sm.c:2167-2172`) | reversed — wrong for storage |

BTstack 1.6.2 tolerated that, because only the LE → BR/EDR branch ever stored a
Classic key (`sm.c:1774-1780`); the BR/EDR → LE branch called
`sm_store_bonding_information()`, which writes the LE bond only.

BTstack 1.8.2 moved the derivation ahead of the DHKey Check (upstream
`232f80e60`) and routes **both** directions through
`sm_process_bonding_information()` → `sm_store_classic_bonding_information()`
(`sm.c:1536-1544`, called unconditionally at `sm.c:1607-1609`). Its guard is

```c
if ((setup->sm_link_key_type != INVALID_LINK_KEY) &&
    (sm_ctkd_from_le_could_update(sm_conn))) {
    gap_store_link_key_for_bd_addr(setup->sm_peer_address,
                                   setup->sm_link_key, setup->sm_link_key_type);
}
```

and neither condition distinguishes direction:

- `sm_ctkd_fetch_br_edr_link_key()` sets `sm_link_key_type` from
  `hci_connection->link_key_type`, so it is valid, not `INVALID_LINK_KEY`;
- `sm_ctkd_from_le_could_update()` (`sm.c:2626-2641`) tests the identity address
  and relative authentication level — a BLURtooth mitigation — and returns true
  both when no stored key exists and when the derived key is at least as
  authenticated as the stored one.

So on a BR/EDR → LE derivation, BTstack 1.8.2 writes the peer's **existing**
Classic link key back into the link key DB **byte-reversed**. The next Classic
reconnect for that peer then authenticates against a corrupted key.

Upstream fixes exist and are **not** in v1.8.2 — both landed 2026-08-18 and are
master-only: `f25861592` (store the derived link key in `sm_key_t` byte order)
and `a0f82a97c` (*"store link key only for LE→BR/EDR key derivation"*, Fixes
\#744). Note that v1.8.2 *does* already contain the
`sm_ctkd_from_le_could_update()` guard, which is easy to mistake for
`a0f82a97c`; it is a different, authentication-level guard and does not gate on
direction.

### Reachability here

PicoSwitch is a **responder** on this path — no local action initiates it.
`sm_init()` registers the BR/EDR Security Manager fixed channel
(`sm.c:5263`), and `l2cap.c:1971-1974` advertises it to peers whenever
`ENABLE_BLE && ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` are both defined, which is
this build. Any peer that sends an SMP Pairing Request over BR/EDR reaches
`SM_BR_EDR_RESPONDER_PAIRING_REQUEST_RECEIVED` (`sm.c:3332-3342`) →
`sm_ctkd_fetch_br_edr_link_key()` → `SM_BR_EDR_W4_CALCULATE_LE_LTK` →
`sm_process_bonding_information()` → the corrupting store.

- **Physical Classic controllers: unaffected.** They are SSP devices that do not
  run SMP over BR/EDR.
- **Android companion: exposed.** AOSP runs BR/EDR CTKD against a peer
  advertising that fixed channel. Whether it does so on Controller Link
  establishment specifically, or only when it creates a Classic bond, has not
  been observed here — hence Strong Evidence rather than Confirmed.

The predicted symptom is the one this project has chased before: Classic
reconnect fails with authentication failure or `PIN_OR_KEY_MISSING` and stays
broken until bonds are wiped. `btstack_host.c`'s disconnect handler already
deletes the local bond on those two reasons, so the visible behaviour would be
repeated re-pairing rather than a hard failure.

### Options (product decision — not taken in this pass)

1. **Flash and watch.** Accept the risk for bench testing, keep the Classic
   reconnect path under observation, and treat a re-pair loop on the companion as
   this defect until proven otherwise. Physical controllers are not at risk.
2. **Wait for a Pico SDK that ships a BTstack containing `a0f82a97c`.** This is
   what STATUS.md recommended on 2026-08-22; the Stage C tree is then ready to
   rebuild against it with no further source work.
3. **Substitute `sm.c`** the way `hid_host.c` is substituted, interposing on
   `gap_store_link_key_for_bd_addr` to refuse the write when the SM connection
   arrived on `L2CAP_CID_BR_EDR_SECURITY_MANAGER`. Technically feasible with the
   existing pattern, but it is a security-relevant change to the project's most
   sensitive subsystem and needs its own design pass, its own tests, and its own
   hardware validation. Explicitly out of scope for an SDK migration.
4. **Disable `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION`.** Removes the corrupting
   path entirely, but also removes the LE → BR/EDR derivation the Controller Link
   currently relies on. `ns2_bt_classic_trust_present()` documents that an LE
   bond can already legitimately exist with no Classic key and admits the
   companion on live session trust, so this may be survivable — but it is a
   product behaviour change requiring hardware validation, not a migration step.

No option was implemented. The migration is deliberately clean.

## SDK 2.3.0 host-side changes worth knowing about

Not modified — recorded so post-migration behaviour is interpreted correctly.

- **`async_context_threadsafe_background.c` alarm fix.** SDK 2.2.0 could leave
  `alarm_pending` set when cancelling an alarm on an end-of-time path, latching
  out later alarm scheduling. PicoSwitch compiles this implementation. 2.3.0
  fixes it. **Plausibly relevant to the unresolved LSTO; not a proven cause.**
- **SDK commit `2a1d5009`** removed a re-entrant BTstack callback fired from
  inside `send_packet` while `CYW43_THREAD_ENTER` was held — a host/controller
  servicing and locking boundary. **Plausibly relevant; not a proven cause.**

No workaround was added for either. PicoSwitch uses the stock 2.3.0 integration.

Also relevant if reliability is measured later: BTstack 1.7 changed HCI
connection collision handling, ignores Page Timeout after an incoming connection
event, and uses Classic page scan repetition mode R1; BTstack 1.8.2 avoids
sending HCI Authenticate a second time in some cases and sequences LE Link Layer
commands. These are credible reasons modernization *might* affect existing
reliability problems. They are not evidence that it did.

## Validation performed

All software. No hardware.

| Check | Command | Result |
| --- | --- | --- |
| Clean build, both boards | `./build.ps1 -Clean` | pico_w and pico2_w OK |
| Compiler diagnostics | build log | no warnings in PicoSwitch, SDK, or BTstack sources |
| Host test suite | `pwsh -File tools\run_host_tests.ps1` | 72/72 declared active targets rebuilt from source and passed; 9 sources classified outside the suite |
| Dependency contract | `python tools\test_btstack_dependency_contract.py` | passed |
| Management boundary suites | the four suites in `tools/run_mgmt_tests.ps1` | passed |
| General Python suites | `test_ns2_trace`, `test_ns2_nfc_semantics`, `test_amiibo_corpus` | passed |
| Install-reset marker | `tools/verify_install_reset_marker.py` on both `.bin` | marker present on both |
| Whitespace | `git diff --check` | clean |

### Warnings

The only compiler warnings in the whole build are in vendored `third_party/opus`:
43× the `#warning` pair in `celt/float_cast.h` (emitted whenever `HAVE_LRINTF` is
undefined — toolchain-independent) and 2× `-Wstringop-overflow=` in
`silk/decode_indices.c`.

The `-Wstringop-overflow=` pair was **verified not to be a GCC 15 regression**:
recompiling that exact file with the same flags under Arm GNU 14_2_Rel1 produces
byte-identical diagnostics. It is a range-analysis limitation on
`indices.GainsIndices[i]` / `indices.LTPIndex[k]` in the SILK *decoder*, which
this firmware never reaches — the DualSense audio path uses the CELT encoder
directly. No suppression was added and no vendored source was modified.

### Regression tests added

- `tools/test_ns2_bt_lifecycle.c` —
  `test_disconnect_convergence_after_btstack_1_8()` pins which
  `gap_disconnect()` statuses still leave a completion event in flight, including
  that `COMMAND_DISALLOWED` must not trigger local convergence and that an
  unrecognised status fails safe.
- `tools/test_btstack_dependency_contract.py` — eight structural guards for the
  decisions above, chosen because in each case the wrong answer still compiles:
  the HIDS Host pool macro, the removed protocol-mode enumerator, the HID Host
  send guard and its CMake substitution, the explicit LE/Classic security policy,
  the disconnect convergence wiring, agreement between the two SDK version
  declarations, the canonical `BTSTACK_ROOT` resolution and skew check, and stale
  tool paths in `.vscode`.
- `tools/test_bluetooth_closeout_wiring.py` was updated, not weakened: it still
  pins that the Controller Link teardown is a plain ACL disconnect ordered after
  the binding is cleared, and now additionally asserts that no second release
  mechanism appears in that function.

## Firmware candidate

Built from source HEAD `2272cb5e6eae` with the Stage C changes uncommitted, so
the embedded build identity is `2272cb5e+dirty`.

| Board | Path | Configuration |
| --- | --- | --- |
| Pico 2 W (RP2350) | `build/pico2_w/PicoSwitchWGA-pico2_w.uf2` | NS2_PRO, DS5 audio ON, 300 MHz, UART diag ON, management ON. Exercises the HID Host shim. |
| Pico W (RP2040) | `build/pico_w/PicoSwitchWGA-pico_w.uf2` | NS2_PRO, audio OFF, UART diag ON, management ON. Uses upstream `hid_host.c`. |

## Original hardware-validation checklist and remaining gates

At the build-only closeout, none of the checks below had been performed. The
later endurance observation above supplies early management, pairing, and
Controller Link evidence, but it does not manufacture results for the remaining
parity or release gates. Functional parity comes before interpreting a broad
migration as a complete fix.

Before any of it, decide the CTKD question above — steps 3, 4, 5, 7 and 13 are
the ones that would expose the link-key corruption, and a bond wipe midway
through would mask it.

1. Boot / UART sanity (`btstate`, build identity)
2. USB enumeration on the expected personalities
3. BLE management pairing and re-pairing — **the first real test of the Secure
   Connections decision**; an existing bond must still re-encrypt
4. Management commands and a full Refresh
5. Management reconnect after an ordinary app close/reopen
6. Physical controller input
7. Android Controller Link
8. Touch Gamepad / normalized virtual input path
9. Rumble and other output
10. Motion where applicable
11. DualSense long-report/audio path — exercises the ported HID Host shim
12. Clean management-loss → Controller Link teardown invariant
13. Ordinary reconnect

Watch `disc.already_gone` in `btstate` throughout. It should stay at 0; a
non-zero value means a record outlived its ACL and is worth investigating on its
own merits.

Only after all of that should reliability be compared against the established
BLE LSTO / reason `0x08` failure — and that first comparison should be bounded
and evidence-rich, not a 500-cycle soak.

## Relationship to the unresolved BLE LSTO investigation

None of this is a fix, and it is not being claimed as one.

The failure recorded in
[`established-management-gatt-failure-2026-08-25.md`](established-management-gatt-failure-2026-08-25.md)
— an established BLE management session lost with HCI reason `0x08`, Android BQR
reporting *Approaching LSTO* beforehand, negotiated LE supervision timeout 5 s,
reproduced with Classic Controller Link absent, strong RSSI — still has no
identified initiating mechanism.

Stage C is *technically capable* of affecting it, because it replaces the entire
host-side stack including the async-context alarm path and the CYW43 servicing
boundary. It is equally capable of not affecting it at all: the CYW43439's own
Bluetooth firmware is byte-identical, so a controller-side defect would survive
this migration untouched.

All existing instrumentation, failure classification, and diagnostics were
preserved so the comparison can actually be measured.

**Current Bluetooth reliability status: the pre-C failure has not reproduced on
the Stage C candidate after more than three hours of ongoing initial endurance,
while deliberate pre-C retesting reproduced it repeatedly within roughly 2-14
minutes. Material improvement is observed; permanent resolution and the exact
responsible host-side change are not proven.**
