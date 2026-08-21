# Controller Link input published to a slot nothing reads

**Date:** 2026-08-21
**Branch:** `ns2-testing`
**Areas:** input seam, source arbitration, companion Adapter page

Found immediately after the Bluetooth reliability pass was accepted, on the same
preserved hardware session. Two separate-looking symptoms turned out to be one silent
misroute in the input seam.

---

## Question

With Controller Link selected as the active console input source:

1. Steam showed no button or axis activity, and
2. the companion's Adapter page showed "None paired" instead of an active-input identity,

while switching *selection* between the DualSense Edge and Controller Link kept working, and
the management session, audio, and both Bluetooth links stayed up.

## Background

This project is a single-controller milestone. Every console-facing reader is a hardcoded
`get_global_gamepad_input(0, …)`, and `ns2_input_arbiter` guarantees that at most one source is
ever accepted for publication. The Adapter page's Controller row reads the `device` management
command, which is `get_global_device(0, …)` — the **console slot's** identity, not physical
attachment. `report_neutralize_slot(0)` clears that identity (`s_dev_name`, `s_dev_vid`,
`s_dev_pid`) on every ownership handover, and it is republished per accepted report by
`set_global_device()`.

## Method

Read-only capture on the preserved session first (adapter UART diagnostics, `adb logcat`, the
app's mirrored diagnostic log, `dumpsys input`), then one controlled A/B using the product's own
source selector, then a real input injection on the Android handheld's local gamepad.

## Environment

| | |
|---|---|
| Adapter | Pico 2 W, `pro2`, build `8616558f+dirty`, ~79 min uptime |
| Sources | DualSense Edge on Classic **conn 0**; Android Controller Link on Classic **conn 1** |
| Management | one GATT generation, `sinceReadyMs` ≈ 8.1 M, zero errors |
| Phone | Android 13, app `2.0.0-debug`, local gamepad "Odin Controller" = `/dev/input/event9` |

## Results

Controlled A/B, same instrument, same session:

```
DualSense (id 2) active:  fresh=false  inputAgeMs=0        slot0 vid=0x054C
Controller Link (id 3):   fresh=false  inputAgeMs 2171 → 4201 → 6501 → 8769 → 11030 → 13479
                                                            slot0 vid=0x0000
```

Injected `ABS_X 0x61a8` on `/dev/input/event9`, confirmed delivered by `getevent -l`:
slot 0 unchanged, `pipe.inputAgeMs` 148882 → 150647.

`pipe.reportCount` kept climbing at ~1275/s throughout: USB output was healthy and carrying
**neutral** state.

### Boundary determination

| Boundary | Verdict | Evidence |
|---|---|---|
| A — Android input events stop | No | `getevent -l` shows the injected event delivered |
| B — transport stops forwarding | No | `reportsSent` increments only on `sendReport()==true` (`BridgeSession.recordReport`) and climbed ~91/s |
| C — arbiter does not select/use them | No | `awaiting_fresh` cleared to `false` within ~2 s of *every* explicit selection of source 3; only an **accepted source-3 report** can clear that latch |
| **D — normalized console state does not update** | **Yes** | slot-0 vid/pid `0x0000`, sticks neutral, `inputAgeMs` climbing monotonically |
| E / F | not reached | — |

## Interpretation

`router_submit_input()` published through

```c
static inline uint8_t ns2_slot(uint8_t dev_addr) {
    return (dev_addr < NS2_SLOTS) ? dev_addr : 0;   // NS2_SLOTS == 4
}
```

so the output slot was the **BTstack connection index**. DualSense on conn 0 published to slot 0
and drove the console; Controller Link on conn 1 published to slot 1, which no reader ever reads.
Slot 0 kept the handover's neutral state, which is why the console saw a neutral controller *and*
the Adapter page truthfully reported an empty identity. The UI was never wrong — it was reporting
an empty slot accurately.

This is the input-direction twin of the feedback-direction bug fixed on 2026-07-12 in
`find_player_index()`, whose own comment already stated the rule being violated: *"This project has
one output identity … always resolving to slot 0 is not a workaround, it's what this stand-in
already claimed to do and didn't."*

It stayed latent for two reasons: BLE connection indices are offset by `BLE_CONN_INDEX_OFFSET` and
are always ≥ `NS2_SLOTS`, so every BLE source fell through to 0; and two **Classic** sources rarely
coexisted. The Bluetooth reliability pass made physical-controller + Controller Link coexistence
routine, which is what exposed it. Connection order decided which source worked.

A second, smaller defect was uncovered behind the first: the Controller Link has **no Bluetooth
name**. It reaches the adapter as an *incoming* Classic HID Device connection, so no inquiry record
ever supplied one and `btdev`/`input sources` both reported `""`. With the slot fix alone the
Adapter page would have shown an attached controller with a blank name.

## Conclusion

**Confirmed.** The console has exactly one output slot; publishing by connection index silently
discarded a correctly-arbitrated source. Corrected to the `NS2_CONSOLE_SLOT` constant, and
`ns2_input_source_display_name()` supplies "Controller Link" for a source whose class the firmware
already determined from the bridge's own declared HID descriptor — never from a name, VID/PID, or
anything reconstructed from Android UI state. A real Bluetooth name always wins; a nameless
*direct* controller stays nameless rather than being mislabelled.

Hardware-confirmed by the maintainer after flashing the corrected firmware: Controller Link input
reaches the console, and the Adapter page shows a truthful active-input identity.

## Guards

`tools/test_ns2_console_slot_wiring.py` pins the one-output-slot invariant in both directions and
is mutation-checked — reintroducing `ns2_slot(e->dev_addr)`, publishing via `e->dev_addr`, or making
`find_player_index()` return a connection index each fail it. The naming rule is covered at its pure
seam in `tools/test_ns2_input_arbiter.c`.

## Remaining unknowns

- Controller Link on a connection index ≥ 1 is now correct, but three or more simultaneous Classic
  sources have never been exercised.
- The bridge still has no true Bluetooth name; "Controller Link" is a class label, not the phone's
  own name. Propagating the Android device name would need a bridge-supplied field.
- The management source list truncates names to 16 characters, so long controller names still
  display cut ("DualSense Edge W").
