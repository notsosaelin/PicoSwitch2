# PicoSwitch2 Status

Current-state snapshot: what is true about PicoSwitch2 right now.

Accepted future work belongs in [`PLAN.md`](PLAN.md). Evidence, protocol detail, and experiment
records belong under [`docs/`](docs/README.md). User-visible release history belongs in
[`CHANGELOG.md`](CHANGELOG.md). Narrative history through 2026-07-15 is archived in
[`docs/archive/status-through-2026-07-15.archived.md`](docs/archive/status-through-2026-07-15.archived.md).

- **The management carrier stopped answering while the adapter looked for a controller. Fixed
  2026-08-31; REQUIRES A REFLASH; hardware smoke test pending.** A KB/M resident upload died on
  `kbm draft bind key:1D a` after 10 000 ms, the app could not reconnect without being
  force-closed, and one run escalated to "identity changed / repair dongle" on a pairing that a
  power cycle then proved was fine.
  **The tell was `reply-timeout` rather than `command-failed`.** Windows writes with response and
  turns any ATT error into a retired session, so the write had SUCCEEDED and no answer came — which
  eliminates the one-slot bridge's BUSY drop, the obvious suspect, because that returns an ATT
  error the client can see. Replaying the entire staged transaction through the real dispatcher over
  the UART `cfg` bridge cleared the firmware half in one pass: begin, four binds, six mouse fields,
  commit and readback, ~10 ms each, profile correctly resident. So the command was fine and the
  ANSWER was missing.
  **`att_server_request_to_send_notification` had one call site, and it sat below
  `if (hid_state.state == BLE_STATE_CONNECTING) return;`** — a guard whose own comment says it
  exists to avoid perturbing advertiser arbitration. `BLE_CONNECT_TIMEOUT_MS` is 10 000 and the
  companion's per-command budget is 10 000, so a single overlapping controller connect attempt was
  sufficient to guarantee a timeout, and `btstack_host.c` already documents a ~50 s worst-case
  cascade. `config_ble_start_advertising()` sits below the same return and `config_ble_handle_disconnect()`
  deliberately does not advertise itself, so the SAME return also left the adapter undiscoverable
  after the link dropped — which is what produced two `Unreachable` results and the false repair
  diagnosis. **One defect, three symptoms, and it can only happen with no controller connected**,
  which is exactly the state the failure log reported (`controller=No controller peers=0`).
  `config_ble_pump_response()` now runs BEFORE that return and is the sole arming site; the
  advertiser arbitration is deliberately unchanged. `cble.tx_wait_max_ms` in `btstate` now measures
  publish-to-notified latency, because nothing previously distinguished "the command never arrived"
  from "the answer was never sent".
  **Three client defects made it worse and are fixed with regressions.** The relationship stayed
  `Connected` with no carrier beneath it and `RequestReconnect` is inert while Connected, so only
  killing the process cleared it — `ReconcileCarrierLoss()` returns it to Idle, touching neither
  registry, pairing nor local library. `AdapterResetSignature` counted an `Unreachable` at any stage
  but `Connect`, including a command on a session whose CCC write had already succeeded over
  encrypted handles — `TransportTrustSnapshot.BondProven` now disqualifies the signature outright,
  and the genuine stale-bond path is untouched because a stale bond fails below the attribute layer
  and can never set it. And the page printed "'X' is now Profile 1 on the adapter" over the error
  banner, because Assign used the void `SafeAsync` overload; it now returns early on failure, and
  the repository verifies the readback by CONTENT FINGERPRINT before returning at all.
  `kbm draft bind`/`mouse` joined the repeatable allowlist on both clients — absolute writes into a
  RAM draft — as defence against the BUSY drop, explicitly not as the fix.
  **New end-to-end coverage.** `KbmResidentUploadTests` drives the real service, repository and
  client for a realistic 12-override profile and pins the ORDERED command sequence in full, so a
  transaction that gains, loses or reorders a command fails and names the difference. That is the
  gap the 2026-08-30 pagination defect escaped through: every builder was unit-tested and each was
  right about a contract that was wrong.
  Host suite 79/79; 4 Windows suites green (123/161/55/524); Android green; parity, closeout-wiring
  and BTstack contract green; both boards build clean.
  [`docs/experiments/kbm-resident-upload-notify-stall-2026-08-31.md`](docs/experiments/kbm-resident-upload-notify-stall-2026-08-31.md)

- **Virtual Amiibo (Phase 5) complete 2026-08-31. Android transfer hardware-confirmed; the
  console-facing half is not.** The library gained a shared interaction model, and the Amiibo
  transport gained four fixes that between them turned a transfer that stalled and dropped the
  adapter into one that completes.
  **The interaction model** separates three ideas one property used to carry: which card is
  focused, which one's details are open, and which are selected for a bulk command. Single tap
  browses, double tap inspects, long press selects; browsing owns the whole surface until details
  are explicitly asked for. `AmiiboInteractionState` is mirrored in C# and Kotlin with 36 parity
  properties asserted on each side, and is intended to govern any future touch-capable client —
  [`docs/architecture/companion-library-interaction.md`](docs/architecture/companion-library-interaction.md).
  **The transport.** Uploads stalled at arbitrary offsets (64, 96, 288 of the same file) and took
  the management session with them. Arbitrary offsets were the tell: a framing fault stops in the
  same place, a scheduling one does not. A UART upload of the same tag completing in 1.3 s cleared
  the firmware's begin/chunk/commit path and localised it to the BLE carrier. Four causes, all
  client-side: transactions ran on `Dispatchers.Main`, so a command's 10 s budget measured how busy
  the UI was; commands were fragmented to 20 bytes although the negotiated MTU was 517 and the
  firmware accumulates to a newline regardless, making an 81-byte chunk five writes instead of one;
  a command dropped by the adapter's one-slot bridge when busy was never retried, and the timeout
  path invalidates the session, so one drop ended a transfer and disconnected the adapter
  (**corrected 2026-08-31:** that drop is not silent — the ATT write returns
  `INSUFFICIENT_RESOURCES` and both clients write with response, so it surfaces as a GATT
  protocol error, not a timeout. The distinction is load-bearing: it is what excluded this
  mechanism as the cause of the resident-upload stall below); and every visible library tile recomposed on every progress tick,
  because the tiles took a parameter Compose could not treat as stable. Windows had the same
  fragmentation and retry defects **plus no turnaround policy at all**, which is consistent with it
  stalling more often than Android. The carrier policies now live in `Management.Core` as mirrors of
  the Kotlin ones.
  **Measured**, layout lab at 1200 entries, scrolling: janky frames 12.77% → 4.52%, legacy jank
  34.04% → 0.00%, 99th percentile 81 ms → 18 ms. Artwork gained a byte-bounded memory cache, a disk
  cache and request coalescing; a relaunch in airplane mode draws the whole library from disk.
  **Hardware-confirmed:** an Android upload to the adapter completing, and a 540-byte UART upload in
  1.3 s. **Not confirmed:** the console reading a tag uploaded this way, a console-written tag
  syncing back, and the Windows carrier fixes — those were validated by tests and build only.
  Two measured follow-ups remain open, both in [`PLAN.md`](PLAN.md): the 100 ms inter-command
  turnaround (≈6.4 s of pure delay on a 2048-byte v3, against a documented 1–3 ms hazard window),
  and raising `AMIIBO_CHUNK_BYTES` from 32 to 48, which is the largest that still fits the
  firmware's 128-byte command buffer.

- **Windows Virtual Amiibo (Phase 5) implemented 2026-08-31; hardware validation outstanding.**
  The Windows companion now has the whole Amiibo surface: a local backup library, the
  portal-compatible ZIP exchange, upload with progress and cancel, present/eject, save1/save2,
  CRC-verified sync, clear, DPAPI-protected key storage, and decrypted register metadata.
  **The precondition came first.** `AmiiboCrypto` already existed twice — Kotlin and
  `web/index.html` — and its failure mode is silent: a wrong HMAC produces a tag the console simply
  rejects, with nothing to read. A C# port would have been a third copy verified against nothing, so
  `tools/fixtures/amiibo/crypto-vectors.json` was built first, generated from the Kotlin
  implementation over real dumps. 14 vectors and 2 negative ones now cover both tag sizes and both
  register states; coverage is DERIVED from the vectors so it cannot go stale, and crossing tag type
  with register state immediately exposed a gap the per-axis view had hidden (no set-up NTAG215 tag,
  still open). The fixture carries **no key material** — the key set is a SHA-256 fingerprint — and
  **no personal data**: every set-up tag in the corpus has a real Mii name, so names are recorded as
  digests, which is a complete equality check that discloses nothing.
  Running the C# suite with and without the key gave the same green result, which is the shape of a
  suite that is not running; a deliberate broken-expectation probe confirmed the decrypting half
  genuinely executes, and that hazard is now closed permanently — a set-but-unusable key path throws
  rather than silently disabling half the file.
  **The rule the page is built around:** a game writing to a tag produces a change that exists only
  on the adapter, so uploading over it and clearing it are refused until it is synced, and sync
  writes the library BEFORE acknowledging — acknowledging first would clear the adapter's dirty flag
  on data saved nowhere.
  4 Windows suites green (123/146/52/412); descriptor parity green. **Not validated on hardware:**
  a tag uploaded from Windows being read by the console, and a console-written tag syncing back.
  [`WINDOWS_PASS.md` §31 Phase 5](WINDOWS_PASS.md)

- **KB/M profile system shipped end-to-end 2026-08-30; REQUIRES A REFLASH; hardware validation
  pending.** Firmware, management protocol, Windows and Android in one pass. Two defects are fixed.
  **(1)** The mapping a keyboard resolved against was **derived** from which peer roles happened to
  be filled, with no `kbm profile` command anywhere — which is how a user could bind `key:05 → B`,
  watch every operation report success and read back correctly, and get nothing at the console.
  **(2)** The editor wrote `kbm bind` per keystroke, erasing flash once per changed key and making
  Save and Discard impossible to offer, because the change had already happened.
  Five concepts are now separate: **runtime mode** (which peers may own the console), **layout**
  (keyboard, or keyboard and mouse — derived from admitted roles, never a user assertion about
  hardware), **template** (immutable ROM; one Default per layout, consuming no slot), **saved
  profile** (six CUSTOM, named, user-selected), and the **active realized mapping** (a per-layout
  SNAPSHOT of content). `ns2_kbm_resolve()` reads only the snapshot, which is what makes
  **Save ≠ Apply structural** rather than a UI convention: saving the profile that is currently
  applied stores it and leaves the console running the old behaviour until Set Active. A pointer
  into a mutable slot could not express that state at all. Stable profile ids survive slot reuse so
  a cached draft cannot rebind to an unrelated mapping; revisions make a stale save a **conflict**,
  never a silent overwrite; an FNV-1a fingerprint over canonicalized content — identical in C, C#
  and Kotlin — answers "is what I saved what is running?" without trusting a local flag and lets
  Apply skip a flash write when content is already realized. Mouse sensitivity, inversion and
  anti-deadzone are **profile-owned**; velocity/idle constants stay transport properties. Saving is
  one staged transaction (`kbm draft begin|bind|mouse|commit|abort`) because a loop of per-binding
  writes is not atomic — a disconnect halfway leaves half of one mapping and half of another.
  Editing 30 controls costs **0** flash writes; Save 1; Apply 1, or 0 when already realized.
  Legacy `kbm map/bind/reset/mouse` keep their exact meaning — they act on the layout's realized
  mapping immediately — and the resulting divergence from the saved profile is reported truthfully
  instead of a client claiming the profile is still applied. `CONFIG_RECORD_BYTES` widened
  1024 → 2048 after verifying the flash map: the config sector is ours alone, the erase already
  covers all 4096 bytes whatever the value is, and there is **no A/B record whose atomicity is
  compromised**. Record is 1888 of 2048, compile-time asserted. Migration v13 → v14 keeps a
  customized mapping as a named profile (`Current Keyboard` / `Current KB + Mouse`) and realizes it,
  so nothing about the user's console changes on upgrade; an unmodified mapping becomes Default and
  consumes no slot; **the v13 management-companion table survives byte for byte**, with its own
  regression. The record remains single-bank and non-CRC: sanitize rejects malformed state and is
  explicitly **not** torn-write detection.
  **Two wire defects were found on hardware and fixed.** Both were the same mistake — a size
  assumption checked against the wrong constraint — and both are recorded in
  [`docs/experiments/kbm-wire-pagination-data-loss-2026-08-30.md`](docs/experiments/kbm-wire-pagination-data-loss-2026-08-30.md).
  **(1)** Adding the active-mapping identity pushed `kbm status` to 729 bytes worst case
  (559 measured on the adapter) against a 511-byte usable slot. An oversized reply is not
  truncated — the bridge substitutes `response_too_large` — so the whole read failed and the
  profile UI never rendered. Split into `kbm status` (318 B) and `kbm counters` (414 B).
  **(2)** The pagination added by that fix kept a **fixed `page * 8` offset** while emitting only
  as many rows as the byte budget allowed, so every page silently dropped its last row. Reproduced
  exactly against the content on the adapter: the Keyboard layout has 26 bindings and a client
  received 25 — index 7, `key:0F → rstick_right` (the longest destination name, so the row the
  budget cut), never sent. That is `Adapter returned an incomplete KB/M binding list`.
  **A fixed page size cannot be rescued by choosing a smaller constant** — any constant is either
  unsafe for the worst-case row or wasteful for the common one, and guessing wrong loses rows
  silently. `kbm map`, `kbm pmap` and `kbm profiles` now use **cursor pagination**: `next` is the
  index of the first item not in the reply, null exactly at the end, decided by the firmware, which
  alone knows how many rows it serialized.
  The read formatters moved to `src/ns2_kbm_commands.c` because `config.c` cannot compile on the
  host — which is why their pagination was covered only by hand-written client fixtures, in both
  C# and Kotlin, that agreed with the bug. `tools/test_ns2_kbm_commands.c` drives the real
  formatter (1077 checks) and generates `tools/fixtures/management/kbm-wire-corpus.json`, the exact
  firmware bytes, which the Windows and Android integration tests replay through their real
  clients. One authority for three implementations.
  **Pre-release companion fallbacks are gone.** `kbm counters`/`kbm profiles`/`kbm active` are
  required, not probed; a missing one gives **Firmware update required** naming the command, rather
  than degrading to a pre-profile editor or synthesizing zeroed counters — the old fallback is what
  made a protocol defect look like an unfinished app. Both companions now expose explicit page
  states (NotRead/Loading/Ready/FirmwareUpdateRequired/Error) and the editor exists only in Ready.
  Firmware config migration (v11/v12/v13 → v14) is unaffected and still supported.
  `cfg <command>` on the UART console now runs any management command and reports the size the
  wireless bridge would see, closing the gap that forced both defects to be diagnosed from source.
  **The fix has not yet run on hardware — it needs a reflash.**
  [`docs/architecture/kbm-profile-system-hld.md`](docs/architecture/kbm-profile-system-hld.md)

