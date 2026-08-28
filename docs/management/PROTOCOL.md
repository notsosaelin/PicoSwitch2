# PicoSwitch2 logical management protocol

Status: Source-Tested normative reference for the current repository revision

## Requirement terminology

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** in this document are to
be interpreted as described in BCP 14, RFC 2119 and RFC 8174, when and only when they appear in all
capitals.

## Scope and authority

This document specifies logical commands and replies. It does not specify how a platform discovers,
pairs with, or reconnects to an adapter. BLE UUIDs, security, fragmentation, and session ownership
are in [TRANSPORTS.md](TRANSPORTS.md).

Firmware `src/config.c` is the current command/formatter authority. The BLE subset is the allowlist
in `config_wireless_command_allowed`; bond cursor formatting is in `src/mgmt_bonds.c`. The portable
reference is `android/companion/management-core`, and representative wire values are in
`tools/fixtures/management/protocol-v1.json`.

## Common wire rules

- A command is non-empty UTF-8 text terminated by LF (`0x0A`). CR is ignored by the firmware
  assembler. A command MUST NOT contain an embedded CR or LF.
- The command payload before LF is at most 127 bytes. The BLE assembler discards an overlong line
  through its terminating LF.
- A reply is one UTF-8 JSON object followed by LF. Clients MAY accept a preceding CR for CDC
  interoperability.
- The BLE reply slot is 512 bytes including LF, so its JSON payload is at most 511 bytes. Firmware
  substitutes `{"error":"response_too_large","code":413}` instead of publishing a partial reply.
- Exactly one logical reply belongs to one logical command. There are no carrier transaction IDs.
  The general persistence counters described below identify save work only; they do not correlate
  arbitrary commands or permit concurrent BLE transactions.
- JSON object field order is not significant. Clients MUST ignore unknown fields and MUST validate
  the fields required by the operation they requested.
- Integers are JSON numbers unless a field is explicitly hexadecimal text. Boolean fields are JSON
  booleans; the `device` battery flags historically permit `0`/`1` and should be accepted as such.

## Errors

Any command can return an object containing an `error` string and optional integer `code`, for
example `{"error":"unknown command"}` or the 413 reply above. The client MUST treat the presence of
`error` as failure even if other fields are present. Common source-defined errors include bad/usage
arguments, unavailable command/personality, busy, timeout, unknown command, no such bond, and
Virtual Amiibo result strings/codes.

Malformed JSON, a missing required field, invalid hex, inconsistent paging metadata, or a reply
larger than the carrier limit is a client-visible protocol failure, not a successful empty/default
value.

## Transport notation

The tables use:

- **BLE** — bonded/encrypted in-band GATT allowlist;
- **Config CDC** — the current CDC-only Config USB personality using the same parser;
- **NS2_PRO** — compiled only in the Pico 2 W audio-capable production configuration.

Unless a row says otherwise, mutations apply in RAM immediately and require a separate `save` if
they are settings that should survive reboot. A mutation acknowledgement is not authoritative
readback.

## Identity, configuration, and source inventory

| Command | Class / transports | Reply and semantics | Persistence / readback | Source / vector |
|---|---|---|---|---|
| `ping` | read; BLE, Config CDC | `{"ok":true}` | none | `handle_line`; `ping` vector |
| `info` | read; BLE, Config CDC | `id`, `product`, `version`, `bridge_contract`, `build` | none; use for build/Bridge compatibility, not a management-version negotiation | `handle_line`; `info` vector |
| `get` | read; BLE, Config CDC | `body_color`, `joycon2_left_accent`, `joycon2_right_accent` as three integers; `lightbar` is a legacy read-only alias repeating body color | authoritative color read | `cmd_get`; `config` vector |
| `device` | read; BLE, Config CDC | controller `name`, `vid`, `pid`, `batteryValid`, `battery`, `charging` | session snapshot | `cmd_device`; `device` vector |
| `input sources` | read; BLE, Config CDC | `active`, `pending`, `explicit`, `fresh`, `transitions`, `more`, and `sources[]` entries with `id`, `conn`, `transport`, `generation`, `name` | session snapshot; `more=true` means registry output was truncated, not paged | `cmd_input_sources`; `inputSources` vector |
| `input active <id\|none>` | mutation; BLE, Config CDC | acknowledgement containing `ok`, active-selection fields may also be returned | RAM-immediate; client reads `input sources` after acknowledgement | `cmd_input_active`; workflow tests |
| `body <r> <g> <b>` | mutation; BLE, Config CDC | `{"ok":true}` | RAM-immediate; `save`, then `get`; each component 0..255 | `handle_line`; workflow tests |
| `jcl <r> <g> <b>` / `jcr ...` | mutation; BLE, Config CDC | `{"ok":true}` | as above, for Joy-Con 2 accents | `handle_line`; workflow tests |
| `lb 0 <r> <g> <b>` | compatibility mutation; BLE, Config CDC | `{"ok":true}`; only slot 0 accepted | legacy alias for body color; new clients SHOULD use `body` | `handle_line` |

