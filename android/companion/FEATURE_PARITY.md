# Android companion feature parity

Evidence snapshot: 2026-08-13. Firmware source and `web/index.html` are authoritative. “Awaiting
hardware” means the workflow is implemented and host/emulator-testable but has not run against a
real PicoSwitch2/AYN Thor.

| Capability | Android status | Complete workflow / rationale |
|---|---|---|
| App appearance themes | Fully implemented client-side | Settings -> Appearance persists System, Light, Dark, or true OLED black plus a default, Joy-Con 1-inspired, or Joy-Con 2-inspired accent palette. Choices are labeled radio options and affect only app UI, never firmware identity colors. |
| BLE discovery and connection | Thor hardware-confirmed | AYN Thor connected in-app by service UUID and populated the management UI. A timeout or oversized reply closes the session. |
| Adapter/firmware information | Awaiting hardware | `info`, `get`, `device` are required core probes. Incomplete shapes and a non-`picoswitch` identity fail closed. |
| Controller information/battery | Thor hardware-confirmed | `device` populated the Home controller card. Firmware now clears identity on disconnect and Android polls it every five seconds; the fix needs a firmware flash to validate the powered-off transition. |
| Personality query/switch | Thor hardware-confirmed | AYN Thor successfully changed personality. Set requires `{ok:true}`, marks USB identity refresh pending, and never replays after rotation. Personality persistence is firmware-owned. |
| Body/lightbar and Joy-Con accent colors | Awaiting hardware | `body`/`jcl`/`jcr` -> `{ok:true}` -> `save` -> local snapshot. UI records the current firmware requirement for a later USB re-enumeration. |
| Controller mappings | Intentionally omitted | Current firmware intentionally exposes no remapping schema. Nintendo-side persistent remapping remains authoritative. |
| Console wake | Awaiting hardware | `wake` -> `{ok:true}` -> queued notice. An explicit unsupported response disables the capability for the session. Wake success is asynchronous and is not falsely claimed. |
| In-band management gate | Awaiting hardware | Standard firmware boots on. Optional `mgmt status` probes the gate; `mgmt on/off` refreshes state. Off is RAM-only, may close the current session, and reboot restores on. New bonds require the physical pairing window. |
| LE management bonds | Host-tested bounded pagination | `bonds list` returns a backward-compatible v2 envelope when complete, or compact `response_too_large` and `bonds list v2 [cursor]` pages when the list exceeds 511 payload bytes. Android follows `next`, verifies totals/no duplicates, and hides legacy/unversioned partial results rather than claiming completeness. Classic HID bonds remain physical-wipe-only. |
| Security state | Host/build implemented; hardware pending | Firmware requires a durable LE bond plus active 16-byte ATT encryption and gates first bond on the physical pairing window. Android Just Works has no MITM and is not called authenticated. Android adds no raw command UI or cloud surface; the bounded user-selected library ZIP excludes keys and adapter state. |
| Amiibo import | Fully implemented | SAF one-shot read, maximum 2048 bytes, accepted sizes 540/572/2048, ordinary BCC normalization matching the portal, structural validation, private transactional copy, exact-content duplicate detection. |
| Local Amiibo library | Fully implemented | Versioned private index, stable UUID filenames, atomic replacement, restart load, orphan recovery, corruption/mismatch warnings, rename, confirmed delete, collision-safe names, and no Android/cloud backup. |
| Amiibo upload/load | Awaiting hardware | status/dirty guard -> begin -> 32-byte chunks -> commit -> queued persist -> poll until verified -> refresh. Failure attempts `cancel`; no optimistic success. |
| Amiibo selection | Fully implemented locally and for the active adapter tag | Selected UUID/navigation/source survive rotation and process recreation. An adapter figure ID is now a first-class display/catalog key even when `library.json` has no local items; selection alone does not mutate the adapter. |
| Present/eject | Awaiting hardware | `present`/`eject` -> `{ok:true}` -> `amiibo status` refresh. Adapter-only state remains actionable even before a matching local backup exists. |
| Clean/used copy selection | Awaiting hardware | Shown only for ordinary images with `hasSave2`; `select save1/save2` -> refresh. It is not shown for v3. |
| Sync console-written data | Thor hardware-confirmed | The fixed APK downloaded and completed the same 540-byte adapter Sync with no CRC/error state. Android treats only ordinary zero as unavailable, retains structure/generation checks, and keeps strict v3 CRC verification. Dirty protection is never acknowledged before local durability. |
| Adapter Amiibo clear | Awaiting hardware | Dirty state blocks clear. Confirmed UI -> `clear` -> poll until no image/persist pending. Local backup is retained. |
| Local delete/rename | Fully implemented | Confirmation/rename dialog -> transactional private index/file mutation. Adapter state is untouched. |
| Amiibo metadata identity | Implemented locally | The page now shows UID/figure ID plus character game code/variant, tag type, model/series, format version, extended variant, size, and CRC from the raw local image. |
| Owner/nickname/dates/write count | Implemented with optional local keys | The portal-tested amiitool-compatible decrypt/read path accepts a validated 160-byte user key file from the Amiibo overflow menu. HMAC-invalid images expose no decrypted fields; keys remain app-private and are never sent, logged, or exported. |
| Amiibo initialization | Implemented locally; awaiting hardware | Explicit confirmation wipes/re-signs an imported ordinary or figure-v3 image with the user-owned portal-compatible `key_retail.bin`, self-verifies both HMACs, clears the captured v3 save ranges, and atomically replaces only the private phone copy. It never sends keys or initialized bytes to the adapter. |
| Catalog artwork/games | Implemented as optional cache-first enrichment | AmiiboAPI `?showgames` metadata is matched by the portal's uppercase head+tail figure ID, cached privately for seven days, bounded to 4 MiB and two short-timeout mirrors, and never gates local import or adapter operations. Friendly name/character/series/type/release, compatible games, title-ID game labels, and best-effort artwork appear in the responsive library hero/cards; adapter-only tags also show loading, offline, or unmatched state with raw identity fallback. |
| ZIP library exchange | Implemented locally; awaiting hardware | SAF export/import uses the portal's v3 `library.json` plus flat `.bin` archive shape. Android bounds archive bytes, entries, image totals, manifest size, and filenames; it validates every image before replacing the private library transactionally. Keys and adapter state are excluded. |
| Phone NFC physical-tag backup | Host-tested; physical gate pending | Amiibo -> **Scan** arms a foreground one-shot `NfcA` reader and runs an Android-independent strict NTAG215 sequence: exact `GET_VERSION` (`00 04 04 02 01 00 11 03` reply), 33 four-page `READ`s from `00` through `80`, `FAST_READ 3A 84 86`, and optional exact 32-byte `READ_SIG`. It persists only a complete 540/572-byte image after raw manufacturer/BCC validation; NTAG213/216, figure-v3/NTAG I2C 2K, malformed responses, and all writes/auth/NDEF/sector/adapter commands are rejected or absent. Physical NFC hardware validation remains open. |
| Android controller HID bridge | Thor in-game hardware pass | Thor live input, public HID registration, app-led bonding, Pico receipt, and working in-game console input are confirmed. Callback-authoritative register/connect handling covers this OEM's misleading immediate `false`. Persisted Auto/Nintendo/Xbox face-label normalization is implemented; corrected labels, latency, lifecycle teardown, and saved reconnect remain physical gates. No root/Shizuku is used. |
| Active controller switching | Host/build implemented; hardware pending | Firmware returns a bounded registry and accepts `input active <id|none>` from the bonded/encrypted management link. Android presents connected sources as one Active controller choice. The existing arbiter neutralizes immediately, commits at a report boundary, and waits for fresh state; it never merges sources or silently falls through after disconnect. Physical coexistence/latency/feedback validation remains open. |
| Adapter relationship / reconnect | Implemented; awaiting rebuilt-APK hardware | First use says **Pair Adapter** and uses Android's required companion chooser/bond consent. The saved address/association drives direct management reconnect, with service-filtered scan fallback after either transport failure or identity mismatch and a fresh bounded attempt on each foreground entry. Disconnect clears adapter-derived UI state. Foreground HID controller mode reuses the relationship without a second chooser; underlying CDM, Classic bond, BLE GATT, and HID registration remain distinct diagnostic states. |
| Motion/rumble/LED/battery from Android | Implemented (v2 contract); host-tested, hardware gate open | The bridge descriptor is now v2: input report 1 appends a vendor block carrying gyro/accel (`int16`, 8192 counts/g and 16.384 counts/dps), battery level/charging, and a motion timestamp; new output report 2 carries rumble left/right, the console player number, and a motion-wanted flag. The v1 field offsets are byte-identical, so the hardware-validated input path cannot regress and either side may be older. Firmware identifies the bridge by an exact descriptor match (not phone VID/PID) and routes motion through its own `SWITCH_MOTION_SOURCE_ANDROID` seam row. Sensors register only while the console actually negotiates its IMU, so an idle handheld is not drained. 27 host assertions cover motion/battery ingest, feedback encode/retry, and v1 non-regression; `tools/check_android_descriptor_parity.py` pins the C/Kotlin descriptors byte-for-byte. Open: motion axis/sign row is reasoned, not measured, and rumble/LED/battery have not run on a physical Thor. |
| Microphone / game audio from Android | **Closed — will not be implemented** | `BluetoothHidDevice`, the only no-root Android path, carries HID reports and has no audio channel, so the bridge can never carry audio. The only transport with enough bandwidth for PCM would be WiFi, which is **permanently prohibited on this firmware** by maintainer decision. With both true there is nothing left to evaluate, so this is closed rather than deferred. (The adapter's audio path is *transmitting* to the DualSense; it has no receive-audio-from-a-wireless-device path.) See `docs/bluetooth/android-audio-feasibility-2026-08-13.md`. |
| Developer diagnostics/export | Fully implemented client-side | Settings -> Developer reports platform permissions/profile/GATT/firmware/capabilities/descriptor/bond/report/identity state and exports a bounded redacted text report through a cache-only FileProvider. |
| Research/raw commands | Deprecated for this client | Firmware diagnostics and low-level Amiibo reader commands remain UART/USB research surfaces and are not exposed. |

## Portal workflow trace findings

- Every adapter mutation is serialized by the transport and requires either `{ok:true}` or its
  documented typed response. Recomposition never sends commands.
- Refresh follows personality-visible, present/eject, save-copy, upload, Sync, clear, bond-remove,
  and connect workflows. Color state is updated only after both color and `save` succeed.
- Unlike the portal, Android closes the GATT session after timeout/oversize so an uncorrelated late
  reply cannot satisfy the next command.
- Unlike the portal, Sync waits for adapter persistence and acknowledges dirty data only after the
  private local copy is durable and the generation/CRC still match.
- While connected, Android polls controller and `amiibo status` every five seconds outside active
  transactions so disconnect/present/dirty/generation changes do not leave the screen indefinitely stale.
- Offline local selection, rename, import, and delete never issue management requests.

## 511-byte firmware response constraint

The command bridge permits at most 511 response bytes plus newline. Current fixed-shape responses
(`info`, `get`, `device`, personality, management, Amiibo status/read, acknowledgements) are bounded
below that ceiling. Bond enumeration now uses the same explicit boundary: a complete v2 envelope for
small lists, compact `response_too_large` for the historical unpaged spelling when necessary, and
`bonds list v2 [cursor]` pages with `next:null` as the completion marker. Android follows every page,
checks the reported total and duplicate indices, and closes the capability as unknown for a legacy
unversioned response. If any unrelated response reaches bridge rejection, firmware also emits the
same compact error where the session is still valid; no partial JSON is accepted.