- **KB/M library and resident bank separated; Android brought to parity 2026-08-30; hardware
  validation pending.** The third defect in the same feature, and an architectural one: the
  companions treated the adapter's **six resident profiles as the user's library**. Six is how many
  the *adapter* can hold so it works with no app attached — not how many mappings a person may
  own. Conflating them made `New` a staged flash write that assigned itself to the working set,
  made `Save` change what the console might run, and made both impossible while disconnected, for a
  task that never needed a device.
  There are now **two stores**. The local library is unbounded, lives in the companion, and its
  New/Duplicate/Rename/Delete/Edit/Save/Discard send **zero** management commands — enforced
  structurally, because `KbmLibraryRepository` owns no transport and cannot acquire one. The
  adapter's bank is **3 positions × 2 layouts plus a built-in Default**, and only
  Assign/Update/Remove/Activate/On-startup reach it. Editing works offline because the firmware's
  canonical default table ships in both companions from
  `tools/fixtures/management/kbm-default-mappings.json`, with parity tests asserting it still
  matches what the firmware emits.
  Schema **14 → 16**: profiles gain a semantic `position` (what the user selects, resolved through
  the derived layout), plus one shared layout-free **switch-key table** so the profile can be
  changed with no app, and `boot_position[]` kept separate from the runtime choice — a switch key
  moves the second and not the first, and only the first costs a flash write. Removing a position
  that is running or is the boot choice falls that layout back to Default, so no reference dangles.
  Cross-platform hand-off rides on the **content fingerprint**: the two companions mint their own
  ids and never see each other's, so a resident copy is matched by content with the name as
  evidence. **Name outranks content alone** — content-first made an edited profile claim an
  unrelated resident it now coincidentally equalled and report itself safely stored, hiding the one
  fact that mattered. Found by the Android cross-platform scenario, fixed in both implementations.
  79/79 host, 663 Windows, 1505 Android JVM, lint clean, debug APK. No firmware source changed in
  this pass.
  [`docs/architecture/kbm-profile-system-hld.md`](docs/architecture/kbm-profile-system-hld.md)

- **BLE keyboard input — root-caused and fixed 2026-08-29; HARDWARE-CONFIRMED.** An 8BitDo
  2DC8:2028 keyboard paired, held the keyboard role, reported `keyboard=true`, and produced no
  console input whatsoever. Three investigation passes went to the transport, the role model and the
  runtime admission path; **all three were correct**, and the BLE HOGP report-ID theory in particular
  is disproven and must not be reopened (BTstack already supplies the id; prepending another would
  break every BLE HID device). The failing stage incremented no counter, which is why it hid.
  Adding one found it in a single command: `undecodedReports` reached 380 with every admission
  counter at zero. `bthid_keyboard_decode_report()` can only fail two ways — bad arguments, or a
  report id that is not THE one declared id — so 380 failures proved the parser had locked onto the
  wrong report. It followed exactly one keyboard report id by design; a keyboard commonly declares a
  boot-compatible 6-key report **and** a vendor NKRO report and the DEVICE picks which to send. Every
  declared keyboard report is now followed, each parsed field carries its report id so the arriving
  id selects the layout, and an undeclared id is still refused. Also fixed in the same region: the
  parser defined `HID_TAG_PUSH`/`HID_TAG_POP` and ignored them while this repository's own USB parser
  has always implemented them — a keyboard bracketing its LED block that way had its key array read
  under Usage Page (LEDs). Confirmed on hardware afterwards: `keyboardReports:153 publishes:153
  undecodedReports:0`. `ns2_kbm_runtime.c` is now host-testable (`test_kbm_runtime_lifecycle`), and
  the three formerly silent admission exits plus `undecodedReports` are in `kbm status` and the
  Windows companion. **Lesson: instrument the boundary before theorising across it.**
  [`docs/experiments/ble-keyboard-classified-as-mouse-2026-08-29.md`](docs/experiments/ble-keyboard-classified-as-mouse-2026-08-29.md)
- **Management companions are remembered across boots 2026-08-29; REQUIRES A REFLASH; device
  validation pending.** A second front end's management bond appeared in Paired Controllers as a
  device the user never paired, offering to forget it. Role was **live evidence only**: the
  observation asserting `management` was gated on a live management handle, and `config_ble`'s
  durable identity is cleared on disconnect — so an offline management bond reported role `unknown`,
  and a bonded, non-connected, roleless peer is precisely what a companion routes to Paired. Worse,
  the forget guard's "two independent guards" were **both** live-only, so the protection existed only
  for the client issuing the command; the other one could be forgotten out of its own management
  relationship. Schema 13 adds a bounded four-entry companion table storing the durable IDENTITY
  address (never an RPA). An entry is written only after a management session actually authenticated
  — remembered fact, not inference — and is dropped when the bond is deleted, so a role can never
  outlive its credential. Upgraded adapters start with an EMPTY table on purpose: each companion
  re-registers on its next session rather than having membership invented from an existing bond.
  Windows needed no change; it already routes `management` to This PC, and now does so with no local
  history at all. 77/77 host tests, 568 Windows tests, both parity guards green, Pico 2 W builds.

- **Classic controller lifecycle and controller identity — pass COMPLETE 2026-08-29; hardware
  confirmed.** Five defects, found in order and each one exposing the next. **(1)** The BOOTSEL
  sample park waited for core 0 with interrupts disabled from inside a BTstack run-loop callback,
  which holds the async_context lock core 0 blocks on for every BTstack-touching management command —
  so remote pairing froze both cores outright. The park is now bounded, withdrawing through a Dekker
  handshake because core 0 tri-states flash CS on the strength of core 1 being parked.
  **(2)** `HCI_Authentication_Complete` only arrives in response to this host's own
  `HCI_Authentication_Requested`, which this firmware sends for the Wiimote family and one named
  device only — BTstack's HID Host registers `LEVEL_0` and never asks. Every other Classic
  controller drives SSP itself, so the key commit waited for an event that never came and no link key
  was ever stored. Encryption Change is now accepted as equally conclusive proof.
  **(3)** With the pairing window open, inquiry restarts with no gap while a controller stays
  discoverable through its own pairing, and an inquiry result for a connection already in flight was
  re-admitted — rebuilding the candidate from a frequently nameless EIR, clearing the parked link key
  and starting a second HID connection. That single mistake produced the whole DualSense symptom
  cluster at once: no durable key, generic classification, and no vendor-driver initialisation, hence
  no player-slot LED and no configured colour. **(4)** A generic-fallback classification was published
  while the PnP SDP query was still outstanding, and the companion reads the inventory once, the
  instant pairing completes — inside that window. **(5)** The remembered controller name is captured
  at bonding, before any driver has claimed the peer, so a controller advertising no name persisted
  the scan handler's display placeholder and reverted to it whenever it went offline. Identity
  promotion is now one-directional: an authoritative driver identity may replace what is remembered;
  the generic fallback never may. **Confirmed on hardware:** one DualSense pairing reaches the DS5
  driver with correct player LED and configured colour, its Classic key persists (`btpeers` reports
  it `tr:1`, bonded, offline), it reconnects on that stored key with the pairing window closed
  (`btauth`: `auth_had_stored_key`, `observed_ok`, `key_size:16`) and survives adapter reboot; Xbox
  BLE connect/reconnect, Forget, and remote pairing all behave; and an offline Xbox keeps its
  identity. Nothing matches on a device name anywhere in these fixes.
  [`docs/experiments/classic-first-pair-readmission-2026-08-29.md`](docs/experiments/classic-first-pair-readmission-2026-08-29.md),
  [`docs/bluetooth/PERSISTENCE.md`](docs/bluetooth/PERSISTENCE.md)
- **Bluetooth Management 2.0 — Phase 7 shipped 2026-08-28; the pass is COMPLETE through all seven
  documented phases; REQUIRES A REFLASH; device validation pending.** Compatibility and degradation.
  `peerForget` and `remotePairing` are probed **independently** of `peers`, because they shipped in
  different phases: an adapter can list peers without being able to forget one, and one flag would
  either hide a working list or draw a button that answers `unknown command`. **An unprobed
  capability stays `Unknown`, never `Unsupported`** — a probe that could not run must not cost the
  adapter a feature. The probes avoid side effects rather than avoiding the question: `remotePairing`
  uses read-only `pairing status` instead of opening a real 30 s window to find out whether the verb
  exists, and `peerForget` uses `p_00000000`, the all-zero hash that no identity address produces, so
  firmware that has the command answers `already_absent` and deletes nothing. New `storage_full`
  pairing reason: with both security stores full, pairing is **refused rather than started**, because
  a window that could only end in a silent eviction is worse than a refusal that names the fix — and
  the fix is the selective forget the user now has. Corrupt-registry recovery was already total and
  already tested (unparseable documents decode to empty, individual bad rows are dropped rather than
  the file, a future schema is refused, text is re-sanitised on read); it was verified rather than
  rebuilt. Firmware-side declared capabilities are deliberately **not** added: `unknown command` is
  the protocol's own authority, and a declaration would be a second thing to keep in step. **Both
  boards rebuild clean from scratch with no new warnings**, 74/74 host-test targets passed, 1317
  Android JVM test executions with 0 failures, both install-reset markers verified, lint 0 errors,
  both APKs assembled, descriptor parity green at bridge contract 4 with an unchanged 161-byte
  digest.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Bluetooth Management 2.0 — Phase 6 remote controller pairing shipped 2026-08-28; REQUIRES A
  REFLASH; device validation pending.** The adapter can be told to pair a controller from the app, so
  a unit behind a TV or inside a dock never has to be reached. **There is no second pairing flow**:
  `pairing start` records a request that the existing control tick consumes and passes to the same
  `open_pairing_window()` the BOOTSEL gesture calls, so the radio behaves identically whichever
  trigger started it. **The firmware owns the deadline** — losing the app, the session or the phone
  cannot leave the adapter discoverable, and `pairing cancel` is a courtesy rather than a safety
  mechanism. `op` is a generation, not a handle, so a status belonging to an older attempt cannot
  describe the current one and switching adapters mid-operation drops the client's view outright.
  Failures name themselves — `no_controller`, `management_disabled`, `busy`, `locked_out` — instead
  of collapsing into "pairing failed", and a second start while a window is open is refused as `busy`
  **including when the user opened it with the button**, because re-arming would silently extend
  something they did physically. **Phase 0's last open product decision is answered: the two pairing
  authorities are now split.** `mgmt_accept_bonding()` read the same flag as controller pairing, so
  opening a controller window also admitted a new management bond; locally that is defensible because
  someone was holding the adapter, but a request arriving over the air would have opened a window in
  which a *different phone* could claim the management relationship. Remote pairing now grants
  controller discovery and no management authority; only the local gesture grants both. The window
  stays at 30 s rather than the design's suggested 60 s, deliberately: it is the same window the
  physical gesture uses, and changing it to satisfy a recommendation about a number would alter
  behaviour the user relies on. **Both boards rebuild clean from scratch with no new warnings**,
  74/74 host-test targets passed (the new target is `test_mgmt_pairing`), 1311 Android JVM test
  executions with 0 failures, a `pairingStatus` vector added to the shared conformance fixture and
  asserted to carry no identity, both install-reset markers verified, `peers_op_run` frames unchanged
  at 36/44 bytes, lint 0 errors, both APKs assembled, descriptor parity green at bridge contract 4
  with an unchanged 161-byte digest. Not yet validated on hardware.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Automatic bond destruction removed 2026-08-28; REQUIRES A REFLASH; device validation pending.**
  On the latest firmware an adapter holding three bonds reported `btbonds: []` **in the same powered
  session, with no reflash and no power cycle**, and then stopped answering UART entirely — `ping`
  included, which is pure core 0 and touches no BTstack marshalling. **The trigger is still not
  proven and is not claimed.** What is proven is the response: **four separate sites deleted a
  durable bond automatically when authentication failed**, each locally reasonable and none aware of
  the others — the LE disconnect handler on reason 0x05/0x06, the LE `SM_EVENT_REENCRYPTION_COMPLETE`
  failure branch, the Switch 2 direct-HCI re-encryption path, and Classic authentication on 0x06.
  Between them they can empty the database, and three bonds is three deletions. **All four now
  preserve the credential and drop only the link.** A genuinely stale bond fails the same way every
  attempt — bounded, visible, recoverable by an explicit act — instead of erasing itself silently;
  the reconnect cascade was already bounded and already declines to chase a peer that left
  deliberately, so nothing loops. The Switch 2 pair is worth singling out: the SM path already
  preserved the custom bond while the direct-HCI path deleted it, so **which half of the same
  recovery ran decided whether the pairing survived**. Destructive paths that remain are all
  user-driven: `peers forget`, `bonds remove`, the BOOTSEL wipe, `btfresh`, MouthPad clear-bond, and
  the install reset. Two audits closed alongside it. **TLV storage is transaction-safe**: deleting a
  tag writes zeros in place, compaction writes the new bank's header *last* as the commit marker, and
  an interruption leaves the previous bank selected — so an interrupted write **cannot** make every
  bond vanish, which is what rules out corruption and points at deletion-by-code. The runtime
  `btstack_erase_flash_banks()` is compiled out on CYW43 entirely. **`flash_safe_execute()`** runs on
  core 1 and parks core 0 via a lockout registered inside the TinyUSB loop, with `UINT32_MAX`
  timeouts on both sides; no lock-order defect was proven, but the exposure scales with how often
  error handling writes flash, and removing automatic deletion makes normal authentication failures
  RAM-only. That is the containment available without redesigning persistence. Both boards rebuild
  clean with no new warnings, 73/73 host targets, the policy pinned across all 256 HCI status values,
  `peers_op_run` frames unchanged at 36/44 bytes, 1303 Android JVM executions with 0 failures, lint 0
  errors, descriptor parity green.
  [`docs/bluetooth/PERSISTENCE.md`](docs/bluetooth/PERSISTENCE.md)
- **Peer pagination and controller routing fixed 2026-08-28; REQUIRES A REFLASH; device validation
  pending.** Refreshing Paired Controllers with a DualSense Edge connected returned
  `response_too_large` and the list never updated. **Reproduced over UART and then in a host
  harness**, so the cause is established rather than inferred: the live inventory is four peers whose
  encoded rows are 164, 182, 129 and 92 bytes, and the page from cursor 0 filled to 502 of 512 and
  then could not fit its 11-byte `],"next":3}`. Cursors 1 through 4 all succeeded, which is exactly
  what made it look like a size problem. **It was a budgeting one.** `MGMT_PEERS_SUFFIX_RESERVE` was
  tested at the top of each loop iteration — against the length left by the *previous* row — and the
  next row was then appended against the **full** capacity, so the reserve reserved nothing. Rows are
  now appended against `capacity - SUFFIX_RESERVE`; cursor 0 returns two rows with `next:2` and the
  inventory paginates. **No buffer was enlarged.** `mgmt_bonds` had the identical defect at both its
  page and legacy sites and was corrected the same way. Routing: a bonded, non-companion peer is now
  a **paired controller whether or not the adapter can currently name it**. Routing on role was wrong
  and hid real hardware — role is live evidence only, so after a reboot every paired controller reads
  `unknown` until it reconnects, and the DualSense sat in Diagnostics while genuinely bonded on both
  transports. Terminology follows actual state now that there is no manual save action anywhere:
  **Connected / Pairing / Paired / Not paired**, and a Classic controller between its ACL and its
  link key reads "Pairing · Completing pairing" rather than implying the user must save it.
  Diagnostics keeps the management relationship, raw LE bond slots and genuinely unattributable
  records. **A cross-core BTstack dispatch race reported earlier in this investigation does not
  exist and was withdrawn** — `btstack_host_init()`'s embedded run loop is USB-dongle only
  (`btstack_host.c:1408`); CYW43 builds link `btstack_run_loop_async_context`, whose
  `execute_on_main_thread` wraps the callback-list mutation in
  `async_context_acquire_lock_blocking()`, and the linked context is
  `async_context_threadsafe_background` with a real multicore-safe recursive mutex. No migration was
  performed. **Both boards rebuild clean from scratch with no new warnings**, 73/73 host-test targets
  passed, BTstack dependency contract passed, both install-reset markers verified, `peers_op_run`
  frames remain 36 bytes (Pico W) and 44 bytes (Pico 2 W), 1303 Android JVM test executions with 0
  failures, lint 0 errors, both APKs assembled, descriptor parity green at bridge contract 4 with an
  unchanged 161-byte digest. The adapter hang seen while spamming refresh remains **unexplained**;
  its most likely trigger — every refresh failing outright — is now gone, but no cause was proven.
  [`docs/management/PROTOCOL.md`](docs/management/PROTOCOL.md)
