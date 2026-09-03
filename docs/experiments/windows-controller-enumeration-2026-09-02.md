# How Windows presents controllers, and which one Controller Link uses

Date: 2026-09-02
Status: Confirmed (measured on the bench named below)

## Question

Which controller on a Windows PC does Controller Link send to the adapter, how
is it chosen, and does the way controllers are enumerated cover the hardware
people actually own?

This was asked because the Windows input backend was known to work with an Xbox
pad and had never been checked against anything else.

## Background

The first Windows implementation polled
`Windows.Gaming.Input.Gamepad.Gamepads.FirstOrDefault()`.

## Environment

| Item | Value |
| --- | --- |
| Machine | YGGSDRASIL, Windows 11 Pro 10.0.26200 |
| Bluetooth | Intel AX210, `USB\VID_8087&PID_0032`, driver 24.40.10.8 |
| Adapter | PicoSwitch2 on Pico 2 W (RP2350), firmware `9cc2511f` |
| Adapter personality | `pro2` (Pro Controller 2) |
| Attached controller | none other than the adapter itself |
| Companion | branch `codex/windows-controller-link` |

The only game controller present during the measurement was the PicoSwitch
adapter's own USB output. That turned out to be the most informative possible
configuration — see "The adapter is a controller" below.

## Method

A probe enumerated all three Windows surfaces in one process and then ran the
shipping catalog and selection rule over the same machine state, so the recorded
decision is the product's, not a reimplementation of it. A second probe measured
the population timing of the WinRT static list.

## Results

### The three surfaces disagree

```
Windows.Gaming.Input.RawGameController   count = 1
    057E:2069  'HID-compliant game controller'
    axes=4 buttons=21 switches=0  gamepadClass=False
    NonRoamableId={wgi/nrid/P<VB-HG050D_b-<RbG^-R9<1UN8]JM\Z8-Vk}

Windows.Gaming.Input.Gamepad             count = 0

HID DeviceInformation (usage 0x01/0x05)  count = 1
    'Switch 2 Pro Controller'
    instance  = HID\VID_057E&PID_2069&MI_00\8&2ca74762&0&0000
    container = db21d59f-e4de-516e-8e99-d3efebdbfb8a
```

| Surface | Provides | Does not provide |
| --- | --- | --- |
| `RawGameController` | every controller; VID/PID; `NonRoamableId`; axis/button/switch counts | a real product name; named controls |
| `Gamepad` | named buttons and axes; rumble | anything outside the XInput class |
| HID `DeviceInformation` | real product name; `ContainerId` | any gameplay input |

Three consequences, each confirmed above:

1. **`Gamepad` is a strict subset.** The same physical device that
   `RawGameController` enumerates gives `Gamepad` count 0. A backend that polls
   only `Gamepad` is silently blind to DualSense, DualShock 4, Switch Pro,
   Switch 2 Pro and most non-Xbox hardware — it reports no error and shows no
   empty state, it simply never produces a frame.
2. **`RawGameController.DisplayName` is not a product name.** It returned the
   literal string `HID-compliant game controller`. A picker built from it alone
   shows identical rows for different devices.
3. **HID carries identity but no input.** It is the only surface that returned
   `Switch 2 Pro Controller`, and the only one carrying `ContainerId`.

### Built-in vs external is answerable generically

`System.Devices.ContainerId` distinguishes them: a device that is physically part
of the machine sits in the machine's own container
(`{00000000-0000-0000-FFFF-FFFFFFFFFFFF}`), and anything plugged in or paired
gets a container of its own. The controller measured here reported
`db21d59f-e4de-516e-8e99-d3efebdbfb8a`, correctly classified External.

This means a handheld's built-in controls (ROG Ally, Legion Go, MSI Claw) are
identifiable **without a VID/PID database**. No such database should be
introduced; it would need maintaining for every handheld ever released, and
would still be wrong for the next one.

### The WinRT list is empty on a cold read

```
t=  0ms  cold synchronous read        -> 0
t= 87ms  after 50ms of awaits         -> 1
t= 53ms  after awaiting HID           -> 1
```

