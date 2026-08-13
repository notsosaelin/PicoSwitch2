# Android companion feature parity

Evidence snapshot: 2026-08-13. Firmware source and `web/index.html` are authoritative. “Awaiting
hardware” means the workflow is implemented and host/emulator-testable but has not run against a
real PicoSwitch2/AYN Thor.

| Capability | Android status | Complete workflow / rationale |
|---|---|---|
| BLE discovery and connection | Thor hardware-confirmed | AYN Thor connected in-app by service UUID and populated the management UI. A timeout or oversized reply closes the session. |
| Adapter/firmware information | Awaiting hardware | `info`, `get`, `device` are required core probes. Incomplete shapes and a non-`picoswitch` identity fail closed. |
| Controller information/battery | Thor hardware-confirmed | `device` populated the Home controller card. Firmware now clears identity on disconnect and Android polls it every five seconds; the fix needs a firmware flash to validate the powered-off transition. |
| Personality query/switch | Thor hardware-confirmed | AYN Thor successfully changed personality. Set requires `{ok:true}`, marks USB identity refresh pending, and never replays after rotation. Personality persistence is firmware-owned. |
| Body/lightbar and Joy-Con accent colors | Awaiting hardware | `body`/`jcl`/`jcr` -> `{ok:true}` -> `save` -> local snapshot. UI records the current firmware requirement for a later USB re-enumeration. |
| Controller mappings | Intentionally omitted | Current firmware intentionally exposes no remapping schema. Nintendo-side persistent remapping remains authoritative. |
| Console wake | Awaiting hardware | `wake` -> `{ok:true}` -> queued notice. An explicit unsupported response disables the capability for the session. Wake success is asynchronous and is not falsely claimed. |
| In-band management gate | Awaiting hardware | Optional `mgmt status` probe gates the switch; `mgmt on/off` refreshes state. It is RAM-only and disabling it may close the current session. |
| LE management bonds | Partial / firmware-limited | `bonds list/remove` is implemented. Malformed, timeout, or >511-byte replies leave capability unknown. Current firmware can also stop adding entries and return valid JSON without a total/truncated marker, so Android labels non-empty results as reported rather than provably complete. Classic HID bonds remain physical-wipe-only. |
| Security state | Fully represented | UI and diagnostics state that firmware writes are unauthenticated. Android adds no raw command UI, network, key, cloud, or exported write surface. |
| Amiibo import | Fully implemented | SAF one-shot read, maximum 2048 bytes, accepted sizes 540/572/2048, ordinary BCC normalization matching the portal, structural validation, private transactional copy, exact-content duplicate detection. |
| Local Amiibo library | Fully implemented | Versioned private index, stable UUID filenames, atomic replacement, restart load, orphan recovery, corruption/mismatch warnings, rename, confirmed delete, collision-safe names, and no Android/cloud backup. |
| Amiibo upload/load | Awaiting hardware | status/dirty guard -> begin -> 32-byte chunks -> commit -> queued persist -> poll until verified -> refresh. Failure attempts `cancel`; no optimistic success. |
| Amiibo selection | Fully implemented locally | Selected UUID/navigation/source survive rotation and process recreation. Selection alone does not mutate the adapter. |
| Present/eject | Awaiting hardware | `present`/`eject` -> `{ok:true}` -> `amiibo status` refresh. Adapter-only state remains actionable even before a matching local backup exists. |
| Clean/used copy selection | Awaiting hardware | Shown only for ordinary images with `hasSave2`; `select save1/save2` -> refresh. It is not shown for v3. |
| Sync console-written data | Thor hardware-confirmed | The fixed APK downloaded and completed the same 540-byte adapter Sync with no CRC/error state. Android treats only ordinary zero as unavailable, retains structure/generation checks, and keeps strict v3 CRC verification. Dirty protection is never acknowledged before local durability. |
| Adapter Amiibo clear | Awaiting hardware | Dirty state blocks clear. Confirmed UI -> `clear` -> poll until no image/persist pending. Local backup is retained. |
| Local delete/rename | Fully implemented | Confirmation/rename dialog -> transactional private index/file mutation. Adapter state is untouched. |
| Amiibo metadata identity | Fully implemented | UID, figure ID, size, CRC and generation are shown from local/firmware evidence. |
| Owner/nickname/dates/write count | Intentionally deferred | Requires local Amiibo crypto and user-supplied retail keys. Keys must remain phone-local; firmware should never receive them. |
| Amiibo initialization | Intentionally deferred | Portal initialization is client-side decrypt/reset/re-sign behavior. It is not a firmware command and needs the same local crypto/key work above. |
| Catalog artwork/games/ZIP exchange | Web-Portal-only candidate | Useful client-side features, but not needed for protocol-complete hardware validation and not generalized in this pass. No network/catalog dependency gates import or Sync. |
| Phone NFC physical-tag backup | Intentionally deferred | Public Android NFC is feasible but is separate from management. Low-level controller-reader commands are deliberately not exposed. |
| Android controller HID bridge | Partial Thor hardware pass | Thor live input and public HID registration reach Ready. The missing Companion Device Manager manifest feature that crashed pairing is fixed and guarded. A legacy VCC root daemon currently competes for Android's one HID Device app slot; app-led bond, Pico receipt, and console input remain. No root/Shizuku is used by this app. |
| Motion/rumble from Android | Unsupported by v1 contract | The fixed Android bridge descriptor is input-only and contains no motion or output reports. |
| Developer diagnostics/export | Fully implemented client-side | Settings -> Developer/diagnostics reports platform permissions/profile/GATT/firmware/capabilities/descriptor/bond/report/identity state and exports a bounded redacted text report through a cache-only FileProvider. |
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