- **Bluetooth Management 2.0 — Phase 5 selective forget shipped 2026-08-28; REQUIRES A REFLASH;
  device validation pending.** The user can now remove one controller's pairing without disturbing
  anything else. `peers forget <id>` is **one atomic firmware operation**, not an app-issued
  disconnect-then-delete pair: resolve the opaque id, guard, drop live links, delete on **both**
  transports, then **re-enumerate to verify** — all inside a single run-loop callback, so nothing can
  race between the steps. It reuses `btstack_host_forget_device_typed()`, which the Phase 0 audit
  identified two years of phases ago as already being the right sequence; the phase was a command
  surface, a guard and a verification rather than new Bluetooth machinery. **The untyped form is
  used deliberately**: "forget this controller" means the device, so a cross-transport peer loses
  every credential it holds. Leaving half would let it reconnect on the surviving transport and look
  like the forget silently failed. **The reply reports what the adapter observed after deleting, not
  what it intended** — `bonded` and `tr` come from a second enumeration, and `incomplete` exists so a
  partial failure is visible instead of a client cache claiming a pairing is gone while the adapter
  still holds one. **`already_absent` is a success**, because a management reply can be lost after
  the command has already executed and a retry must not report failure for completed work; a
  *malformed* id is still a usage error, since reporting it as already-forgotten would tell the user
  a controller was removed when nothing was addressed. **The management companion cannot be forgotten
  here** — refused twice over, structurally on its durable identity and again on role, which also
  catches the same phone in its Controller Link relationship; getting that wrong would cut off the
  app issuing the command. The peer id stays one-way: the adapter resolves it by rebuilding the
  inventory and recomputing each id, so a client can address only a peer the adapter itself reported
  and cannot smuggle in a raw address. App: Forget appears on Connected and Saved rows with
  consequence-first confirmation copy (the disconnect sentence only where it applies), the inventory
  is re-read after every attempt including failures, and **history is kept on purpose** so a
  forgotten controller becomes a Recent row rather than vanishing. **Both boards rebuild clean from
  scratch with no new warnings.** 73/73 host-test targets passed (`test_mgmt_peers` +5 cases,
  including the phase gate in miniature), 1301 Android JVM test executions with 0 failures (up from
  1293), a `peersForget` vector added to the shared conformance fixture and asserted to carry no key
  material, both install-reset markers verified, lint 0 errors, both APKs assembled, descriptor
  parity green at bridge contract 4 with an unchanged 161-byte digest. Not yet validated on hardware:
  the nine-step checklist in the audit document's §15. Next: Phase 6, remote pairing — which owes one
  product decision first, whether the management bonding window may keep sharing
  `pairing_window_open` with controller pairing.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Management LE identity canonicalised and the companion bond protected, 2026-08-28;
  HARDWARE-CONFIRMED.** The reboot regression is resolved: the adapter reconnects across ordinary
  power cycles and the companion is one peer, not two. Investigating the Phase 4 post-reboot
  `RepairRequired` report found several proven defects in how the firmware identifies its management
  companion. **The cause of the first post-reboot 0x05/0x06 is NOT established, and this checkpoint
  does not claim to have root-caused that regression** — it fixes what the source proves is wrong and
  leaves the question open until hardware says otherwise. `config_ble.client_addr` is the
  over-the-air address from `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`, which under LE privacy is a
  rotating RPA, and it was being used for durable comparisons where only the resolved identity is
  valid. Three of them were therefore **permanently false**: the reconnect selector could not exclude
  the connected companion, the Classic authentication path could never recognise the companion's own
  Controller Link, and an explicit forget left the management session running.
  `btstack_host_companion_terms()` was already correct. `config_ble_durable_addr()` now answers with
  the record `sm_le_device_index()` says the link is authenticated against, falling back to the
  identity from the newly handled `SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED`, then to the connection
  address. **BTstack does the resolution; nothing re-derives it.** The same mistake was why one phone
  showed as two peers — a bonded identity row plus an unbonded RPA row — so the peer observation is
  emitted under the durable identity and the two collapse into one `management` peer carrying the
  real Classic + LE transport mask. Separately, `SM_EVENT_REENCRYPTION_COMPLETE`'s failure branch
  could delete the **companion's** bond: the management peripheral is absent from the central-role
  connection table by design, so `find_connection_by_handle()` returned NULL and NULL fell through to
  controller stale-key recovery. Reaching it is unrecoverable — the app excludes 0x05/0x06 from retry
  and goes straight to a terminal repair state — so management re-encryption failures now drop the
  link and keep the bond. Also fixed: the Phase 4 peer structures made `peers_op_run`'s frame 6132
  bytes, which is safe on Pico 2 W's 48 KiB core-1 stack but **overflowed Pico W**, whose core 1 gets
  the SDK default 2048 bytes in SCRATCH_X with 6388 bytes of headroom before allocated memory; the
  workspace moved to static BT-owned storage, taking the frame to 52 bytes with identical peer
  semantics and a `_Static_assert` guarding the move. UI: Paired Controllers now holds physical
  controllers only — the management phone, unattributable records and raw LE bond slots moved to a
  Diagnostics **Bluetooth records** card. That boundary rests on the corrected firmware identity
  model rather than hiding it. Android retry policy deliberately **unchanged** for this checkpoint,
  so hardware can show whether the firmware fixes alone resolve the reboot regression. **Both boards
  rebuild clean from scratch with no new warnings**, 73/73 host-test targets passed, BTstack
  dependency contract passed, both install-reset markers verified, 1293 Android JVM test executions
  with 0 failures (up from 1283), lint 0 errors, both APKs assembled, descriptor parity green at
  bridge contract 4 with an unchanged 161-byte digest.
  [`docs/bluetooth/PERSISTENCE.md`](docs/bluetooth/PERSISTENCE.md)
- **Bluetooth Management 2.0 — Phase 4 names, classification and history shipped 2026-08-28;
  HARDWARE-CONFIRMED.** The adapter can now say what a controller *is*,
  and the app remembers it. `peers list` gained three optional fields: `class` — the bthid driver
  identity the adapter derived for a live connection, "Sony DualSense" rather than the "Wireless
  Controller" the device calls itself — plus `vid`/`pid`. **`class` outranks `name` deliberately**:
  the remote name is a claim by the device that its owner can change, while the classification is
  the conclusion this firmware reached from VID/PID and the HID descriptor, which is the same
  decision that determines how the controller is parsed. **The envelope stayed at `v:1`.** The new
  fields are optional on an existing shape, so an older app ignores them and a newer app reads an
  older adapter's pages unchanged; bumping the version would have broken both directions to describe
  a change that breaks neither. The other half is app-side: per-adapter history (`PeerHistoryRecord`
  / `AdapterPeerHistory` / `PeerHistoryCodec` / `PeerHistoryStore`), which closes the gap Phase 3
  documented — the adapter classifies from live evidence only, so after a power cycle every saved
  controller that has not reconnected was reported `unknown` and nameless. History remembers the
  strongest role the adapter ever *proved* and the best name it ever gave, and `ControllerInventory`
  merges the two into **Connected / Saved pairings / Recent / This phone**. **History never rewrites
  the adapter's answer**: the live role is carried through verbatim, the remembered one travels
  beside it, and the row says "remembered" — the protocol's rule that `unknown` must be rendered as
  unidentified and never promoted to `controller` is preserved literally. The one decision memory
  makes alone is exclusion: a peer proven to be the user's own phone stays out of the controller
  list even when the adapter can no longer identify it, because being wrong there costs a row under
  "This phone" while being wrong the other way offers to forget the management relationship. **Only
  a complete inventory read is recorded** — a partial one is indistinguishable from an adapter that
  has forgotten a controller, and recording it would tell the user their controllers had been
  unpaired. One read happens automatically per verified session so history advances without the user
  pressing refresh. **Adapter-side peer metadata (design §24.2) is deliberately NOT implemented**:
  it was built after this phase's first pass, destabilised the Bluetooth core, and was withdrawn;
  the storage audit's capacity finding still stands but capacity was never the binding constraint,
  and what actually broke is an open unknown that must be answered before anyone tries again. UI:
  Settings now carries **Paired adapters** and **Paired controllers** as two separate cards — the
  saved-pairings card moved off the Controllers page, which keeps its subject of who is driving
  right now — and Recent's only action is "Remove from history", which is app-local and is not and
  does not read as a forget. Selective forget remains Phase 5. **Both boards rebuild clean from
  scratch with no new warnings.** 73/73 declared active host-test targets passed (`test_mgmt_peers`
  grew by 5 cases), 1283 Android JVM test executions with 0 failures, up from 1222 (app debug 321, app release 321,
  `:bridge-core` 568, `:management-core` 73), peer vectors in the shared conformance fixture carry
  the new fields, lint unchanged at 0 errors, both APKs
  assembled, and descriptor parity green at bridge contract 4 with an unchanged 161-byte digest. Not
  yet validated on hardware: the ten-step checklist in the audit document's §14. Next: Phase 5,
  selective forget.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Bluetooth Management 2.0 — Phase 3 read-only peer inventory shipped 2026-08-27; REQUIRES A
  REFLASH; device validation pending.** The adapter can now report what it has paired.
  `peers list [cursor]` merges the Classic link-key store and the LE device DB into one row per
  physical device — the first use of `gap_link_key_iterator_*` in this project — annotates each with
  the role the adapter can currently prove, and carries **no key material in any field**. That last
  point is structural rather than careful: `mgmt_peer_t` has nowhere to put a key, and the Classic
  iterator's key output is written to a local that nothing reads and is then wiped. Peers are
  deliberately a different model from bonds: with cross-transport key derivation configured, the
  management phone holds both an LE bond and a Classic link key, so a bond-shaped list would show the
  user's own phone twice and call it a controller. **Role is live evidence only** — the connected
  management client, the Controller Link peer identified from its HID descriptor, connected
  controllers, and the `JPLC` reconnect record — so a stored bond whose owner has not been seen since
  boot is reported `unknown` and the UI says "Saved pairing, not yet identified" rather than guessing
  it into the controller list. That is also what keeps the phase gate true in the hardest case: a
  freshly booted adapter cannot misclassify the management bond as a controller because it declines
  to classify it at all. One bug was caught by its own test: the merge resolved competing
  observations by comparing role enum values numerically, and the enum's declaration order is the
  reverse of the precedence order, so a phone that was both the management companion and a connected
  input source came out as a **controller** — exactly the misclassification the gate forbids.
  Precedence is now an explicit function, written separately so reordering the enum cannot silently
  change the answer. Read-only: `peers` has no mutating form in protocol version 1, and selective
  forget remains Phase 5. New surfaces: `peers list` over BLE/CDC, `btpeers [cursor]` over UART
  (deferred, never blocking core0), and a Saved pairings card on the Controllers page. **Both boards
  build clean with no new warnings.** 73/73 declared active host-test targets passed (up from 72; the
  new target is `test_mgmt_peers`), 1222 Android JVM test executions with 0 failures (app debug 295,
  app release 295, `:bridge-core` 568, `:management-core` 65), peer vectors added to the shared
  conformance fixture so a non-Kotlin client inherits the same guards, lint unchanged at 0 errors,
  both APKs assembled, and descriptor parity green at bridge contract 4 with an unchanged 161-byte
  digest. Not yet validated on hardware: the eight-step checklist in the audit document's §13, whose
  gate is comparing the app's list against the adapter's own `btpeers` output. Next: Phase 4, names,
  classification and history.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Bluetooth Management 2.0 — Phase 2 generation-safe adapter switching shipped 2026-08-27; device
  validation pending.** Switching the active adapter is now one generation-owned transition:
  `ActiveAdapterCoordinator` owns which adapter is active and the `Settled / Retiring / Activating`
  phase, and `AdapterSwitch` executes the ordered handover. **The outgoing adapter is retired
  completely before the incoming one becomes authoritative** — the retirement awaits
  `AdapterRepository.disconnect()`, which returns only after the transport has retired its GATT
  generation and emitted its final state, and it joins any in-flight connect job first so a job
  still unwinding cannot publish the previous adapter's outcome afterwards. Three further guards:
  `accepts(address)` gates the connection collector so nothing is accepted during a retirement and
  only the active adapter's own address is accepted outside one; activation outcomes are guarded by
  adapter **identity** rather than generation, because the connect path is shared with ordinary
  reconnects, so a result for A cannot settle B; and the registry's selection follows the
  coordinator so the two can never disagree. **A failed switch does not fall back** — the chosen
  adapter stays selected and reads "Selected, not connected · tap to retry", because quietly
  reconnecting something other than what was asked for while the UI names the choice is the failure
  mode this design exists to prevent. Controller Link and the on-screen controller are stopped
  before management is retired and are never carried across; a switch also closes overlays and drops
  to the Adapter section so no screen stays bound to the previous adapter. `AdapterRelationship-
  Coordinator` is unchanged and still owns one attempt at one relationship; the counters are
  deliberately separate, because merged, a connection retry would be indistinguishable from a change
  of adapter. One design bug was caught by its own test: `begin` treated an already-selected adapter
  as "already active" even when disconnected, which made the truthful failed-switch state a dead
  end; the guard now also requires `connected`. **Companion source only; no firmware source changed
  and no reflash is required.** 1203 Android JVM test executions with 0 failures (app debug 295, app
  release 295, `:bridge-core` 568, `:management-core` 45), the 20 new cases pinning the ordering
  property against a recording fake rather than a radio; lint unchanged at 0 errors with no findings
  in changed files; both APKs assembled; descriptor parity green at bridge contract 4 with an
  unchanged 161-byte digest. Not yet validated on hardware: the eight-step switching checklist in
  the audit document's §12. Next: Phase 3, read-only peer inventory — which is the first phase
  needing firmware work, since Classic link-key enumeration does not exist yet.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Bluetooth Management 2.0 — Phase 1 multi-adapter registry shipped 2026-08-27; device validation
  pending.** The companion now remembers many adapters instead of one. `AdapterRegistry` /
  `AdapterRegistryCodec` (schema 1) / `AdapterRegistryStore` replace the single-record
  `AdapterRelationshipStore`, migrating the existing saved adapter on first read and leaving the old
  preferences file on disk so a bad migration is recoverable by hand.
  `AdapterRegistryReconciler` replaces the single-relationship reconciler with a set reconciliation
  over `CompanionDeviceManager.myAssociations`. **The defect that forced the Forget/Pair cycle is
  gone:** a verified connect used to call `disassociate()` on the previously saved adapter whenever
  the association ID differed, so connecting to the second adapter unregistered the first. Adapters
  are now unregistered only when the user asks, and repair/remove/reconcile are all per-adapter, so
  reflashing one adapter cannot disturb another. Adapters carry an app-local sanitised alias
  (duplicates allowed, disambiguated by a four-character identity suffix), and Settings gained an
  Adapters list with select, rename and remove-from-app. Two deliberate departures from the design's
  obvious reading, both recorded with their rationale: `Ambiguous` no longer blocks connecting (it
  now means two association records claim ONE adapter — stale bookkeeping, not a broken pairing,
  because the registry always has a definite address to dial), and selecting an adapter does not
  chain into a connect, because a teardown followed immediately by a connect is precisely the race
  Phase 2's switch coordinator exists to own. `ManagementOwner`'s one-transport invariant is
  untouched. **Companion source only; no firmware source changed and no reflash is required.** 1163
  Android JVM test executions with 0 failures (app debug 275, app release 275, `:bridge-core` 568,
  `:management-core` 45), 32 of them new; `lintDebug`/`lintRelease` 0 errors and no findings in the
  changed files; both APKs assembled; descriptor parity green at bridge contract 4 with an unchanged
  161-byte digest, confirming no wire contract moved. Not yet validated on hardware: the two-adapter
  checklist in the audit document's §11 is the maintainer's to run. Next: Phase 2, the
  generation-safe active-adapter switch.
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md)
- **Bluetooth Management 2.0 — Phase 0 audit complete 2026-08-27; no code changed.** The
  multi-adapter / peer-inventory / remote-pairing upgrade described in
  `BT_MANAGEMENT_UPGRADE_PASS.md` has had its repository audit performed and recorded in
  [`docs/bluetooth/bt-management-2.0-phase0-audit.md`](docs/bluetooth/bt-management-2.0-phase0-audit.md).
  None of the eight documented stop conditions fired, so Phase 1 may proceed. Three findings change
  what the later phases must do. (1) The companion already uses `CompanionDeviceManager` as a
  load-bearing part of its relationship model and treats **more than one association as
  `Ambiguous` -> `RepairRequired`**, so the registry must widen `AdapterRelationshipReconciler`
  rather than be added beside it. (2) `CompanionViewModel` **actively `disassociate()`s the
  previously saved adapter** on every verified connect whose association ID differs — that single
  call is what forces the Forget/Pair churn, and it must be retired as part of Phase 1, not after.
  (3) The management phone is simultaneously the LE management peer and the Classic Controller Link
  peer with `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` enabled, so the cross-transport multi-entry peer
  is the project's common case rather than an edge case and the Phase 3 role model must handle it
  from its first version. Decisive positive results: the adapter already has a stable identity (the
  management peripheral advertises with `BD_ADDR_TYPE_LE_PUBLIC` and the app already keys on it, so
  nothing new needs broadcasting), `btstack_host_forget_device_typed()` is already the atomic
  forget-peer sequence the design asks for, `bonds list v2` is a reusable pagination precedent for
  peer inventory, and opening the controller pairing window does **not** tear down BLE management —
  `btstack_host_disconnect_all_devices()` walks only the controller tables and never touches
  `config_ble.handle`, which is what makes remote pairing feasible. One product decision is owed
  before Phase 6: `mgmt_accept_bonding()` reads the same `pairing_window_open` flag as controller
  admission, so an app-started pairing window would also admit a **new management phone** for 30 s.
  Audit only — no firmware, companion, or test source was touched, and no hardware action was taken.