Source IDs are unsigned 32-bit values expressed in decimal. `none` selects the neutral/no-owner
state. A client MUST NOT synthesize or merge source identities.

## Personality, wake, management, bonds, and save

| Command | Class / transports | Reply and semantics | Side effects / required follow-up | Source / vector |
|---|---|---|---|---|
| `personality` | read; BLE, Config CDC | `current`; `available[]`; identifiers `pro2`, `gc`, `jcl`, `jcr`; `current` can also be `config` | none | `cmd_personality`; `personality` vector |
| `personality <pro2\|gc\|jcl\|jcr>` | mutation; BLE, Config CDC | `ok` plus `switching:true`, or `unchanged:true` | queues USB personality switch; BLE session is independent; Config CDC can disappear | `cmd_personality_set`; workflow tests |
| `reenumerate` | mutation; BLE, Config CDC | `{"ok":true,"reenumerating":true}` | queues detach/re-enumeration of the current controller identity; rejected in Config personality | `cmd_reenumerate`; workflow tests |
| `wake` | mutation; BLE, Config CDC; NS2_PRO | `ok`, `queued:true`, `result:"pending"` | delivery only; poll `wake status` | `handle_line`; workflow tests |
| `wake status` | read; BLE, Config CDC; NS2_PRO | `result`, `consoleAsleep`, `identityValid`, `attempts`, `lastAttemptMs` | terminal results: `advertised`, `console_awake`, `no_identity`, `radio_busy`; `pending` may remain | `handle_line`; `wakeStatus` vector |
| `mgmt` / `mgmt status` | read; BLE, Config CDC | `ok`, `enabled` | reports RAM-only in-band gate | `cmd_mgmt`; `management` vector |
| `mgmt on\|off` | mutation; BLE, Config CDC | `ok`, `enabled` | RAM-only; production reboot restores on. `off` can end the BLE session after its reply | `cmd_mgmt`; workflow tests |
| `bonds list` | read; BLE, Config CDC | if bounded: v2 envelope described below; if not: error 413 | no partial success | `cmd_bonds`, `mgmt_bonds_format_legacy` |
| `bonds list v2 [cursor]` | paged read; BLE, Config CDC | `v:2`, `total`, `bonds[]`, `next` integer or null | follow `next` until null; validate stable total and unique indices | `mgmt_bonds_format_page`; `bonds_page` vector |
| `bonds remove <index>` | mutation; BLE, Config CDC | `{"ok":true}` or error | removes an adapter-side LE bond; list again. It is distinct from Android association/bond state and may interrupt the affected peer | `cmd_bonds`; workflow tests |
| `peers list [cursor]` | paged read; BLE, Config CDC | `v:1`, `total`, `peers[]`, `next` integer or null | read-only logical peer inventory; follow `next` until null | `cmd_peers`, `mgmt_peers_format_page`; `peersPage` vector |
| `save` | mutation; BLE, Config CDC | BLE: `ok`, `queued:true`, `requested`; CDC: `ok`, `requested` only after the synchronous wait, or `save timeout` | `requested` is the session-local unsigned 32-bit identity assigned to this persistence request. BLE acceptance is not durable completion | `handle_line`; `saveQueued` vector |
| `save status` | read; BLE, Config CDC | `pending`, `requested`, `completed` | authoritative general-settings persistence snapshot; `pending` is exactly `requested != completed` | `handle_line`; `saveStatus` vector |

Bond entries use `i` (device-DB index), `addr`, and may include `name` and `type`. Clients MAY accept
the historical `index`/`address` aliases. A v2 total changing between pages, a repeated index,
non-progressing cursor, empty page with non-null cursor, or final count different from `total` is a
pagination failure. A legacy/unversioned bounded list cannot prove completeness and must be labeled
accordingly.

### Peers are not bonds

`bonds` enumerates LE device-DB **slots**. `peers` enumerates logical **devices**: the Classic
link-key store and the LE device DB merged by identity address, so one physical device is one row
however many security records it holds. That distinction is load-bearing, not cosmetic — this
firmware builds with cross-transport key derivation, and the management phone routinely holds a
Classic record *and* an LE record. The two commands answer different questions and neither replaces
the other.

Peer entries carry:

