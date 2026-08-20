# Bluetooth architecture

Status: source-confirmed against Pico SDK 2.2.0 and the production Pico W/Pico 2 W builds on
2026-08-20. Physical coexistence regression remains pending.

## Context

```text
physical controller(s)                      management client
 Classic HID or LE HID/custom ATT           bonded LE GATT peripheral
             \                                  /
              \                                /
               CYW43 radio + HCI + one BTstack instance
                              |
                    btstack_host.c (core 1)
                  /            |              \
       controller transport   radio policy   config_ble service
                |                |                    |
        joypad-os bthid    scan/inquiry/wake     wireless bridge
                |                                     |
       source registry/arbiter                  config parser (core 0)
                |
       one logical console input owner
                |
       Switch 2 USB personality (core 0)
```

## Pinned stack

The production build uses:

- Pico SDK `2.2.0`, repository revision `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`;
- BTstack submodule revision `501e6d2b86e6c92bfb9c390bcf55709938e25ac1`
  (`v1.6.2-1-g501e6d2b8`);
- `BTSTACK_USE_CYW43=1`, Classic and LE enabled, privacy resolution enabled;
- 16 Classic key slots and 16 LE device-database slots.

Pinned source, not current upstream BTstack, defines production behavior.

## Ownership

| State or operation | Owner | Cross-core rule |
|---|---|---|
| HCI, GAP, SM, GATT, HID Host, scan and inquiry | BTstack run loop, core 1 | Core 0 MUST NOT call these directly |
| Classic link-key DB and LE device DB | BTstack/TLV, core 1 | List/remove requests are marshalled to core 1 |
| Controller connection tables and reconnect target | `btstack_host.c`, core 1 | Core 0 reads bounded snapshots only |
| Management command parsing and settings | `config.c`, core 0 | Wireless commands use the bridge request/response seam |
| Active input ownership | source registry/arbiter, core 1 | Additional peers do not imply merged controller ownership |
| USB personalities and console reports | core 0 | Input crosses through existing snapshots/seams |
| BOOTSEL sampling | core 0 with cooperative core-1 park | Gesture policy is dispatched on core 1 |

Persistent trust mutation MUST occur on the BTstack/run-loop context. The management `bonds`
commands therefore queue an operation and publish completion instead of reading or mutating the LE
database from core 0.

## Radio roles

One BTstack instance simultaneously supports:

- BR/EDR HID host for Classic controllers, keyboards and mice;
- LE central/client for standard HIDS controllers and the Switch 2 custom ATT path;
- LE peripheral/server for bonded, encrypted management;
- short non-connectable wake advertisements.

These roles share the CYW43 radio and LE device database, but not their connection lifecycles.
Ordinary controller disconnect MUST NOT tear down management. Ordinary management disconnect MUST
NOT tear down a controller. A global wipe intentionally includes the management bond and queues its
link for disconnect.

## Discovery ownership

`ns2_bt_host.c` owns the user-facing pairing window and source-completeness policy.
`btstack_host.c` owns the mechanics:

- LE scan and Classic inquiry run together when discovery is required;
- an explicit pairing window outranks speculative direct reconnect;
- a complete selected controller source may idle discovery;
- a partial keyboard/mouse source retains the documented bounded completion window;
- wake advertising pauses discovery, emits the bounded replay, then restores the prior policy;
- a pending BLE connect keeps the pairing-window close deferred until that attempt resolves.

Discovery is not authorization. Every connection/security path MUST independently enforce trust
admission.

## Connection identity and roles

Controller BLE links occupy `hid_state.connections[]`; the management peripheral link occupies
`config_ble`. Only a successfully initialized controller link can update `JPLC`, the preferred
reconnect record. This structural provenance prevents a management bond from becoming a direct
controller reconnect target.

The underlying LE database itself has no PicoSwitch2 role field. Consequently, inventory can show
address/type but cannot label a disconnected entry as controller versus management. An unknown
entry may cause policy to prefer scanning, but only the `JPLC` identity is eligible for direct
reconnect and SM still controls cryptographic reuse.

## Stable boundaries

This pass does not redesign controller mappings, active-input arbitration, Keyboard + Mouse
composition, wake payload generation, or management authorization. Those remain independent
subsystems with Bluetooth as their transport.