- **Stage C dependency modernization — built and under initial hardware endurance 2026-08-25.**
  The firmware now builds against Pico SDK 2.3.0 (`98a542c1`), BTstack 1.8.2 (`075a0780`),
  cyw43-driver 1.1.1 (`055d6427`) and Arm GNU 15.2.Rel1, replacing 2.2.0 / 1.6.2 / 1.1.0 / 14.2.Rel1.
  Four upstream changes needed deliberate adaptation, three of them silent: the `hids_client` →
  `hids_host` rename **including `MAX_NR_HIDS_CLIENTS` → `MAX_NR_HIDS_HOSTS`** (the old spelling
  builds a zero-entry pool), the removal of `HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT`,
  `gap_disconnect()` no longer synthesising a disconnection-complete event for an already-released
  handle, and LE **Secure Connections Only now defaulting ON** — which this host explicitly turns
  back off, because requesting SC while permitting legacy fallback is the documented policy and
  SC-Only would also invalidate existing sub-16-byte-key bonds. The HID Host long-report shim was
  ported: 1.8.2 provides the 16-bit report length natively, so only the pending-send guard and the
  L2CAP diagnostic hook remain patched. **The CYW43439 Bluetooth firmware blob is byte-identical
  between the two SDK trees (`2075e3be…8647`), so this migration cannot have fixed a
  controller-side defect.** Both boards build clean with no warnings outside vendored libopus (and
  those reproduce identically under GCC 14.2). 72/72 declared active host-test targets passed, plus
  a new `tools/test_btstack_dependency_contract.py`. The maintainer subsequently flashed a Stage C
  candidate to a different Pico 2 W and began endurance on a different Android device. At the
  latest report it had exceeded three hours of established management traffic with five-second
  replies still completing, no observed HCI `0x08` or management disconnect, no obvious pairing
  regression, and about 20 successful Touch Gamepad reconnects. The run remains active. This is a
  material initial reliability improvement, not a complete parity campaign or proof that failure
  probability is zero. Read-only build inspection still resolves stock BTstack v1.8.2; no later
  CTKD-fix pin is configured, so the cross-transport risk described below remains live.
  [`docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md`](docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md)