`RawGameController.RawGameControllers` is populated by machinery that needs a
turn first, so the **first** read in a fresh process reports nothing. The catalog
was safe only incidentally, because it awaits HID enumeration before reading it.
The app's first refresh is issued during service construction — exactly the cold
case.

This is the same failure mode as the `Gamepad`-only bug (enumerate nothing, look
healthy), reachable by a refactor as innocuous as "skip the HID work when there
is nothing to name". The catalog now waits explicitly, bounded by evidence: HID
has already reported how many controller-class devices exist, and disagreement
between the two surfaces is the cold-start signature. When HID also finds
nothing there is nothing to wait for and it returns immediately.

### The adapter is a controller

The single enumerated device was `057E:2069` — **the PicoSwitch adapter's own USB
output**, indistinguishable by identity from a genuine Pro Controller 2, because
that is precisely what it is emulating.

Auto-selecting it would feed the adapter's own output back in as its input. The
failure presents as *drifting sticks*, not as an error.

## Selection rule

The ambiguity rule is shared with Android and lives in `ControllerCandidates`:
exactly one usable device is chosen without asking; two or more means the user
decides. Windows adds only what Windows can answer:

- A source that cannot be read is not chosen automatically.
- A source matching the connected adapter's emulated identity is not chosen
  automatically.
- On a handheld, the **built-in** controls win over an external pad — the
  machine's own sticks are the controller in the user's hands, and an external
  pad is a deliberate act they can express by choosing it. Two built-in devices
  (a Legion Go with both halves) is ambiguous again and defers.
- An explicit choice outranks all of the above, including the adapter guard:
  someone may be testing that loop deliberately.

Recorded outcome for this machine, from the shipping code:

```
auto-selectable: 0
resolved (no remembered choice): NONE
reason: The only controller found looks like this adapter's own output.
        Connect a different controller, or choose it anyway.
explicit choice of the adapter still honoured: True
```

### The adapter guard is keyed on wire names

`PersonalityProducts` is keyed on the management wire names — `pro2`, `gc`,
`jcl`, `jcr` — not on invented aliases. An earlier revision guessed `joycon2l`
and `pro`; those never match, so the guard would have looked present in the code
and done nothing, and the loop it exists to prevent would have happened anyway.
A test walks every `Personality` enum value and fails if one has no product id.

The guard is armed only while an adapter is connected, because it matches against
the personality that adapter reports. With no management session the adapter's
USB echo is genuinely indistinguishable from a real Pro Controller 2 — and
Controller Link cannot start without a trusted management session, so the guard
is armed exactly when it can matter.

## Interpretation

Discovery coverage and semantic input coverage are different properties, and
conflating them is what produced the original defect.

| Device | Discovered | Identified | Readable |
| --- | --- | --- | --- |
| Xbox / XInput-class | yes | yes | yes |
| Switch 2 Pro | yes | yes | **no** |
| DualSense, DualShock 4, Switch Pro | yes | yes | **no** (expected; not yet measured) |

`RawGameController` reports axis and button **counts**, not meanings. Reading a
non-gamepad-class device would require mapping unnamed control arrays per device,
which cannot be done by index because values happening to move proves nothing
about which control moved. That work is not attempted here and is not faked: such
a device is enumerated, named, offered, and labelled "cannot be read", and if
chosen anyway the UI says outright that Controller Link will not receive input
from it.

## Confidence

**Confirmed** for: the three surfaces and their disagreement, the cold-read
timing, `ContainerId` classification of an external device, the adapter's own
enumeration identity, and the resulting selection outcome. All were measured on
the bench above and are reproducible with the probes described.

**Not measured**: `ContainerId` for a genuine handheld's built-in controls (no
handheld was available); DualSense and DualShock 4 `gamepadClass` (inferred from
the same subset rule that was confirmed for the Switch 2 Pro).

## Remaining unknowns

- Whether any non-XInput controller projects onto `Gamepad` in current Windows
  builds. Everything measured so far says no.
- What a real handheld reports for `ContainerId` on its built-in controls. The
  rule is the documented Windows contract, but it has not been observed here.

## Suggested follow-up

Only if broader physical-controller support becomes a release requirement:
measure `RawGameController` control ordering for one specific device against a
known input sequence, and derive a mapping from that evidence. Do not assign
meanings by index.
