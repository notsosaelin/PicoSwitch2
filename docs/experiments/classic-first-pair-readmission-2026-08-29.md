# Classic first-pair failure: inquiry re-admission destroys the candidate

Date: 2026-08-29
Status: Root cause **Confirmed** from hardware evidence plus source. Fix implemented; fix itself
is **not** hardware-validated.

## Question

A freshly wiped and freshly flashed adapter paired a DualSense once. The controller connected and
worked, but:

- the app reported it as the generic gamepad, not `Sony DualSense`;
- it had no player-slot LED;
- the adapter-configured controller colour was not applied.

Those last two are driver **initialisation** effects, not display metadata, so the question was
whether the connection had ever been owned by the DS5 driver at all — and whether that shared a
cause with the classification and with the missing Classic link key.

## Environment

| Item | Value |
|---|---|
| Adapter build | `a05083ec+dirty` (reported by `bridge`) |
| Includes | `8be6ce0` Classic key persistence fix, `a05083e` provisional-classification fix |
| Board | Pico 2 W, personality `pro2` |
| Controller | Sony DualSense, Classic BT, address suffix `79:33:86` |
| Channel | CP210x UART diagnostic surface (`tools/uart_query.ps1`) |
| Prior state | freshly wiped, freshly flashed, one pairing performed |

The controller had **already disconnected** by the time UART access was available
(`hci_disconnect` reason `0x13`, remote user terminated), so the live BTHID device table could not
be read. The `btlife` ring preserved the session and supplied the decisive evidence instead.

## Captured state (read-only)

```
btbonds  {"btbonds":[]}
btpeers  {"v":1,"total":0,"peers":[],"next":null}
btdev    {"btdev":[]}
btauth   {"btauth":"none"}
btreject {"btreject":"last","addr":"FE:20:11:18:93:4B","trust_present":false}
btlife   admission:{fresh_accepted:0, reject_window:3}
         auth:{deferrals:0, collisions:0}  enc:{deferrals:0, peer_completed:0, collisions:0}
         disc:{hci:1, last_handle:"0x000B", last_reason:"0x13"}
```

Session timeline, from `btlife dump` (ms since boot, addresses truncated as the ring stores them):

```
33023  acl_up        h=11  793386
33102  link_key_req  have_key=0
35023  inquiry_start                 <- rediscovery, mid-connection
35313  inquiry_start                 <- again
35475  inquiry_start                 <- again
35482  enc_change    status=0 enabled=1 key_size=0
35953  hid_ready     key_size=16  793386
209830 hci_disconnect reason=0x13 h=11
```

No `auth_start` and no `auth_done` appear anywhere in the ring.

## Findings

**1. The pairing was peer-led, as `8be6ce0` predicted.** `link_key_req` with `have_key=0`, then a
successful `enc_change`, then `hid_ready` at a 16-byte key size — with no locally requested
authentication and no Authentication Complete. This is direct hardware corroboration of that
commit's reasoning. Note the ring has **no event type for Link Key Notification**, so its absence
from the dump is not evidence either way.

**2. The link key was still not stored.** `btbonds` empty after a successful, encrypted pairing —
so the Encryption-Change proof added in `8be6ce0` did not commit. `admission.fresh_accepted` is 0,
and the controller's later reconnect attempts were refused (`reject_window: 3`,
`trust_present: false`).

**3. Inquiry restarted three times inside the 2.5 s between `acl_up` and `enc_change`.** With the
pairing window open, `ns2_bt_inquiry_restart_delay_ms()` returns 0, and a controller stays
discoverable throughout its own pairing — so the device being connected is rediscovered repeatedly,
mid-connection.

**4. `GAP_EVENT_INQUIRY_RESULT` guarded only against a duplicate INCOMING attempt.** An outgoing
attempt in flight to the same device was re-admitted, and re-admission rebuilds the candidate:

- `pending_name` is overwritten with the new result's name, **frequently empty** because an EIR
  need not repeat it. That name is what the `classic_connection_t` slot is built from and therefore
  what the driver match sees — so the device binds `bthid_gamepad_driver` instead of
  `ds5_bt_driver`, never runs DS5 initialisation (player-slot LED, configured colour), and the peer
  inventory reports the generic driver.
- `classic_pending_security_prepare()` runs again, clearing the parked link key that the Encryption
  Change was about to commit.
- A second `hid_host_connect()` is issued and another connection slot allocated for the same
  address.

## Conclusion

**One defect produced all three symptoms.** The classification, the missing player-slot LED and the
missing configured colour are the same failure — the connection was never promoted to the DS5
driver, because the evidence it would have matched on was destroyed by a rediscovery of a
connection already in progress. The missing link key is the same event clearing the parked
credential.

A second pairing "fixed" identification only because its timing differed: no rediscovery landed
inside the critical window.

Confidence: **Confirmed** for the mechanism (source plus the ring above). The fix is
implementation-complete and host-tested but **not** hardware-validated.

## Fix

`ns2_bt_classic_inquiry_admission_is_duplicate()` — suppress an inquiry result for a device that
already has an attempt in flight or a live ACL, in either direction. Both terms are required:
before the ACL exists the pending record is the evidence; after HID open clears that record, the
ACL is.

## Remaining unknowns

- Whether the PnP SDP VID/PID query would additionally have rescued the classification in this
  session is **unknown** — the connection had ended before the device table could be read, so
  neither the resolved VID/PID nor the bound driver was directly observed.
- Whether `8be6ce0`'s Encryption-Change proof commits correctly once re-admission no longer wipes
  the parked key is untested on hardware. This session cannot show it, because the key was cleared
  before the proof arrived.

## Suggested follow-up

One pairing on the fixed build, then `btbonds` (expect one BR/EDR entry), `btdev` (expect
`Sony DualSense`), and `btlife dump` (expect no `inquiry_start` between `acl_up` and `hid_ready`).