- **Controller Link face-button inversion — fixed 2026-08-24, hardware replay pending.** The
  2026-08-23 Touch Gamepad face fix moved the firmware's four bridge face usages onto their logical
  A/B/X/Y destinations. That is correct for the on-screen pad, but `from_android_bridge` provenance
  is per device: the adapter sees one bridge stream and cannot tell an on-screen press from a
  built-in-pad press, so every Controller Link face press inverted on console under both layouts.
  Root cause was one shared `mapFaceButton` serving two origins that need OPPOSITE corrections — an
  on-screen slot sends the letter it draws, a physical key must be interpreted against the legend
  the handheld actually prints. Now `mapTouchFacePosition` and `mapPhysicalFaceKey`, applied per
  origin in `ControllerInputState.publish()`. **Companion-only; no firmware source changed and no
  reflash is required.** The physical path gained the cross-layer coverage it never had:
  `tools/fixtures/controller_link_face_mapping.csv` drives `ControllerLinkFaceMappingTest` (platform
  key + layout -> usage) and `tools/test_controller_link_face_goldens.c` (usage -> production
  parser -> bridge seam -> Pro Controller 2 report bit), mirroring the touch catalog fixture, and a
  resolver test asserts the two mappers can never resolve alike. Bench testing on a live Odin 2 Mini
  the same day found the second half of the defect: AYN's button-layout toggle changes the device
  IDENTITY (`0x2020/0x0111` "Odin Controller", legend-reporting, versus `0x0112` "Xbox Wireless
  Controller", positional) and Auto claimed BOTH as Nintendo, so a handheld left in Xbox mode
  inverted every face button on its own. Auto now separates the two modes. Touch Gamepad behaviour is
  byte-identical; the 20-row touch golden is unchanged and green. Console confirmation of the
  corrected labels is still owed on both origins.
- **Last software verification:** 2026-08-24 — Controller Link face-button repair. **Companion
  source only; no firmware source changed and no adapter reflash required.** 72/72 declared active
  host-test targets rebuilt from current source and passed (up from 71; the new target is
  `test_controller_link_face_goldens`), 798 Android JVM test executions across `:bridge-core`,
  `:management-core` and both app variants with 0 failures, and
  `check_android_descriptor_parity.py` green at bridge contract 3 with an unchanged 161-byte
  descriptor digest — confirming this is a semantic repair above the wire, not a contract change.
- **Previous software verification:** 2026-08-24 — Touch Gamepad layout editor and profile pass.
  **Android only; no firmware source changed**, so the host/firmware suites were not re-run and the
  2026-08-23 result below remains the current firmware verification. This pass: 777 Android JVM test
  executions across `:bridge-core`, `:management-core` and both app variants with 0 failures (up
  from 714; the new coverage is the profile library, its JSON codec and legacy migration, and editor
  selection/alignment/snapping at four window shapes and three densities), `lintDebug` +
  `lintRelease` with no new findings, and both APKs assembled. Editor behaviour was additionally
  driven on an Android 15 emulator through the debug layout lab; the Touch Gamepad entry below
  records what that covered and what it could not.
- **Last firmware + full-tree verification:** 2026-08-23 — personality-aware Touch Gamepad layout
  pass. Both
  board builds, **71/71 declared active host-test targets rebuilt from current source and passed; 9 test
  sources are explicitly classified outside the active host suite**
  (`pwsh -File tools/run_host_tests.ps1`, inventory in
  [`docs/host-test-inventory.md`](docs/host-test-inventory.md)), the in-band management group plus
  descriptor parity, controller-link classification, console-slot wiring, Bluetooth identity and
  the trace/NFC/corpus Python suites, 714 Android JVM test executions (app debug 226, app release
  226, bridge-core 217, management-core 45), `lintDebug` + `lintRelease`, both APKs, and both
  install-reset markers. This is not a claim that every test in the tree passes: five of the
  nine excluded sources are uncovered on the host, and three represent coverage lost when the code
  they exercised changed.
- **Withdrawn claim (negative knowledge, 2026-08-21):** earlier entries citing "76/76", "78/78", or
  "79/79 compiled host tests" were **not** valid proof about current source. Only 23 of the 79
  host-test sources had a build recipe; the rest were executables accumulated in `build/host-tests`
  by ad-hoc `gcc` invocations of unknown vintage, and the documented procedure was to run whatever
  binaries the directory happened to contain. Cleaning `build/` exposed this. The runner now
  recreates its output directory empty, refuses to start if any `tools/test_*.c` lacks either a
  recipe or a declared exclusion, and counts only what it built. Do not restore the old totals.
- **Last hardware validation:** 2026-08-21 — fresh LE management pairing, repeated Refresh,
  personality switching with post-transition management recovery, DualSense Edge + BLE management
  coexistence, Controller Link ↔ physical-controller source switching, a ≥75-minute mixed soak with
  continuous controller audio and no drops, and Controller Link input reaching the console from a
  non-zero Classic connection index. Records:
  [`docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md`](docs/experiments/android-le-bond-transport-and-coexistence-soak-2026-08-21.md),
  [`docs/experiments/controller-link-console-slot-misroute-2026-08-21.md`](docs/experiments/controller-link-console-slot-misroute-2026-08-21.md).
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

**Android bridge contract 4 — GL/GR (added 2026-08-26; source-verified, hardware validation open).**
The Pro Controller 2 grip buttons are now reachable from the companion. They always existed below
the bridge: `switch_pro2_encode.c` writes `SWITCH_EXTRA_GL`/`GR` to report `0x09`, `ns2_build_report_05`
to report `0x05`, and `NS2_BASE_BUTTON_MAP` already routed `JP_BUTTON_A4`/`A5` to `NS2_DST_GL`/`GR`.
What was missing was any way for the Android bridge to reach them: `ControllerButton` had fifteen
entries, the wire's button field was two bytes carrying exactly fifteen usages, and the descriptor
had ONE pad bit left. Two buttons did not fit, so contract 4 widens the field to three bytes —
**the first offset-shifting change since v2**: the hat moved from wire byte 9 to 10 and the whole
vendor extension (motion, battery, flags, timestamp) with it, so the v1 report is no longer a prefix
of the v2 one. Strict identification is unchanged and is what makes that safe: a contract-3 APK
against contract-4 firmware fails the exact descriptor match, falls back to the v1 profile and
reports the skew, rather than silently reading motion from the wrong offsets. **Reflash when
updating the APK across this boundary.** The firmware change is one table: `gamepad_quirks_android_bridge()`
names usages 16/17 as `A4`/`A5`, kept separate from the shared `SEQ_BUTTON_MAP` so no generic pad
declaring seventeen buttons acquires grip presses; `BLE_MAX_BUTTONS` went 16 → 17 so a location is
recorded for usage 17. On the touch side GL/GR are **optional** Pro2 controls — bound by the profile,
present in the CATALOG, `inDefaultLayout = false` — so the shipped layout is byte-for-byte unchanged
and saved layouts load unmodified, while the editor can add, move, scale, rotate, latch and delete
them like any other digital button. No other personality exposes them.
Under Editor 2.0 "optional" no longer means "present and hidden": the catalog and the layout are
separate things, so an unplaced control is simply absent and Add Control offers the whole catalog
unconditionally. Adding one at its authored spot restores its cluster membership too, so
delete-then-re-add is not a way to quietly break a group. Verified on device (pre-2.0 UI): the
default Pro2 layout is unchanged, the dialog offers "GL / GR — Optional, not in the default layout",
and adding each places it in the outer bottom corners.

**NSO GameCube `Z` on PC/Steam — fixed 2026-08-26, source-verified, PC hardware validation open.**
`Z` worked on a Switch 2 and did nothing at all on PC/Steam. Root cause:
`switch_gc_encode_report05()` hardcoded the ZR bit (byte0 `0x80`) to zero, on the written
assumption that GameCube `Z` had "no representable bit position in this shared format". Report
`0x0A` — the console path — carries `Z` in its own GC slot, so the console was unaffected; report
`0x05` is the only report a PC host ever selects, and it carried nothing. `ZL` was emitted all
along, which is the same lopsidedness `ns2_seam.c` already had to work around
("`SWITCH_MASK_ZL` has a live bit in the GC encoder, `SWITCH_MASK_ZR` does not"). **The assumption
was wrong: GameCube `Z` IS the ZR control** — the console's own Test Input screen names it "ZR",
and a genuine NSO GameCube Controller's `Z` is recognized as ZR by Windows/Steam, which it could
only be by setting that bit. Physical legend and host semantic are separate facts and both are now
stated wherever this control is described: the shell (and the Touch Gamepad) print **`Z`**, the
host-facing logical control is **ZR**. The fix is one line — report `0x05` now sets ZR from
`GC_MASK_Z`, sourced from `gc_extra` alone exactly as report `0x0A` does, so the two reports cannot
disagree about whether it is pressed. Nothing on the console path changed; trigger detents really
do have no slot in `0x05` and are unchanged (a PC reads travel from the analog tail at
`0x3C`/`0x3D`). Pinned by `tools/test_switch_gc_report.c` cases 13/15/16/17 and, on the touch side,
by `TouchProfilesTest`'s legend-versus-binding test.

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
- **There is exactly ONE console output slot, and a BTstack connection index is never a slot.**
  Every console-facing reader is a hardcoded `get_global_gamepad_input(0, …)`, and the arbiter
  already guarantees a single accepted source, so `router_submit_input()` publishes through
  `NS2_CONSOLE_SLOT` and `find_player_index()` returns 0. Indexing the shared slot arrays by
  connection index has silently discarded a correctly-arbitrated peer twice — rumble on 2026-07-12,
  input on 2026-08-21 — because BLE connection indices are offset past `NS2_SLOTS` and fall back to
  0, so only a *Classic* peer on connection 1..3 is affected. Pinned by
  `tools/test_ns2_console_slot_wiring.py`.
- A source with no Bluetooth name is not "no controller". The Android Controller Link arrives as an
  incoming Classic HID Device connection, so no inquiry record ever names it;
  `ns2_input_source_display_name()` supplies "Controller Link" from the source class the firmware
  derived from the bridge's own declared HID descriptor. A real name always wins, and a nameless
  direct controller stays nameless.
- Surfaces: UART `input sources` / `input active <id|none>`, the same bounded query over
  bonded management (source names truncated to 16 characters), and the companion's
  **Active controller** selector. The Adapter page's Controller row is the console slot's published
  identity — active-input truth, not physical attachment.

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
- **Touch Gamepad:** personality-aware layout engine and editor implemented and source-tested;
  **device-validated 2026-08-24, console-in-the-loop acceptance open.** The touchscreen itself can
  be the controller, for a host with no gamepad at all. Gamepad -> Touch
  Gamepad opens a full-screen mode whose input terminates in the same `ControllerState` and crosses
  the same transport; firmware sees the declared Android bridge source but cannot distinguish the
  screen from the handheld's built-in controls.
  The portable half is the existing plain-JVM `:bridge-core` module's
  `dev.picoswitch.bridge.touch`; its current directory under `android/companion/` is historical,
  not Android ownership. It now owns an exhaustive Pro Controller 2 / NSO GameCube / sideways
  Joy-Con 2 Left / sideways Joy-Con 2 Right profile catalog, immutable templates and fixed output
  bindings, versioned per-profile layout documents, composition, editor operations, schema policy,
  validation, and contact quarantine across live personality replacement. Android owns only the
  Compose renderer/pointer/editor UI and app-private `SharedPreferences` store. `Config` or
  unconfirmed personalities stay neutral; profile changes release and swap without tearing down
  the Classic controller link.
  **Editor 2.0 (added 2026-08-26, source-tested; device and console validation open):** a layout is
  now an INSTANCE-BASED scene rather than a sparse patch over an immutable stencil. Two statements
  carry the whole design and are pinned by `TouchLayoutDocumentTest`: *control instance identity is
  not logical button identity* (several instances may share a binding and stay separate objects),
  and *default layout membership is not personality capability* (the catalog says what may exist,
  `inDefaultLayout` says only what the shipped arrangement places). Schema version **2** stores a
  list of instances; version 1 is read once and migrated by `TouchLayoutMigration`, with the
  pre-migration text kept beside it under a `.v1` key.
  What that buys: duplicate controls (two A buttons, both pressing A), real delete-and-re-add
  instead of hide/show, arbitrary grouping of any controls at all, free rotation with magnetic snap
  to the authored orientation and its quarter turns, explicit z-order, session undo/redo over whole
  revisions, a Preview mode that plays the working draft without leaving the editor, and a toolbar
  that floats anywhere or docks to any of the four safe edges. The runtime aggregates per INSTANCE
  and resolves per binding only at publish — digital by OR, triggers by max, sticks and the D-pad by
  contact ownership with hand-off — so releasing one of two A buttons cannot release the other
  (`TouchDuplicateControlTest`). Rotation is visual and hit geometry only: hit testing
  inverse-transforms the point, the audit measures rotated screen-space extents, and no binding,
  D-pad direction or analog-trigger travel axis moves with it.
  The layout editor is a direct-manipulation edit MODE rather than a settings panel: the whole
  controller stays drawn, input forwarding pauses, and the only chrome is ONE small movable toolbar
  of twelve constant slots (`handle | Add | Select several | Duplicate | Group | Delete | Undo |
  Redo | More | Preview | Save | Done`). A selection changes what those buttons DO and what the More
  menu contains; it never adds a second bar and never changes the slot count, so the toolbar cannot
  reflow under a finger. Everything contextual — size, rotation, z-order, hold mode, exact numbers,
  reset — lives in the More menu, headed by the name of the control it will act on. Tap selects, an
  explicit Select mode extends the selection, drag moves, and two fingers scale and rotate together
  through one gesture loop; a whole gesture is one undo entry. The toolbar is dragged by a handle
  after a long press, previews its dock candidate with a haptic tick, remembers landscape and
  portrait placements separately, and its top-left is coerced into the interaction-safe rectangle
  unconditionally by `TouchToolbarLayout.topLeft` — a toolbar that cannot be reached has no Done
  button, so reachability is a post-condition rather than a special case
  (`TouchToolbarPlacementTest`).
  Geometry problems are drawn ON the offending control — red outline, translucent red fill, corner
  handles in the error colour — from `ResolvedTouchLayout.invalidControlIds`, which is derived from
  the same findings that decide whether the layout may be played. An editor that recomputed its own
  idea of validity could paint a control red while the layout played, and the user would have no way
  to tell which of the two was lying (`TouchInvalidControlTest`).
  Controls are named the way they are DRAWN wherever a person reads them — the Add picker, the menu
  header, the inspector title, the delete notice and the audit's own sentences all resolve through
  `TouchControlNaming`. Pro Controller 2's face controls carry no authored legend (their letter comes
  from the binding at draw time), so anything naming them from their ids produced "face-north" and
  "face-east"; those are internal cardinal slots and not buttons anyone has pressed. Duplicates keep
  the name and gain a copy number (`B (2)`), while findings still identify instances by id
  (`TouchControlNamingTest`).
  Group editing expands a selection to its cluster, and what is outlined is exactly what an
  edit moves. Optional grid and snapping (`TouchEditorAlignment`, pure, over resolved geometry) pull
  a movement onto region centre lines, other controls, safe edges or grid lines while never making a
  position unreachable, and apply one correction to the whole selection so cluster spacing survives.
  The obsolete Nintendo/Xbox face-layout toggle is **gone**; the Touch Gamepad menu now switches the
  real controller personality through `switchPersonality`, the same lifecycle the adapter screen
  uses, with no Touch-only personality state.
  Layouts are organized into per-personality **profiles**: an immutable synthesized `Default` plus up
  to twelve user profiles, with create/duplicate/rename/reset/delete, unsaved-change confirmation,
  and a save-onto-Default path that creates a named profile instead of overwriting the one layout
  that is always recoverable. The factory profile is never persisted, which makes that protection
  structural. The pre-profile single override document is adopted once on upgrade rather than
  discarded. `TouchProfileLibraryJsonCodec` also encodes a single profile as a standalone document —
  the export/import foundation. Bounded uniform scale, precise numeric position/size/rotation entry,
  per-selection/profile reset, hit-bound preview and blocking audit feedback all remain, and none of
  it mutates a shipped default.
  Contact ownership remains keyed on the platform's stable contact identifier (never its array
  index), circular stick clamping with a
  rescaling radial deadzone, eight D-pad sectors with radial and angular hysteresis, a declarative
  layout resolved into the interaction-safe rectangle and mechanically audited for overlap/target
  size/bounds, and one idempotent release-all invoked from every invalidating boundary.
  **Editor 2.0 production-polish pass (2026-08-27, source-tested and device-verified on the
  companion UI; console-in-the-loop still open).** Six defects fixed, each with regression coverage:
  1. *Rotation was effectively impossible to start.* The magnetic snap computed its target from the
     STORED angle plus one frame's delta, so a control sitting on a snap target could never leave
     it — every frame proposed stored-plus-a-fraction, the magnet pulled it back, the stored angle
     never moved, and the next frame asked the identical question. Rotation only escaped when a
     single frame happened to carry more than the 6° snap radius, which on a 120 Hz panel means
     flinging a whole hand. `snappedRotationDelta` now takes the gesture's own accumulated raw
     intent, which makes the magnet a DETENT rather than a wall. No saved semantics, snap radius or
     snap target changed; scale and rotation still both apply from the same frame.
  2. The GameCube X/Y bean LETTERS were drawn tilted and selection outlines were drawn rotated,
     because artwork rotation, outline rotation and total visual rotation had been collapsed into
     one angle. They are three separate questions and are now three properties
     (`artworkRotationDegrees`, `outlineRotationDegrees`, `visualRotationDegrees`).
  3. Any drag destroyed the undo history: the gesture-end handler called `history.reset` to "rebase"
     onto the pre-drag document, which clears both stacks. Pushing the endpoint is all that was ever
     needed (`TouchEditorHistoryTest`).
  4. Deleting a grouped control counted the TAPPED ids rather than the effective targets, so four
     face buttons vanished and the notice said "A deleted" — a destructive action under-reporting
     itself (`TouchGroupTransformTest`).
  5. The undo notice used Material's `Snackbar`, which lays itself out at the width it is offered —
     the whole window here — so "A deleted UNDO" arrived as a bar most of the screen wide. It is now
     content-sized with a ceiling, in the editor's own chrome colours, and steps over a
     bottom-docked toolbar instead of covering Delete/Undo/Redo.
  6. The personality chips wrapped without vertical spacing, so the fourth controller sat flush
     against the first row.
  Copy was cut to what the headings and controls do not already say — the re-enumeration warning,
  the hold gesture, recovery and migration text are kept because none of them can be guessed — and
  the More menu's three bare latch rows (`Default / Enabled / Disabled`) gained the one caption that
  makes them mean anything.
  **Double-tap-hold-slide, with retrigger (added 2026-08-26, device-validated for gesture
  recognition, mash-immunity, visuals and lifecycle clearing; in-game feel open):** double-tapping a
  digital control, HOLDING the second press and then SLIDING away locks a persistent hold, so a
  thumb does not have to stay planted on a run or aim button. It is entirely a `:bridge-core` concern —
  `effectivePressed = (touchPressed || latchedPressed) && !retriggering` — so no transport,
  firmware, HID report or cadence changed, and the console cannot tell a latched button from a held
  one. TIMING ALONE CANNOT CREATE A HOLD, because timing alone collides with real play: a plain
  double tap collides with mashing (mashing IS a stream of double taps), and a double tap whose
  second press is merely held collides with the ordinary "double tap, then keep holding" a game may
  ask for directly — no dwell separates those, because they are the same input. So the dwell only
  ARMS the gesture and a deliberate slide of `latchCommitDistanceUnits` (64 logical units) from the
  press origin commits it; nothing a game asks a player to do involves pressing a button and dragging
  off it. While armed the control is still an ordinary held button and letting go simply ends the
  press. **The slide is reversible until the finger lifts (added 2026-08-26, device-verified):**
  bringing the same contact back within `latchCancelDistanceUnits` of where the press began takes
  the hold off again, returns the contact to ARMED so sliding out re-locks it, and leaves an
  ordinary press that releases normally on lift. Nothing is published across either transition —
  the finger is still down, so the control is physically pressed throughout and the console sees no
  edge at all; only the hold that would have OUTLIVED the finger changes. The cancel radius is
  `gestureSlopUnits` (24 logical units) rather than a new constant, because it is the same question
  that constant already answers — is this finger still essentially where it started? — and it sits
  well inside the 64-unit commit distance so the pair is a hysteresis band rather than one
  threshold a resting thumb can flap across; `TouchLatchConfig` refuses a configuration that closes
  the band. Cancelling is gated on the hold having been committed BY THE CONTACT STILL DOWN, which
  is load-bearing rather than bookkeeping: a press on an already-held control begins inside the
  radius by definition, so without that gate the smallest jitter would silently drop a hold made
  earlier. It fires one `LatchReleased` tick and reopens the padlock, logs
  `state=unlatched reason=slid_back`, and carries its own `latchesCancelled` counter in the touch
  diagnostics line — "the gesture completes and the user keeps taking it back" is a different fault
  from "it completes when nobody meant it to". An analog trigger cancels the same way and its held
  LEVEL goes with the hold. CREATING a hold is deliberately harder than removing one — engage is a leading tap, a dwell
  of `2 x holdThresholdNanos` (360 ms) and the slide; release is a single press held for the base
  itself (180 ms), no leading tap and no slide — because an unwanted hold is a stuck button the user
  has to diagnose while a lost one is one gesture away from coming back. Both dwells derive from one
  base by a named rule so they cannot drift apart, and the base sits well below the platform's 500 ms
  long-press timeout so neither feels like a context menu. Arming is announced by the lightest haptic
  tick and an OPEN padlock badge that closes on commit, which is what makes the gesture
  discoverable. Tapping an already-latched control retriggers it — the hold is
  masked for 48 ms at publish time so a real release edge exists, then reasserts — because a control
  the game already sees as held produces no edge otherwise; the mask is sized against the session's
  conflated 125 Hz mailbox and one 30 Hz consumer frame, the badge does not blink, and repeated taps
  cannot unlatch. The pulse is decided when the press ENDS, since a press on a held control is
  ambiguous until then, which is what stops every unlatch emitting a pointless release/press first;
  and when the release dwell elapses the finger stays authoritative, so clearing the hold produces no
  edge of its own. The recognizer only observes presses the engine has already published, so no tap
  is delayed. Dwells and the mask are the only timed work: the engine publishes `nextDeadlineNanos`
  and the surface ticks it, a pull model with no queued closure, which is what makes any pending
  timed work unable to resurrect a button after teardown. Double-tap window, minimum gap and
  first-tap bound come from `ViewConfiguration`; the dwells, the mask, the drift tolerance and the
  commit distance (both displacement from the contact origin in logical units, never a control-bounds
  test, so button size and where inside it the user touched change nothing) are project constants in
  `TouchLatchConfig` and are not user-facing. Sticks and
  the D-pad are excluded structurally by `TouchControlKind.supportsLatch`. A global setting supplies
  the default and the editor's per-control Default/Enabled/Disabled overrides it, carried as one
  optional `latch` flag in the existing sparse-override schema — old layouts load unchanged and the
  schema version deliberately did not move. Latches ride the same release-all as everything else and
  are additionally dropped when the latch configuration changes; a latched control shows the pressed
  fill plus a padlock badge, latch transitions are logged as `controller/touch latch`, and the touch
  diagnostics line carries latch and retrigger counters. `TouchLatchOutputTest` pins every gesture as
  published controller-state edges over a real `ControllerInputState`, not as internal flags.
  **Analog trigger travel on the NSO GameCube personality (added 2026-08-26; travel geometry
  root-caused against device measurement and replaced 2026-08-26; console-in-the-loop acceptance
  open):** the on-screen `L` and `R` now have real travel. Touch the
  trigger and pull it toward the middle of the screen; how far you pull is how deep the trigger
  goes, full travel reaches the GameCube terminal click, and the SAME tap-dwell-slide hold gesture
  every digital control uses locks the trigger at the level the slide ends on. No visible rail,
  slider or track was added — the trigger is the handle and the travel space is gesture space,
  costing no permanent gameplay screen area — and no firmware, transport, HID report or cadence
  changed. **The gesture direction is derived from the control's current position**, never from
  which trigger it is: the axis points from the resolved control centre at the middle of the
  interaction-safe rectangle, so a trigger the user drags to the bottom-left pulls up-and-right on
  the very next gesture with nothing to configure. That direction is taken in the layout's
  NORMALIZED space, not in pixels, and the difference was measured rather than reasoned: derived in
  pixels the shipped `L` resolved to `(0.818, 0.575)` on a 1920x1025 handheld, `(0.772, 0.635)` on
  a 16:10 tablet and `(0.712, 0.703)` on a 4:3 one, so the same authored control produced a gesture
  ten degrees apart between devices purely because a wider window puts its centre further right.
  Dividing by the region's extents first makes the pull a property of the LAYOUT — `(0.605, 0.796)`
  for that control on every window shape — and for top-placed triggers it leans DOWN, which is
  where a thumb was already going. The visible fill follows the same axis, so it now grows downward
  on the shipped placement instead of across it. It is frozen for the duration of each gesture
  (a recomposition or an animating inset must not rotate the axis under a thumb already pulling)
  and a control parked on the exact centre falls back to the nearest edge's inward normal rather
  than normalizing noise. Value is the vector PROJECTION onto that axis, so a thumb's natural arc
  costs nothing and no invisible corridor exists to fall out of; the owning contact keeps the
  trigger until it lifts, however far outside the visible control it wanders. Full travel is TWO BUDGETS, and the pull ends
  when it spends either: `Rx = travelFraction (0.50) * min(width, height)` across,
  `Ry = Rx * verticalTravelRatio (0.50)` down, and
  `fullTravel(axis) = min(Rx / |axis.x|, Ry / |axis.y|)`. The guarantee that buys is the one the
  complaint was about: **completing a pull never displaces the finger by more than `Ry` vertically
  or `Rx` horizontally, whatever direction the axis points.** A purely horizontal axis still
  resolves to exactly `Rx` and a purely vertical one to exactly `Ry`, unchanged on every region.
  Two earlier versions are recorded here because both were plausible and both are disproven. One
  shared distance for every direction (`min(width, height) * 0.50`) came first, and feel testing
  rejected it: the same pixels are a quarter of a landscape screen's width but half of its height.
  A weighted BLEND, `|axis.x| * Rx + |axis.y| * Ry`, replaced it and DEVICE MEASUREMENT rejected
  that too — it charges the horizontal budget in proportion to how much of the AXIS lies along X,
  but a thumb pulling a top-placed trigger moves down, spends no width, and still paid for it. See
  `docs/experiments/touch-analog-trigger-vertical-travel-2026-08-26.md`: on an Odin 2 Mini in
  landscape the shipped `L` needed 985 px of a 1025 px usable height for a downward stroke, and the
  same stroke on the shipped geometry now needs 404 px (39%), measured the same way. The
  coordinate pipeline was cleared by the same capture and is NOT a factor: pointer positions arrive
  in exactly the pixel space the region is built in, with no rotation, density or inset term
  between them. Which triggers have travel is a property of the PROFILE, not the control:
  `TouchControlAction.Trigger(analog = true)` is set only on the GameCube `L`/`R`, because only
  `switch_gc_encode` passes a trigger byte the console acts on; Pro2 and Joy-Con triggers are
  digital on the far side and still answer fully on the way down. **Nothing is published at
  pointer-down**, because on this personality full travel IS the terminal click and a speculative
  press would fire the detent at the start of every deliberate pull; the press resolves later —
  into a pull past the platform's drag slop, into a deliberate full pull after the same
  `holdThresholdNanos` base both latch dwells derive from (so "tap it then keep holding it" stays
  an ordinary held trigger), or into a tap on release that publishes a full pull for one
  `retriggerReleaseNanos` pulse so the conflating 125 Hz mailbox cannot swallow it. **A press that
  is DEFINING a hold resolves in a different order**, which was the second correction from device
  testing. That press — the second of a double tap, which the latch recognizer has already
  classified as an Engage candidate — is on its way to CHOOSING a level, so resolving it into a
  full pull first put `255` and the terminal detent on the wire before the slide had selected
  anything, and a 40% hold arrived as full-click-then-slide-down. On the GameCube personality that
  click is a gameplay action a partial pull does not have. So a latch-defining press starts with no
  full-pull resolve pending at all and gets one only when the gesture ARMS, one `holdThresholdNanos`
  later, by which point the slide that selects the level is available and the arming tick has said
  so. A press that never slides is still an ordinary held trigger; it just arrives at full travel
  one base later. **That fallback is a state transition, not only an output.** When it wins it
  CONSUMES the hold candidate on that contact (`TouchControlLatch.abandonArm`), because otherwise a
  slide made afterwards could still lock a partial hold — persistent state reached through a
  gesture the recognizer had already answered as "you are holding the trigger down", which reads as
  the trigger spontaneously dropping from full to a level nobody chose. After it, the same contact
  behaves like any other live pull: the value follows the finger and lifting off leaves nothing
  held. Consumption is scoped to the contact, so lifting and repeating the gesture latches
  normally. The detent's
  hysteresis is enforced on the PUBLISHED VALUE and not on a local Boolean, which is the one
  non-obvious thing here: the firmware seam derives the GameCube click from the trigger byte alone
  (`> 224`) and discards the `L2`/`R2` bits for a generic bridge source, so every sub-detent value
  is capped at exactly byte `224` and only the detent itself reaches `255` — see the retune warning
  in `docs/switch2-gc/mapping.md`. The hold is the SHARED one, not a second system:
  `TouchControlLatch` still decides *whether* a control is held and the trigger only remembers at
  what *level*; the single specialization is that an analog trigger measures its commit slide as
  the projection onto its own axis, so a sideways or outward slide cannot lock a trigger to
  nothing. A finger always outranks a hold and the hold returns on lift; a tap on a held trigger
  pulses full (or, when it is already full, pulses a release edge, exactly as the digital retrigger
  mask does); and pressing-and-holding a held trigger removes the hold AND leaves the same contact
  armed, so a slide immediately chooses a new level without lifting off. Latched levels ride the
  same idempotent release-all as every other hold and are dropped at every boundary including a
  personality change. Visually the pad fills in the CARDINAL direction the USER ACTUALLY SWIPED —
  down, up, right or left — rather than a rotated diagonal wipe across a rounded pad, which reads
  as a shading artefact rather than as a level. **What the picture reads and what the value reads
  are deliberately different questions**, and a diagonal axis is where they part company: the
  shipped `R` has an inward axis of about `(-0.732, +0.681)`, so a straight downward swipe projects
  positively onto it and correctly increases travel — but choosing the fill from that axis drew a
  bar growing LEFT while the thumb moved DOWN. So:

  ```text
  value  <- projection onto the position-derived inward axis   (unchanged)
  fill   <- cardinal of the actual swipe displacement          (presentation only)
  ```

  The swipe direction is established EXACTLY ONCE per contact, at the moment it crosses the same
  drag slop that turns the press into a pull, from the whole displacement since pointer-down, and
  is frozen from then on — a thumb sweeps an arc, and re-reading it per frame would flip the bar
  mid-pull. It is discarded on release, so the next gesture decides again. Before a swipe exists —
  at rest, or during a press that has not moved far enough to mean anything — the position-derived
  inward axis supplies the default, which is also what makes an editor move re-present a control
  immediately without waiting to be pressed. The ENGINE publishes all of it per control
  (`TouchDiagnosticsSnapshot.analogTriggerFills`) rather than the renderer re-deriving anything,
  because only the engine holds the frozen values. Nothing about a control's identity or its
  authored position is encoded, and none of it reaches travel, the detent, the latch or ownership.

  One thing about the RESTING default is worth stating so it is not later mistaken for a rendering
  bug: untouched, the shipped `L` shows a rightward fill and `R` a leftward one. Both analog
  triggers occupy the outer slots (`trigger-l`, anchor 0.075; `trigger-r`, anchor 0.925), so they
  mirror each other and both axes lean across. `zl` now occupies the inner-left slot at anchor
  0.200 and mirrors `z` in the inner-right slot. `TouchAnalogTriggerGeometryTest` pins both mirror
  pairs. As soon as either trigger is swiped, the swipe wins. The padlock badge is unchanged; the
  detent gets one haptic tick on entry only. The
  diagnostics line carries detent/pulse counters and the live levels by control id, and latch
  engage events now log `level=`. The authored default swaps only the left-side `L` and `ZL` slots:
  `L` is at x=60 and `ZL` at x=160; every other control position and all scales are unchanged.
  Stored layouts load unchanged — no schema field was added. (`TouchAnalogTriggerOutputTest`'s commit
  assertion is stated against the detent rather than against a particular step of one slide.)
  **GameCube `z`/`Y` proximity: real, but only in the hit MARGINS (settled 2026-08-27,
  source-verified; GameCube template revision 2, unchanged from the shipped geometry).** Teaching
  `TouchLayoutAudit` to measure rotated screen-space extents — needed anyway once any control can be
  turned — immediately reported `z` overlapping the `Y` bean, which the pre-2.0 audit could not see
  because it bounded the bean with its UNROTATED box and the bean is authored at −11°. An Editor 2.0
  pass responded by moving the whole top row from y=42 to y=34. That was the wrong call and has been
  reverted: the overlap is real but lies ENTIRELY between the two controls' hit MARGINS. Their drawn
  shapes clear each other by about a unit, which a dense probe through `containsVisual` confirms
  (`TouchGameCubeDefaultTest`). Both controls therefore remain reliably pressable by aiming at what
  is drawn, and the established top-row height and every non-L/ZL control position remain unchanged.
  The audit now classifies an overlap three ways — none, margin-only, artwork — and only an ARTWORK
  collision blocks. That is the property that actually matters: when drawn shapes collide the router
  has to let z-order decide what the user pressed, and a control that answers unpredictably is worse
  than a layout that refuses to load and says why. A courtesy margin, by contrast, is an invitation
  rather than a claim on space. **Negative knowledge worth keeping:** an audit finding is not on its
  own a reason to move approved artwork; check first whether the finding is about the drawn shapes or
  about the margins around them.
  **KNOWN, pre-existing, unfixed — GameCube `c-stick`/`b` artwork collision near 2:1
  (characterised 2026-08-27, source-verified).** Separately from the above, at aspect ratios in the
  band ≈1.945–2.057 the `c-stick` and the small `B` button genuinely overlap in their DRAWN circles,
  by about four units at the worst point, and the shipped GameCube controller refuses to draw there.
  That band includes 18:9 displays. It is not an Editor 2.0 regression — the pre-2.0 audit reported
  it too, and slightly sooner — but no probe shape in the project's established list lands inside the
  band, so the check had never run there. Fixing it means moving or shrinking approved GameCube
  artwork, which is a product decision rather than a polish-pass one, so it is pinned by a
  deliberately named KNOWN-defect test that fails when someone fixes it.
  **Sideways Joy-Con action-cluster orientation (fixed 2026-08-26, device-verified on both halves):**
  the four action controls were placed by reading their logical names as screen positions, so
  `direction-up` was drawn at the top of the display when the shell's up button points at the
  player's LEFT once the half is held sideways — the player pressed the button in the X position and
  got Y. `TouchClusterRotation` now states each half's turn once (L anticlockwise, R clockwise,
  agreeing with the firmware's own `joycon2_pack_sideways_stick`) and `squareDiamond` places a
  cluster keyed by each control's slot on the shell. Direction markings rotate with the shell because
  an arrow's meaning is its orientation; Joy-Con Right letters stay upright because a letter's
  meaning is the letter. Purely visual: every binding still matches the firmware's sideways encoder,
  and `TouchSidewaysJoyConTest` pins physical identity, logical action and screen position
  separately. No control id changed, so stored profiles keep every position their owner chose; both
  templates declare a newer revision to record that the shipped defaults moved. Physical
  versus touch input is an explicit `InputAuthority`, never a merge; entering rebinds the session
  with `bindSource(null, touchCapabilities)` so console rumble reaches the host's own actuator, and
  no synthetic input device is invented. Face controls are positions resolved through
  `ControllerLayoutResolver`; the Android bridge seam preserves those logical A/B/X/Y usages
  instead of applying the direct-controller B/A/Y/X map a second time. The correction is limited to
  those four bridge face usages; direct controllers, all non-face controls, and raw Joy-Con source
  bits are unchanged. **That seam correction is per DEVICE, not per origin**, so it also landed on
  Controller Link and inverted it (fixed 2026-08-24 — see below). The exact bridge descriptor also pins the sequential/no-extra parser profile,
  so an incidental phone/PC name or VID/PID cannot activate a physical-controller quirk before the
  seam. A shared 20-row catalog fixture now enforces exact face-key/label/HID-usage coverage and is
  driven through the production descriptor parser, bridge-aware seam and final Pro2/GameCube/Joy-Con
  encoders; direct-controller mappings and raw Joy-Con source bits are checked unchanged. Android
  Back, including either edge gesture, opens/closes the Touch Gamepad menu; leaving now requires its
  explicit Exit action.
  Editor behaviour was exercised on an Android 15 emulator through the debug layout lab (selection,
  group selection, long-press multi-select, drag with live snap guides, size stepping, blocking
  audit feedback, grid, all four toolbar docks, profile create/save/persist-across-restart, the
  unsaved-changes prompt, and the too-small-window refusal).
  **Maintainer-validated on two Android devices, 2026-08-24:** pinch scaling, multi-touch editing,
  live profile swaps, and the interruption behaviour the stuck-input matrix guards — losing focus,
  sleeping and app-switching each release every held input as intended. **Disconnect/reconnect is
  additionally confirmed against a real Switch**, which is the half the handheld cannot observe
  about itself: the console is neutral on the far side of a link interruption, not merely the
  neutral report sent. Owner-reported rather than capture-proven; no log or capture was taken, and
  the release path it exercises is the same idempotent release-all the host tests cover.
  Still open: per-personality control-by-control correctness against the matching console
  personality, held-contact profile changes with the Classic link retained, and in-game feel.
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

