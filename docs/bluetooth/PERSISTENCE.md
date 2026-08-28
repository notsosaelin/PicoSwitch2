# Bluetooth persistence and reset contract

Status: exact source and linker/build-layout audit on 2026-08-20. Logical deletion and builds are
source-tested; physical flash/reboot observations for this correction are pending.

## Physical flash map

Flash sectors are 4 KiB. The production SDK supplies two alternating BTstack TLV banks. RP2350 A2
reserves the final sector, so its BTstack base is one sector lower than RP2040.

| Purpose | Pico W, 2 MiB | Pico 2 W, 4 MiB |
|---|---:|---:|
| Virtual Amiibo bank 0 | `0x1FA000` | `0x3FA000` |
| Virtual Amiibo bank 1 | `0x1FB000` | `0x3FB000` |
| PicoSwitch2 configuration | `0x1FC000` | `0x3FC000` |
| Unused by these stores / BTstack TLV A | `0x1FD000` | `0x3FD000` |
| BTstack TLV A / TLV B | `0x1FE000` | `0x3FE000` |
| BTstack TLV B / SDK-reserved final sector | `0x1FF000` | `0x3FF000` |

The table's slash-separated row reflects the board difference: Pico W TLV occupies the final two
sectors; Pico 2 W TLV occupies sectors `SIZE-3S` and `SIZE-2S`.

## Logical stores in the BTstack TLV banks

The Pico SDK `btstack_cyw43_init()` installs one TLV instance, then attaches both databases:

| Tag family | Owner | Contents | Capacity |
|---|---|---|---:|
| `BTL0`…`BTL15` | BTstack Classic link-key DB | address, link key, key type, sequence | 16 |
| `BTD0`…`BTD15` | BTstack LE device DB | identity/type, IRK, LTK, EDIV/RAND, security flags | 16 |
| `JPLC` | PicoSwitch2 | preferred BLE address/type/name, VID/PID, optional Switch 2 LTK | 1 |
| `JPLK` | PicoSwitch2 | post-wipe/install admission lock byte | 1 |

`JPLC` is a reconnect hint plus Switch 2 custom-key record; it is not a substitute for the BTstack
bond database. Global wipe and forget-one for the matching identity MUST clear the whole record and
all in-memory copies. A zero-address record with residual key bytes MUST NOT be written.

The LE backend permits sparse slots. `le_device_db_count()` is a count, not a traversal bound.
Enumeration and deletion MUST visit `0..le_device_db_max_count()-1` and skip empty slots.

## Public deletion semantics

PicoSwitch2 uses:

- `gap_drop_link_key_for_bd_addr()` / `gap_delete_all_link_keys()` for Classic trust;
- `gap_delete_bonding(address_type, address)` for LE trust.

Direct `le_device_db_remove()` calls are not the product deletion boundary. In the pinned SM,
`gap_delete_bonding()` removes the database entry and reloads the controller resolving list when LE
privacy resolution is enabled. Wipe and forget MUST preserve those side effects.

## Operation matrix

| Operation | Settings | Amiibo | Classic keys | LE bonds | `JPLC` / Switch 2 key | `JPLK` | Admission afterward |
|---|---|---|---|---|---|---|---|
| Ordinary reboot | preserve | preserve | preserve | preserve | preserve | preserve | existing policy |
| Disconnect all | preserve | preserve | preserve | preserve | target remains | preserve | no new trust without window |
| Forget one LE identity | preserve | preserve | preserve | delete matching typed entry | clear if matching | preserve | closed unless window open |
| Triple-tap wipe all | preserve | preserve | delete all | delete all, including management | delete | store locked | closed |
| New UF2 first boot | reset | reset both banks | erase | erase | erase, then absent | recreate locked | closed |
| Reboot of same installed image | preserve | preserve | preserve | preserve | preserve | preserve | existing policy |
| Full-chip erase | erase | erase | erase | erase | erase | erase | closed by default fresh-admission policy |

The management `bonds remove <index>` surface is LE-only. Classic per-device deletion has no public
management command; triple-tap remains the user-facing global operation.

## Peer roles