| Field | Meaning |
|---|---|
| `id` | Opaque, stable, non-secret handle derived from the identity address. Deterministic across reboots. Clients MUST treat it as opaque and MUST NOT parse it, and MUST NOT substitute a database index, which is reused. |
| `addr` | Identity address, 12 uppercase hex digits. |
| `tr` | Transport bitmask: `1` BR/EDR, `2` LE, `3` both. `0` means connected with no stored key. |
| `role` | `management`, `controller_link`, `controller`, or `unknown`. |
| `bonded` | Whether any security record exists for this peer. |
| `conn` | Whether the peer is connected right now. |
| `name` | Optional. Sanitised to printable ASCII by the adapter. |

Pagination rules are identical to `bonds list v2`, and so are the failure conditions: a changed
`total`, a repeated `id`, a non-progressing cursor, an empty page with a non-null cursor, or a final
count different from `total`. Clients MUST treat any of these as a failure rather than as a shorter
inventory — a dropped row is a saved controller the user cannot see.

**`role` is evidence, not a label.** The adapter classifies from what it can currently observe: the
connected management client, the Controller Link peer identified from its HID descriptor, connected
controllers, and the stored reconnect record. It has no persistent role metadata, so a stored bond
whose owner has not been seen since boot is reported `unknown`. Clients MUST render `unknown` as
unidentified and MUST NOT promote it to `controller`. Clients MUST also tolerate role values they do
not recognise by treating them as `unknown`, so a newer adapter's vocabulary cannot make a page
unreadable.

`management` outranks `controller_link` and `controller` when one device qualifies for more than
one, because a single phone can hold the management session and the Controller Link relationship at
the same time. Presenting that phone under saved controllers is the specific error the ordering
prevents.

**No key material appears in any peer field, in any protocol version.** Link keys, LTKs, IRKs and
CSRKs MUST NOT be exposed. The firmware's peer record has no field capable of holding one; that is
the mechanism, not merely the intention.

`bridge_contract` belongs to Controller Bridge compatibility. There is no separate negotiated
management protocol version. Compatibility is currently exact command behavior, optional-family
fallback through `unknown command`/`unavailable`, bond envelope `v:2`, tolerant unknown JSON fields,
and firmware build identity.

General persistence identities start at zero on boot and advance modulo 2^32 for every settings
save request, including automatic/internal requests. `completed` advances only after the actual
settings-sector erase/program finishes. A client waiting for the ID returned by `save` considers
that request complete when `completed` has reached it in modulo-uint32 order; a later automatic
request may therefore leave `pending:true` even though the client's earlier request is complete.
Clients MUST use a bounded poll and MUST NOT interpret `queued:true` alone as durable completion.
Older firmware can omit `requested` and return `unknown command` for `save status`; clients may
still request a legacy save but cannot claim authoritative completion on that firmware.

## Keyboard and mouse

KB/M identifiers are protocol identifiers, not UI labels.

| Command | Reply / mutation | Required behavior |
|---|---|---|
| `kbm` / `kbm status` | status object | Read live/effective state. `mode` is effective, `override` is configured choice, and `profile` is active mapping profile. Required booleans are `keyboard`, `mouse`, `nativeMouse`; counters/generations are diagnostic fields. |
| `kbm mode` | `mode`, `override`, `available` | Read the mode summary. |
| `kbm mode <auto\|controller\|keyboard\|kbmouse>` | acknowledgement with selected `mode` | RAM-immediate. Read `kbm status` for authoritative effective state; effective mode can differ from override. |
| `kbm map <kb\|kbm> [page]` | `profile`, `page`, `pageSize`, `total`, `bindings[]`, `more` | Page is 0..32; firmware page size is currently 8. Assemble until `more=false`; total/profile/page must remain coherent. |
| `kbm bind <kb\|kbm> <source> <destination\|none\|default>` | acknowledgement naming profile/source/destination | `none` is an explicit neutral binding. `default` clears the override and restores the canonical effective binding. Read the complete profile map after mutation. |
| `kbm reset [kb\|kbm\|all]` | acknowledgement naming reset target | Empty target and `all` reset mode, mappings, and mouse values; profile target resets that profile. Read affected state/maps afterward. |
| `kbm mouse` | mouse configuration and bounds | Read values and adapter-reported limits. |
| `kbm mouse <field> <value>` | same complete mouse object | Firmware validates the value and returns authoritative state. UI debounce/coalescing is client policy, not wire semantics. |

Profiles are `kb` and `kbm`. Sources are `key:NN`, where `NN` is hexadecimal HID keyboard usage
`04`..`E7`, or `mouse:N`, where `N` is 1..5. Destinations are:

```text
none a b x y l r zl zr gl gr l3 r3
dup ddown dleft dright minus plus home capture c
lstick_up lstick_down lstick_left lstick_right
rstick_up rstick_down rstick_left rstick_right
```

Each binding has `src`, `dst`, and `custom`. Mouse fields are `sensitivity`, `sensitivityx`,
`sensitivityy`, `recenter`, `invertx`, `inverty`, and `antideadzone`. The mouse reply fields are
`sensitivityX`, `sensitivityY`, `recenterMs`, `invertX`, `invertY`, `antiDeadzone`, plus
`sensitivityMin`, `sensitivityMax`, `recenterMinMs`, `recenterMaxMs`, and `antiDeadzoneMax`. Clients
MUST use those reported bounds rather than duplicate firmware limits.

## Virtual Amiibo adapter management

Supported image sizes are 540, 572, and 2048 bytes. Binary data and CRC32 are uppercase or lowercase
hex text without separators; firmware parsing is case-insensitive. Transfer chunks contain 1..32
bytes.

| Command | Reply / effect | Workflow rule |
|---|---|---|
| `amiibo status` | `loaded`, `dirty`, `presented`, `v3loaded`, `persisted`, `persistPending`, `size`, `signature`, `hasSave2`, `usingSave2`, `generation`, `payloadCrc`, `uid`, `figureId`, nested `upload` | Read before destructive replacement/clear and while waiting for persistence. |
| `amiibo begin <size> <crc32>` | acknowledgement or Virtual Amiibo error | Starts isolated transactional upload for the size/CRC. |
| `amiibo chunk <offset> <hex>` | acknowledgement or error | Send contiguous chunks of at most 32 bytes. |
| `amiibo commit` | acknowledgement or validation error | Commit primary/normal image. |
| `amiibo commit save2` / legacy `commit used` | acknowledgement | Commit console-written secondary copy for supported standard tags. |
| `amiibo cancel` | acknowledgement | Aborts an upload. A client SHOULD send it best-effort after any post-begin failure. |
| `amiibo read <offset> <1-32>` | `offset`, `data` | Download selected/current bytes. Allocate only after validating status size; verify returned count and payload CRC where available. |
| `amiibo read save1 ...` / `read save2 ...` | as above | Explicit standard-tag copy; aliases `clean`/`used` are accepted. v3 does not expose two copies. |
| `amiibo downloaded` | acknowledgement | Acknowledges successful sync; a client SHOULD verify generation/CRC did not change first. |
| `amiibo select save1\|save2` | acknowledgement or error | Select active standard-tag copy; aliases `clean`/`used` accepted. |
| `amiibo present` / `amiibo eject` | acknowledgement | Changes presented state. |
| `amiibo persist` | BLE queued acknowledgement; CDC synchronous result | Request flash persistence, then poll status until `persisted=true` and `persistPending=false`. |
| `amiibo clear` | BLE queued acknowledgement; CDC synchronous result | Do not clear dirty data before sync; poll until no image and no pending persistence. |

An upload is `status -> begin -> chunks -> commit -> persist -> status polling`. The reference client
uses a six-second workflow timeout for persistence polling; that timeout is client policy, not a
wire constant. Download is `status -> reads -> CRC verification`; acknowledgement/persist of the
downloaded state is a distinct operation.

Under `NS2_PRO`, firmware also exposes low-level `amiibo reader on`, `reader off`,
`reader send <hex>`, and `reader reply` for controller-as-reader research. They require a connected
genuine Pro Controller 2 and explicit NFC sequencing. They are not a general high-level Virtual
Amiibo workflow and are intentionally not wrapped by the portable `ManagementClient`.

## Commands outside production BLE management

`state`, audio/motion diagnostics, raw capture, firmware-read tracing, `sw2cap`, and `btid` remain
diagnostic Config CDC/UART surfaces and are rejected by the BLE allowlist. The UART KB/M diagnostic
surface shares KB/M parsers/formatters but is not the connected management transport described here.

## Persistence and side-effect summary

- Color and KB/M settings apply in RAM and require `save` for persistence.
- `mgmt off` is RAM-only by design and resets to enabled at reboot.
- Active input selection is runtime state.
- Personality mutation queues USB switching; `reenumerate` queues a USB detach/reconnect.
- Virtual Amiibo has its own `persist`/`clear` lifecycle and status flags.
- LE bonds live in the BTstack device DB and are removed with bond operations or install reset.
- `queued:true` is acceptance of deferred work, never proof that the flash write completed; use the
  acknowledged `requested` identity with `save status` when durable completion matters.

## Non-normative examples

The JSON objects in `tools/fixtures/management/protocol-v1.json` are representative firmware-shaped
examples. They are executable conformance vectors but do not replace required-field rules above.