## Windows companion — Phase 4 implemented; Phase 3 hardware smoke-tested 2026-08-29

Second host platform, designed in `WINDOWS_PASS.md` and implemented under `windows/companion/`.
C# on .NET 9 with WinUI 3. The Roadmap's own numbering is used below so the gap between what exists
and what is designed stays explicit.

- **Delivered. 545 tests pass.**
  - *Phase 0* — the project skeleton with the architecture boundaries enforced from day one, plus a
    WinUI shell that builds x64/ARM64, packages as MSIX, and satisfies the single-instance exit
    criterion when run.
  - *Phase 1* — `PicoSwitch.Management.Core` in full (protocol, commands, client, pagination,
    single-flight session, BLE carrier constants and reply assembly, domain), and
    `PicoSwitch.Bridge.Core`'s `Core/` and `Protocol/` (button/axis model, input state machine,
    face-layout resolver, candidate rule, motion convention, rumble shaping, counters, contract,
    descriptor, encoder, output codec).
  - *Phase 2* — the BLE GATT management transport, radio capability probe, pairing/unpair helper,
    recovery and bond-mismatch policies, `ManagementOwner`, `AdapterRepository`, the adapter
    registry and peer-history books with their codecs and atomic stores, the relationship and
    active-adapter coordinators, and the ordered `AdapterSwitch`.
  - The bridge session, the input/motion/battery/output backends and the whole touch layer are
    Phase 6/6a and do not exist.
- **Sharing level.** Level 1: the C# reimplements the documented contracts rather than linking the
  Kotlin modules, because the relocation trigger in `docs/bridge/PLATFORM_BACKEND.md` is a *JVM*
  second consumer. `:bridge-core` and `:management-core` were deliberately **not** moved.
- **Anti-drift.** Both languages read the same fixtures. `tools/check_android_descriptor_parity.py`
  now covers **three** languages, including a separate C# digest registry, and was verified to fail
  on a one-sided C# descriptor edit. A new shared fixture,
  `tools/fixtures/bridge_report_goldens.csv`, closes the gap the descriptor guard could not: 47
  vectors of normalized state → wire bytes, generated from the Kotlin encoder and consumed by both
  encoders, so a divergence in how the report is *filled* fails in the other language's suite.
- **Controller Link is still gated.** The HOGP peripheral-role experiment (`WINDOWS_PASS.md` §14.5)
  has not been run, so whether this PC can act as a controller source is **Unknown**. Nothing in
  Phase 6 may be scheduled until it is.
- **The shell builds, packages and runs.** x64 and ARM64 unpackaged, plus an unsigned MSIX. Verified
  by running it on 2026-08-29: the window appears with the custom title bar, Mica, the
  `NavigationView` rail and the connection banner, and **a second launch exits 0 leaving exactly one
  instance** — the Phase 0 exit criterion for single-instance activation, which is the Windows
  enforcement of "one process, one active management session".
