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
| In-band management gate | Awaiting hardware | Optional `mgmt status` probe gates the switch; `mgmt on/off` refreshes state. It is RAM-only and disabling it may close the current session. |
| LE management bonds | Partial / firmware-limited | `bonds list/remove` is implemented. Malformed, timeout, or >511-byte replies leave capability unknown. Current firmware can also stop adding entries and return valid JSON without a total/truncated marker, so Android labels non-empty results as reported rather than provably complete. Classic HID bonds remain physical-wipe-only. |
| Security state | Fully represented | UI and diagnostics state that firmware writes are unauthenticated. Android adds no raw command UI, key, or cloud surface; the bounded user-selected library ZIP excludes keys and adapter state. |
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
| Android controller HID bridge | Thor in-game hardware pass | Thor live input, public HID registration, app-led bonding, Pico receipt, and working in-game console input are confirmed. Callback-authoritative register/connect handling covers this OEM's misleading immediate `false`. The next APK adds persisted Auto/Nintendo/Xbox face-label normalization; corrected labels, latency, lifecycle teardown, and saved reconnect remain physical gates. No root/Shizuku is used. |
| Adapter relationship / reconnect | Implemented; awaiting rebuilt-APK hardware | First use says **Pair Adapter** and uses Android's required companion chooser/bond consent. The saved address/association drives direct management reconnect (service scan fallback) and foreground HID controller mode without a second chooser. Underlying CDM, Classic bond, BLE GATT, and HID registration remain distinct diagnostic states. |
| Motion/rumble from Android | Unsupported by v1 contract | The fixed Android bridge descriptor is input-only and contains no motion or output reports. |
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
below that ceiling. `bonds list` is the only current variable-cardinality response demonstrated to
approach it. Firmware stops appending entries before its 512-byte JSON buffer is exhausted and does
not include `total`, `truncated`, or `nextOffset`; the result can therefore be valid JSON yet silently
incomplete. If a different response reaches bridge rejection, firmware currently drops it and the
client sees a timeout. No compatible pagination command is defined. Firmware should add an explicitly
versioned bounded page (or completeness metadata) and return a compact `response_too_large` error when
publish fails. Until then Android rejects malformed/oversized replies and visibly warns that a valid
non-empty bond list is only the set firmware reported.
