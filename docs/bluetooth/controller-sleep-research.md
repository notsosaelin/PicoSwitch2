# Controller sleep alongside console sleep

**Status:** 🔵 Research concluded 2026-07-17. No safe generic runtime change was found. The current
controller-managed idle timeout and the hardware-confirmed console-wake path remain unchanged.

## Goal and non-negotiable constraints

The desired behavior is for a BLE controller to power down when the Switch 2 sleeps while:

1. preserving its BLE bond,
2. accepting an ordinary reconnect as soon as the user wakes the controller, and
3. preserving the confirmed first-real-input console wake path.

Bond deletion, pairing lockout, admission filtering, and a reconnect blackout are not acceptable
ways to make the controller appear asleep.

## Current lifecycle

Core 0 publishes TinyUSB mounted/suspended state to `ns2_wake.c`. After 750 ms of stable USB
inactivity, core 1 arms automatic console wake. A later neutral-to-pressed controller report starts
the confirmed Nintendo wake advertisement. The Bluetooth host intentionally continues running
while USB is suspended.

BLE controllers are peripherals and PicoSwitch2 is the central. A controller that wants to
reconnect advertises; the central must still choose to create the connection. PicoSwitch2 currently
does that in two places:

- the advertising-report handler connects to a supported controller while scanning; and
- while scanning, the bonded-device fallback makes a direct attempt every 20 seconds.

The disconnect handler already distinguishes an intentional peer shutdown from a link failure.
Peer shutdown resumes scanning without deleting the bond or entering the five-attempt direct
reconnect cascade.

## Findings

### 1. An ACL disconnect is not a controller sleep command

`gap_disconnect()` only terminates the BLE link. It does not tell a controller to power off. The
previous generic post-sleep disconnect experiment demonstrated the practical risk: the Xbox
relationship became stranded and had to be restored, so the experiment was reverted.

For Xbox in particular, MissionControl's current documentation states that the controller cannot be
switched off in software and attempts to reconnect after a host disconnect. Its source was checked
at commit `d3941d433f15827de8aea116d61ea17bb61d0bcc`.

The current xpadneo Xbox BLE driver was checked at commit
`3acca9f5e211edb601000bb64767b78b2468f787`. Its controller output path implements rumble but no
shutdown or sleep command. Absence from a driver is not protocol proof by itself, but it agrees with
the observed Xbox behavior and MissionControl's controller-specific finding.

References:

- [MissionControl known limitation](https://github.com/ndeadly/MissionControl#known-issues-and-limitations)
- [xpadneo Xbox BLE driver](https://github.com/atar-axis/xpadneo)

### 2. Preventing the reconnect also prevents immediate wake

There is no BLE equivalent of an independently accepted incoming ACL connection here. The
controller advertises and the Pico, as central, initiates the connection. Therefore a policy that
keeps a just-disconnected controller offline must suppress either the advertising-triggered
connection, the periodic direct connection, or both.

That suppression is a reconnect blackout. During it, a HOME press can make the controller
advertise, but PicoSwitch2 cannot receive the pressed HID report until it reconnects. The existing
automatic console-wake policy consequently cannot see the input. A fixed cooldown merely chooses
how long wake is broken; it does not remove the conflict.

The comparison implementation in `Dycool/NS-PC-Control` explicitly disconnects controllers and
pauses its own proactive reconnect loop for the entire confirmed-sleep interval (checked at commit
`8232d88d3c364148b4f49b22b6045a2c001cc2f3`). That is a valid product-policy choice for its
architecture, but it does not satisfy this project's no-reconnect-blackout requirement and was not
ported.

### 3. BLE low-power connection parameters are not power-off

The vendored BTstack provides `gap_update_connection_parameters()` and
`gap_request_connection_parameter_update()`. Longer intervals and Peripheral latency can reduce
radio duty while retaining an ACL. They cannot transition a controller to its powered-off state.

The Bluetooth Core specification defines Peripheral latency as connection events during which the
Peripheral is not required to listen. The connection, supervision timeout, and event counter remain
active. This is useful for link power optimization, not controller sleep.

Changing these parameters also carries controller-specific input risk. Xbox BLE is known to be
sensitive to unsuitable connection intervals, and no per-family asleep/awake values have been
captured for this hardware. Applying speculative values would add wake latency or link instability
without achieving the requested power-off behavior.

References:

- [Bluetooth Core Link Layer: connection events and Peripheral latency](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/low-energy-controller/link-layer-specification.html)
- [BTstack GAP APIs](https://bluekitchen-gmbh.com/btstack/appendix/apis.html)

### 4. No standard HID controller-sleep command exists in the current path

The registered BLE drivers expose input parsing and family-specific output such as rumble. None
currently exposes a verified sleep capability. A future controller-specific command is possible
only when supported by a real capture or authoritative protocol evidence; it cannot be inferred
from the presence of a generic HID output channel.

## Decision

Do not change runtime behavior for this item:

- keep bonds and pairing admission untouched;
- keep connected BLE controllers connected while the console sleeps;
- allow each controller's own idle timer to power it down;
- continue scanning immediately after a controller intentionally powers itself off; and
- preserve the confirmed real-input wake state machine unchanged.

This is preferable to a change that appears to save power but silently makes HOME unable to
reconnect or wake for a controller-dependent interval.

## Evidence gates for future work

Reopen implementation only if one of these produces new evidence:

1. **Verified controller-side sleep command.** Capture a real host/controller exchange that powers
   down a specific BLE family without clearing its bond. Implement it as an explicit per-family
   capability, never as a generic HID command.
2. **Distinguishable advertisements.** Capture and compare the complete advertisement sequence
   after a host-requested disconnect versus a user pressing HOME on a powered-down controller. If
   a stable controller-specific distinction exists, PicoSwitch2 may be able to ignore only the
   former.
3. **Explicit policy tradeoff.** If immediate reconnect/wake is no longer a requirement, add an
   opt-in timed reconnect blackout with a documented wake-delay cost. It must not be the default.