- **Two toolchain traps, recorded so they are not rediagnosed** (`windows/companion/docs/README.md`
  §4). First: `XamlCompiler.exe` reports XAML errors by **exiting 1 in complete silence** — no
  output, no `output.json`, and it still emits every `*.g.cs`, so it reads as a tooling crash when it
  is a reporting failure over real authoring errors. Rebuild with
  `-p:UseXamlCompilerExecutable=false` to get the message. Second: MSIX packaging needs **.NET
  Framework MSBuild**, because the Windows App SDK's `WinAppSdkValidateAppxManifestItems` task loads
  `System.Security.Permissions` and the .NET SDK's own MSBuild cannot resolve it; `build.ps1 -Msix`
  finds a Visual Studio MSBuild and says so plainly when there is none.
- **One specification correction.** `WINDOWS_PASS.md` §27.3 requires the manifest declare "exactly
  `bluetooth`, nothing else". `MakeAppx` rejects that: a WinUI 3 **desktop** app packaged as MSIX
  must also declare `runFullTrust`, which marks the package as full-trust Win32 rather than UWP and
  does **not** imply elevation. The manifest declares exactly those two and a guard test asserts the
  exact set.

- **Happy path — Confirmed on hardware, 2026-08-29.** The maintainer ran the Phase 2 build against
  a real adapter. Reconstructed from the persisted artifacts rather than from a live capture; every
  item below is a state that could not exist unless the step before it worked, which is why these
  are Confirmed rather than Strong Evidence. What is NOT claimed is anything about timing, retries,
  or repeated sessions.
  - BLE discovery on the management service UUID — the row exists for an adapter found by UUID
    filter, not by name;
  - the Windows pairing ceremony (`ConfirmOnly` + `Encryption`);
  - an encrypted management GATT session;
  - the `info.id == "picoswitch"` identity gate **before** any registry persistence — a row is
    written only on the far side of that check;
  - firmware (`2.0`) and personality (`pro2`) reads, cached as display-only state;
  - a **complete** five-peer logical inventory, folded into history, which refuses a partial read
    outright;
  - registry persistence and reload (`adapters.json`);
  - peer-history persistence and reload (`peer-history.json`).

- **One Bluetooth Management 2.0 assumption confirmed rather than reasoned.** Four of the five peers
  reported `role: "unknown"` while `bonded: true`, and only one carried a live identity
  (`Xbox Wireless Controller`). That is exactly the offline/post-reboot shape the model predicts, and
  it is direct hardware evidence that Paired Controllers must route on **durable bonded/trust
  evidence** rather than on live `role`. Routing on role would have shown the user an empty list with
  four real pairings hidden behind it.

- **Boundary C — Confirmed PASS, 2026-08-29.** One management client, no churn, read from the
  adapter's own `btlife` ring rather than from host counters: 21 lifecycle records on a single
  handle, strictly alternating, **zero** alternation violations; two Refreshes, all five
  destinations and a minimise/restore produced **no** transition at all; Disconnect retired the
  session exactly once; every disconnect carried `cause=none` and `disc.ctrl`/`disc.hci` never
  moved. Evidence: [`dumps/windows-phase2-oneclient-2026-08-29.md`](dumps/windows-phase2-oneclient-2026-08-29.md).
  Method note for whoever repeats it: `mgmt_watch.ps1` at `-IntervalMs 1500 -RingEverySec 60` gives
  a ~42 s effective host sample gap, too coarse to ORDER a connect/disconnect pair. Read the ring,
  not the poll deltas.

- **Boundaries A and B — Confirmed PASS, 2026-08-29 17:40.** Each cost a defect on the first run
  and was confirmed on a retest the same day. On the retest the **first Connect press after the
  flash** reached `RepairRequired` in 2.4 s, with every predicate resolving as the corrected model
  predicts (`paired=True observed=True answeredGatt=False linkFailures=2/2 -> BOND MISMATCH`), and
  `[repair] unpair <addr>: removed` appeared — the line that was entirely absent before.
  - **A. The bond-mismatch signature was wrong for this platform.** It expected the refusal at the
    ATTRIBUTE layer — `AccessDenied` or an authentication `HRESULT`. Against a genuinely reflashed
    adapter Windows produced neither: four attempts, every one `stage=services
    GattCommunicationStatus=Unreachable`, no ATT byte, no `HRESULT`, because the status was
    *returned* rather than thrown. **`Unreachable` after the device resolves is the Windows shape of
    a bond mismatch here** — Windows encrypts the link for a bonded peer before any ATT transaction
    exists, so the failure is below the attribute layer. Corroborated by elimination: `Uncached` is
    proven from source, so it is not a cache artefact; `mgmt_access.h` gates advertising and
    connection on neither bonding nor the pairing window, so no firmware rule refuses service
    discovery; and an unpair plus re-pair fixed it instantly with no firmware change.
    The corrected signature is **compound**, because `Unreachable` is also what a powered-off
    adapter produces: Windows still paired, the exact address seen advertising by the
    service-UUID-restricted watcher, `Unreachable` after the device resolved, and that happening on
    **two independently resolved device objects**. Nine negative tests pin that an absent,
    never-seen, unpaired or transiently failing adapter cannot reach `RepairRequired`.
    One consequence corrects a documented expectation: the attribute shape still ends the ladder at
    the first failure, but the link shape **must** let the fallback scan run, because the fallback
    is what produces the corroborating second observation.
  - **B. Repair did nothing at all.** It resolved the Windows pairing through
    `AdapterRecord.DeviceId`, a cached WinRT device path that **nothing had ever populated** — so it
    took its null branch every time, logged `no Windows device path cached`, cleared the repair flag
    and reported success while the stale bond survived. A repair test existed and passed, because it
    asserted only that the row survived, and the row survived a no-op. Repair now resolves the
    paired device fresh from the **address**, unpairs once, **verifies** Windows agrees, and clears
    the flag only then; `DeviceId` is removed outright, and `IAdapterPairingGateway` puts a seam
    under the call so the unpair is asserted rather than assumed.
  - Two adjacent findings, both from the same log. **Pair silently reuses a stale OS bond** — with
    Windows already paired the ceremony is skipped entirely, so four Pair presses ran no ceremony at
    all; the flow now says so, and a stale bond met through Pair reports a message that names a way
    out, since there is no remembered row to repair. **Remove is local-only and that is correct**
    (WINDOWS_PASS.md §19.5) — behaviour unchanged, diagnostic line now says what it did not do,
    semantics pinned by test.
  - **Confirmed as a security property:** the adapter admits a new management bond only inside its
    physical double-tap window. Observed in both directions — `AuthenticationTimeout` with the
    window shut, `Paired` with it open — matching `mgmt_accept_bonding` exactly.

- **The mechanism is now measured, not inferred.** A diagnostic added for the retest read, four
  times identically, `link status=Unreachable connection=Connected session=Closed maxPdu=23`.
  Windows established the LE link (`Connected`), the GATT session never opened (`Closed`), and the
  ATT MTU never left its 23-byte default — so **no ATT transaction of any kind occurred**. A link
  that comes up and carries no attribute traffic is the shape of encryption failing immediately
  after connection. *That* Windows encrypts a bonded peer before any ATT exchange is Confirmed by
  observation; that the failure is specifically SMP remains inference, because Windows exposes no
  SMP or HCI detail. Nothing in the implementation depends on the inference.
- **The negative case is hardware-confirmed too, which is the property that actually protects the
  user.** With the adapter powered off (2026-08-29 17:52) the classifier correctly declined:
  `paired=True observed=False answeredGatt=False linkFailures=1/2 -> not a bond mismatch`, with
  `link ... connection=Disconnected`. A switched-off adapter returns the same
  `GattCommunicationStatus` as a reflashed one, so this is the case where a wrong signature would
  offer to destroy a working pairing to recover from a flat battery. The exact observed values are
  pinned as a regression test.
- **A promotion criterion was fully met and promotion was declined anyway.** `ConnectionStatus`
  reads `Connected` for a present adapter and `Disconnected` for an absent one, so it IS a valid
  presence discriminator. But working it through every attempt observed across both sessions, it
  would change no outcome: the binding constraint is the two-resolved-devices corroboration, not
  presence. Relaxing *that* given proven presence is the tempting next step and is refused — a
  transient discovery failure on a healthy adapter produces the identical shape, and one
  observation does not establish that `Connected + Unreachable` is conclusive. `ConnectionStatus`
  stays diagnostic-only, now as a confirmed diagnostic rather than a speculative one.
- **Still open after A, B and C, and neither blocks Phase 3.**
  1. the recovery ladder's retry and 350 ms backoff have still never executed on hardware — every
     observed failure was at a non-retryable stage, so the retry has correctly never been taken;
  2. the A → B active-adapter handoff (boundary D) — **deferred, no second adapter available**.
     Nothing has been executed against two adapters, so the multi-adapter handoff is **not**
     hardware qualified and must not be described as such. Confidence is software only: the
     relationship and active-adapter coordinators are unit-tested over a fake port, including the
     ordering rule and generation-based rejection of stale callbacks, and no source evidence
     suggests a defect. What that cannot show is whether a real trailing asynchronous callback from
     A can reach B's state, because a fake port completes deterministically and a radio does not.
     Carried forward as a hardware validation item, to run before a second adapter is advertised as
     supported.

Next: hardware validation of Phase 4 (H9), the remaining Phase 3 manual UX pass (§26.5), then
Phase 5 (Virtual Amiibo). Boundary D whenever a second adapter exists (the adapter dashboard, which is the MVP). Some
Phase 3-shaped UI already exists because Phase 2 could not be exercised without it — the Adapter
page, Pair/Refresh/Disconnect, the remembered-adapter list with Connect/Repair/Remove, live session
state, the peer inventory and the Diagnostics page. That glue is real and stays; Phase 3 is an audit
of what remains on top of it.

- **An audit before spending bench time found the signature could not fire at all**, and it was
  worth running: three independent wiring defects, each of which would have read as "the hypothesis
  was wrong" on the bench. Thrown WinRT failures were never wrapped, so the `HRESULT` half had
  nothing to inspect; "Windows still paired" was read off a connection object already disposed by
  classification time; and "peer answered" was set only on the scan path, while a remembered adapter
  connects directly. Fixing them is what made the hardware run produce a usable answer rather than a
  second mystery — though the answer turned out to be that the condition set itself was wrong.
  The evidence the transport records is now three separate facts rather than one conflated
  `PeerReachable`: an advertisement for the exact address, an attribute-layer answer, and a count of
  independently resolved devices that failed at the link. They are accumulated per **logical**
  attempt — spanning the direct connect, any retry and the fallback — and reset when that attempt
  changes, so a session from ten minutes ago cannot vouch for a peer that is now switched off.

The prepared hardware sequence for the four boundaries, with what to capture and what would falsify
each, is [`docs/experiments/windows-phase2-boundaries-2026-08-29.md`](docs/experiments/windows-phase2-boundaries-2026-08-29.md).

Reference: [`windows/companion/docs/README.md`](windows/companion/docs/README.md), `WINDOWS_PASS.md`.

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
- **Established-session BLE `0x08`: pre-C confirmed; not reproduced after Stage C so far.** In a
  spontaneous failure Android's lower Bluetooth layer reported LE HCI `0x08` about 3 ms before the
  companion received write status 133, proving that 133 was downstream of an already-lost ACL. The
  Classic Controller Link was reported lost 15.000 s later. Source and live traces show one
  process-wide transport and strict request serialization; a bounded instrumented comparison then
  completed 75/75 requests (49 background polls and two full Refresh workflows) on one GATT
  generation with no errors or disconnect. A later management-only Condition F reproduced the same
  established LE timeout after Classic had been intentionally disconnected, proving Classic is not
  required. Its post-failure ring proves that the RP2350 rebooted at
  16:24:49.932-16:24:49.955, immediately before Android surfaced `0x08`; persistent recovery
  scratch rules out the project's deliberate Bluetooth recovery reboot, but the pre-C build did not
  expose the generic reset cause. Deliberate pre-C retesting later reproduced failures across
  approximately 2-14 minute lifetimes. By contrast, the ongoing Stage C candidate had exceeded
  three hours without reproduction at the latest observation. The leading interpretation is that
  the host-side SDK/BTstack/cyw43 integration change may have eliminated or dramatically reduced
  the failure; no exact responsible change is isolated, and the CYW43439 PatchRAM is unchanged.
  [`docs/experiments/established-management-gatt-failure-2026-08-25.md`](docs/experiments/established-management-gatt-failure-2026-08-25.md)

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
10. **Touch Gamepad physical acceptance — largely closed 2026-08-24 (owner-reported).** The
   maintainer validated the editor and the interruption behaviour on two Android devices: pinch
   scaling, multi-touch editing, live profile swaps, and losing focus / sleeping / app-switching
   while controls are held, each releasing every held input as intended. That closes the part of the
   stuck-input matrix reachable from the handheld alone, and it is why this gate is no longer
   release-blocking. **Disconnect/reconnect was additionally confirmed against a real Switch**, which
   settles the console-side question the handheld cannot answer about itself — the console is neutral
   after a link interruption, not merely sent a neutral report.
   Still uncovered: every Pro2, NSO GameCube, Joy-Con Left and Joy-Con Right control checked against
   the matching real console personality; held-contact profile changes with the Classic link
   retained; and in-game correctness and ergonomics.
11. **Remaining Bluetooth wipe/flash matrix.** The strict Xbox Elite Series 2 corrected-wipe retest
   passed. Run the still-uncovered powered-off/reboot/release-UF2 and other-family cases in
   [`docs/bluetooth/VALIDATION.md`](docs/bluetooth/VALIDATION.md). Record bond state before the remote
   returns so old trust and automatic replacement trust cannot be confused.