A bond entry is one security record for one transport. A **peer** is one physical device, and may
own several entries — with `ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` configured, the management phone
normally owns two. `peers list` (see [management/PROTOCOL.md](../management/PROTOCOL.md)) reports
peers, merged by identity address, read-only, and without key material of any kind.

| Role | Evidence that establishes it | Persistent? |
|---|---|---|
| `management` | Connected as the BLE peripheral's management client (`config_ble.client_addr`) | No |
| `controller_link` | A connected Classic HID peer the arbiter classified as the Android bridge from its HID descriptor | No |
| `controller` | A connected controller source, or a match against the `JPLC` reconnect record | Partly — `JPLC` survives reboot |
| `unknown` | Everything else | n/a |

The roles are ordered: `management` wins over `controller_link`, which wins over `controller`. One
phone can hold both relationships simultaneously, and reporting it as a controller would put the
user's own phone in a list of things to forget.

**Role classification is live evidence only.** There is no persistent per-peer role store, so after
a reboot a stored bond whose owner has not yet reconnected is `unknown`. That is the correct answer,
not a gap to be papered over with a guess: the security databases record an address, a key and a key
type, and nothing about what the device is.

Since Phase 4 the adapter also reports, for a **connected** peer only, the bthid driver identity it
derived (`class`) and the resolved `vid`/`pid`. Those are live too, and are absent for a peer that is
merely bonded.

### Persisting peer metadata was tried, and withdrawn

> **Do not reimplement this without new evidence.** Storing non-secret peer metadata on the adapter
> — a project-owned TLV tag beside `JPLC`/`JPLK`, advisory rather than authoritative, erased by the
> install reset exactly as bonds are — is the obvious way to make role and name survive a reboot,
> and it is what the design document recommends (`BT_MANAGEMENT_UPGRADE_PASS.md` §24.2). It was
> implemented after Phase 4's first pass and it **destabilised the Bluetooth core**. It was
> withdrawn, and Phase 4 shipped with the companion remembering peer metadata instead.
>
> The storage audit's capacity finding still stands; capacity was never what failed. What actually
> destabilised the core has **not** been established. The untested first hypothesis is TLV write
> contention with the security databases, which share the same flash banks. Establish the cause
> before rebuilding the same shape.

The consequence to accept meanwhile: peer names and roles that survive a reboot live on the
management phone, so a different phone sees only the adapter's live answers. See
[`../android-companion.md`](../android-companion.md).

### Known limitation

Merging is by identity address. A dual-mode device whose LE identity address differs from its
Classic BD_ADDR appears as two `unknown` peers rather than one. That is incomplete rather than
wrong, and is preferred over asserting an association the adapter cannot demonstrate.

## Install-reset marker

Every UF2 carries a dedicated pending marker page in the application image. On the first boot after
that UF2 is written, core 0 runs before USB, CYW43 and core 1:

1. Validate marker placement below the reserved persistence boundary.
2. Erase the complete six-sector region (`SIZE-6S` through end of flash).
3. Program the marker page from its pending magic to zero.
4. Load default settings and an empty Amiibo store.
5. Initialize BTstack/TLV on core 1.
6. Recreate `JPLK=1` before enabling discovery or Classic connectability.

Erase precedes marker consumption. Power loss before consumption repeats the erase safely. Power
loss after consumption but before `JPLK` recreation still leaves no trust material; the independent
fresh-admission rule rejects new trust while no explicit window is open.

The previous implementation erased only five sectors while the verifier and reserved layout began
at `SIZE-6S`. That left Amiibo bank 0 outside the reset despite a log claiming all slots were reset.
The 2026-08-20 correction makes the runtime erase match the six-sector contract.

## Firmware flashing terminology

“UF2 flashed” is not enough evidence to identify what happened. For the release UF2 path above, a
newly written marker deliberately resets persistent state once. A debug/programmer operation that
does not rewrite the marker page may preserve state. A full-chip erase removes everything. Test
reports MUST name the exact flashing method and record bond state before the remote is powered on.

## Security and observability

Diagnostics MAY expose slot count, address/type, reconnect phase and security outcome. They MUST NOT
expose Classic keys, LTKs, IRKs, Switch 2 key components, PIN bytes, or raw TLV records containing
secrets.