12. **Controller Link reliability candidate — the single open Bluetooth flash gate.** Implemented and
   source-tested 2026-08-22; **nothing about it has run on hardware**.

   **Blocker found and fixed 2026-08-22 (identity).** The first flashed candidate failed 10/10 on a
   fresh pairing for a reason unrelated to any of the three changes below: the LE management service
   advertised GAP Appearance `0x03C0` (Generic HID) while the adapter is the HID *host*. Android
   synthesises a Class of Device from that Appearance whenever it has none stored — every fresh
   pairing — persisting major class 5 (Peripheral). Its own HID Device profile then refuses the
   adapter (`btif_hd` → `check_cod_hid`, `(cod & 0x1F00) == 0x0500`) *after* the ACL,
   authentication, encryption and both HID channels succeed. Appearance is now `0x0080` Generic
   Computer, agreeing with the unchanged Classic CoD `0x000104`, guarded by a `_Static_assert` and a
   source check in `tools/test_bluetooth_closeout_wiring.py`. Confirmed from the app over ADB on the
   failing build: `major=0x0500 hostOk=false`.

   Because the poisoned class lives in the *phone's* record, an already-poisoned pairing must be
   cleared once and re-made against corrected firmware. No migration path was added: those records
   were produced by development builds, and encoding cleanup for them into production architecture
   would be worse than clearing the test relationship once. **Note a fresh management bond requires
   the physical pairing gesture** (`mgmt_accept_bonding` → `pairing_window_open`), so the clean-state
   validation cannot be automated. Flash exactly one artifact,
   `build/pico2_w/PicoSwitchWGA-pico2_w.uf2` from commit `f6cbe41`, and run the turnkey procedure in
   [`docs/experiments/controller-link-cycling-failure-2026-08-22.md`](docs/experiments/controller-link-cycling-failure-2026-08-22.md).
   It bundles three independent changes so one campaign settles all of them, and their evidence
   levels differ — do not let a single pass/fail collapse them:
   - **Type C** (dominant, 8 of 10 failures): companion encryption stand-down. Mechanism is
     source-established, **not** Confirmed — the acceptance run is what proves or falsifies it.
     `enc.deferrals > 0`, `enc.peer_completed ≈ deferrals`, `enc.collisions == 0`,
     `enc.unencrypted_active == 0`.
   - **Mode 1** (1 occurrence in 35 cycles): idle inquiry restart gap. Too rare for a 30-cycle run to
     confirm or refute; record any `PAGE_TIMEOUT` separately and do not read it as Type C.
   - **Mode 2** (0 occurrences in 35 cycles): cross-transport Classic trust. A real independent
     source-level defect; the campaign can only show it stays absent (`reject_window` unchanged).
   Type A (Android's own Bluetooth process abort) is an Android defect this candidate does not
   address and must be excluded from Type C statistics.

   **Appearance fix validated on hardware 2026-08-22.** Build `4b19842`, forgotten and re-paired:
   Appearance `0x0080` → Android stored Class of Device `0x000100` (Computer) → **18 consecutive
   Controller Link establishments, zero `check_cod_hid` refusals**. The encryption stand-down also
   fired for real on that build (`enc.deferrals 1`, `enc.peer_completed 1`, `enc.collisions 0`,
   `enc.unencrypted_active 0`). Type C, Mode 1 and Mode 2 remain as described above.

   **Controller Link is not a standalone transport — invariant violation found and fixed
   2026-08-22.** Android's Controller Link is a facility of a live BLE management relationship, not
   an independent transport: it may be established only while that peer holds a connected, bonded,
   encrypted management session, and it must be torn down when that session is genuinely lost.
   *Confirmed on hardware* that build `4b19842` did neither. Forcing a real management loss with
   `mgmt off` (which drops only the LE ACL, leaving the phone's Classic HID link untouched) left
   `cble.client false` with `classic_ready 1`, the tablet still listed in `btdev`, still holding
   active input ownership, and still streaming (`out:sent=41` 18 s later).
   `config_ble_handle_disconnect()` never referred to the Classic link its session had authorised,
   so there was no cleanup for the degraded session to have bypassed. Fixed by refusing
   cross-transport Classic admission with no live management session (at the HCI connection filter,
   before an ACL exists) and by tearing the latched companion ACL down on management disconnect
   through the existing Classic close path. Physical controllers are single-transport and cannot
   reach either change. `btauth` is now per-ACL, and `btstate` gained `auth:{deferrals,collisions}`
   (authentication-side `0x23` was previously counted nowhere) and
   `clink:{handle,refused_no_mgmt,mgmt_teardowns}`. Soak workloads A–D live in
   `tools/controller_link_cycle.py`. **Not claimed:** that the authentication collisions caused the
   CYW43/HCI wedge.

   **`bootsel` UART command added.** The adapter's BOOTSEL button is bound to pairing/personality
   gestures, and the running firmware exposed no way into the bootloader, so every soak iteration
   cost a physical unplug-and-hold. `bootsel` reboots via `reset_usb_boot()` — UART only, dev builds
   only, same trust boundary as `persona` and `mgmt off`. One manual BOOTSEL flash is still needed to
   *install* the build that carries it; every iteration after that is unattended.

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
- **Classic link keys are invisible to every enumeration surface (JoypadOS lineage `08a7e1e`).**
  `bond_snapshot_refresh()` walks `le_device_db` only, so UART `btbonds` and the management bond
  list can only ever show LE bonds. A bonded Classic controller that is powered off appears
  nowhere even though its key is persisted and it reconnects fine, and during the 2026-08-22
  investigation this is what made Mode-2 rejections unattributable. JoypadOS added
  `btstack_host_list_classic_bonds()` over `gap_link_key_iterator_*()`, which works on both the
  USB-dongle and CYW43 paths. Deliberately **not** done in this pass: making it user-visible needs
  management wire and UI changes too, which is not the "clearly separable, low-risk" bar. Note that
  the Settings string *"The stored-pairing list could not be read completely"* is a **different and
  smaller** issue — do not conflate the two. That section is explicitly scoped to LE bonds, and it
  renders on `bondsComplete != true`, which also catches `null`. `markBondsUnknown()` sets exactly
  that, so *"not read yet"* is presented to the user as *"could not be read"*. A three-state flag
  rendered as two states; the fix is to distinguish unread from truncated.
- **RSSI liveness probe cannot detect a dead remote (JoypadOS lineage `4486e7c`).**
  `HCI_Read_RSSI` is answered by the *local* controller, so a healthy reply proves nothing about the
  peer. JoypadOS removed theirs after it shot healthy links every 8 s under Classic+BLE coexistence
  load, when the CYW43 delayed the command-complete. We share the design flaw, but our telemetry
  shows it has never misfired (`probes 445/445`, `failed 0`, `timeouts 0`, `recovery.attempts 0`),
  so it is a cleanup task, not an incident. Do not remove it without new evidence implicating it.
- **Migrated to Pico SDK 2.3.0 / BTstack 1.8.2 on 2026-08-25; a stock-v1.8.2 Stage C candidate is
  under initial endurance and the CTKD hazard below remains unresolved.** The audit that follows was
  written
  on 2026-08-22 as a reason to stay on 2.2.0; the maintainer chose to modernize anyway, so it is now
  a **live risk on the candidate build**, not a deferral rationale. Its CTKD mechanism was
  re-verified from source in the installed `v1.8.2` tree during the migration and holds exactly as
  written: `sm_ctkd_fetch_br_edr_link_key()` loads the *existing* Classic key into
  `setup->sm_link_key` byte-reversed, and 1.8.2 now routes the BR/EDR→LE direction through
  `sm_store_classic_bonding_information()`, which stores it. v1.8.2 *does* carry a
  `sm_ctkd_from_le_could_update()` guard that is easy to mistake for `a0f82a97c` — it gates on
  authentication level, not on direction, and does not prevent this. Physical Classic controllers do
  not run SMP over BR/EDR and are unaffected; the Android companion is the exposed peer. Options and
  reachability analysis:
  [`docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md`](docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md)
- **The 2026-08-22 audit, retained verbatim as the evidence:**
  SDK 2.3.0 pins BTstack `075a0780f` (= `v1.8.2`), which does contain all three upstream fixes that
  looked relevant: `be7469f18` (connection collision — ignore `PAGE_TIMEOUT` after an incoming
  connection event), `9a82d560f` (page scan repetition mode R1), and `232f80e60` (derive the BR/EDR
  link key before sending DHKey Check). It does **not** contain the two commits that fix the CTKD
  rework `232f80e60` introduced: `f25861592` (store the derived link key in `sm_key_t` byte order)
  and `a0f82a97c` (*"store link key only for LE→BR/EDR key derivation"*, Fixes #744, authored by a
  Raspberry Pi engineer). In `v1.8.2`, `sm_process_bonding_information()` calls
  `sm_store_classic_bonding_information()` unconditionally, so in the BR/EDR→LE direction it writes
  the *existing* link key back in the wrong byte order and **corrupts the link key DB**. Both fixes
  landed 2026-08-18 and are upstream-master only. Upgrading the SDK today would therefore trade a
  known-good CTKD path for a known-broken one. Of the three "relevant" fixes, only `be7469f18`
  plausibly touches our paths at all, and the R1 change is about the *remote's*
  Page_Scan_Repetition_Mode in our outgoing `Create_Connection` — it cannot affect whether a phone
  can page us. Revisit when a Pico SDK ships a BTstack containing `a0f82a97c`.

  *(Still the correct read of the trade. What changed on 2026-08-25 is the decision and the initial
  hardware evidence, not the CTKD analysis: the host-side stack was modernized and its stock-v1.8.2
  candidate is under endurance. A later exact upstream pin containing the confirmed CTKD fixes is
  planned but is not present in the configured build.)*

## Negative knowledge — settled, do not rediscover

- **The Controller Link "cycling failure" is the tablet's Bluetooth controller, not the adapter.**
  Repeated management → Touch Gamepad → stop → end-management cycles eventually fail with
  management and Controller Link dropping together, `0x85` in the app, and self-recovery without an
  adapter reboot. Reproduced on cycle 3 of 10 over ADB against the flashed build. Adapter counters
  cleared every adapter-side theory in the same run: `reject_window` unchanged (not admission),
  `control_tick_max_gap_ms` pinned at its old 851 ms high-water mark (not core-1 starvation),
  `recovery.attempts`/`reboot.requests` still 0. Across **35 automated cycles** the dominant failure
  (8 of 10 in the 25-cycle campaign) is **Type C**: the ACL comes up in ~1 s, authentication
  **succeeds**, and then `SET_CONNECTION_ENCRYPTION` returns
  `HCI_ERR_LMP_ERR_TRANS_COLLISION` (0x23) in ~7 ms, Android tears the ACL down, and the app burns
  its ~21 s establishment budget. Which two LMP transactions collide is **Unknown**; the adapter's
  `LM_LINK_POLICY_ENABLE_SNIFF_MODE` is a candidate but was not changed on a hypothesis. In the same
  35 cycles Mode 2 never occurred at all and Mode 1 (`PAGE_TIMEOUT`) occurred **once**. The other
  two tablet-side failures are rarer and must not be merged with Type C or each other.
  **Type C is ours, and it is now located.** Android's sequence is structurally identical in all 8
  failures and 11 successes, so it is not the variable — sniff, packet-type change and duplicate
  connects are all specifically eliminated. Pinned BTstack sets `BONDING_SEND_ENCRYPTION_REQUEST`
  unconditionally on every successful Authentication Complete (`hci.c:4240`), without regard to who
  initiated authentication or whether this host requires encryption at all (our HID Host registers
  `LEVEL_0`). Both controllers report that event, so both hosts start the same LMP procedure — which
  is precisely what `LMP Error Transaction Collision` denotes. Upstream master is identical
  (`hci.c:5046`), so there is nothing to backport. Fixed in-repo without patching BTstack, because
  `hci.c:4708` notifies the application between the flag being set and `hci_run()` consuming it. *Type A*: Android's own Bluetooth stack aborts —
  `hci_layer.cc:255 handle_command_response: Waiting for READ_REMOTE_SUPPORTED_FEATURES(0x041b),
  got LINK_KEY_REQUEST_REPLY(0x040b)` → SIGABRT in `gd_stack_thread`, process dies and restarts.
  Android initiated that ACL and L2CAP itself and warns about the window in its own log ("*Maybe
  wait until read feature complete beforehand*"); no adapter behaviour was found to provoke it, and
  the Qualcomm patch reload is cleanup *after* the crash, not its cause. *Type B*: no crash at all —
  the tablet's controller reported both ACLs lost with HCI `0x08` and woke the host to say so. The
  2.35 s `SLEEP_IND` window there is **routine IBS UART idle, not an outage** (124 sleep cycles
  alongside 113 successful GATT round trips in the preceding ten healthy minutes), so the real
  outage length is unmeasured — do not quote 2.35 s as one. Awkwardly, the Classic link carries a
  20 s supervision timeout and reported `0x08` anyway only ~8.4 s after the last healthy round trip,
  so Type B's mechanism below the surviving host is **Unknown**; "the controller became
  unavailable" is the honest ceiling, and it is explicitly *not* established as a sleep/wake stall.
  One radio serves both transports, so both can be lost in the same episode; the 2026-08-25 trace
  reported LE first and Classic 15 s later rather than as simultaneous callbacks. `0x85` is
  *our* label for Android GATT 133 and is downstream; Fluoride separately prints `0x85` constantly
  as a BTM power-mode state, which is unrelated noise. The user-visible ~30–40 s recovery is
  ~2 s of real outage (process restart *and* controller firmware patch reload complete by T+1.9 s),
  then one doomed ~20 s app-side HID attempt, then retry latency. Do not re-open this as an adapter
  bug.
  [`docs/experiments/controller-link-cycling-failure-2026-08-22.md`](docs/experiments/controller-link-cycling-failure-2026-08-22.md)
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

- compiled C host tests — **one manifest, one command**: `pwsh -File tools/run_host_tests.ps1`
  builds all 72 declared tests from current source into a freshly emptied `build/host-tests` and
  runs only what it built. Covers protocol codecs, report encoders, motion PDU/seam invariants,
  BOOTSEL policy, wake identity, battery, audio control/resampler, virtual-tag store and v3 runtime
  replay, the keyboard/mouse mapping model, keyboard HID report decoding, settings-schema migration,
  the Bluetooth admission/liveness/lifecycle policy objects, and the seeded lifecycle model.
  `tools/run_mgmt_tests.ps1` is the `management` group plus its Python boundary suites. Nine further
  `tools/test_*.c` are declared non-suite (platform-dependent, obsolete, or a separate lab tool) in
  the runner's `$notHostTests` table — see also
  [`docs/host-test-inventory.md`](docs/host-test-inventory.md)
- Python suites for trace decoding, NFC semantics, the amiibo corpus, and motion analysis
- JVM tests for `:bridge-core` and the Android backends, plus the architecture guard. The Touch
  Gamepad's correctness lives here rather than on a device: contact ownership under reordered
  batches and non-contiguous identifiers, stick and D-pad geometry, authority transitions, release
  safety, and the declarative layout audited on real resolved geometry at seven window shapes and
  four densities
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
the Bluetooth Keyboard / Keyboard + Mouse input pass, the 2026-08-21 Android/Bluetooth reliability
pass, and the Touch Gamepad on top of it.

The reliability pass is physically accepted. Confirmed on hardware: fresh LE management pairing,
repeated Refresh, personality switching with post-transition management recovery, DualSense Edge +
BLE management coexistence, Controller Link ↔ physical-controller source switching, and a ≥75-minute
mixed soak with continuous controller audio and no controller, management, or bond loss. The bounded
HCI/CYW43 OFF/ON recovery it added has still never fired on hardware — its logic is host-tested, but
the recovery path itself remains unvalidated in the field.

The Classic controller lifecycle pass is physically accepted (see the lead entry). One measurement
from that investigation is unexplained and worth knowing before it is rediscovered: on 2026-08-29,
**every background `input sources` poll took ~8.0 s against the app's 10 s timeout** — 40+
consecutive samples, while foreground `peers` (352 bytes) and `device` on the same GATT link and the
same callback thread completed in 153 ms and 164 ms. One Paired Controllers refresh timed out during
that session; the ring shows `mgmt_disconnect` reason `0x13`, so **Android closed the link and the
adapter re-advertised 26 ms later** — the firmware did not stall, and the peers/pagination/session
paths were audited and found sound. Ruled out for the 8 s: `cmd_input_sources` is fully inline, core
0 was responsive, no USB host was attached so the documented TinyUSB ISO-endpoint starvation path was
inactive, and the LE management link requests latency 0 with a 7.5–50 ms interval. The firmware has
no concept of foreground versus background, so the difference originates on the Android side —
possibly aggravated by the adapter's continuous Classic inquiry (3 s bursts every ~13 s while idle,
by design). **Unproven, and deliberately not "fixed" by raising a timeout.** The decisive next step
is a timestamp across the management exchange (write received → reply published) so a slow adapter
and a slow transport stop being indistinguishable.

The open items above are hardware gates to close opportunistically when the relevant hardware is in
front of the maintainer. KB/M (gate 9) is implementation-complete and host-validated with nothing
hardware-confirmed. The Touch Gamepad (gate 10) is no longer release-blocking: the stuck-input
sub-item was the reason it was, and on 2026-08-24 the maintainer confirmed that losing focus,
sleeping and app-switching all release held inputs on two Android devices, and that disconnect and
reconnect leave a real Switch neutral. What is left of gate 10 is breadth rather than safety —
control-by-control coverage per console personality, held-contact profile changes with the Classic
link retained, and in-game feel. The next accepted engineering work is the current development
priority in [`PLAN.md`](PLAN.md).
