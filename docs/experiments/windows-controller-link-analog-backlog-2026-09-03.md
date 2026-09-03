# Analog "backlog": stick movement replaying after the player stops

Date: 2026-09-03
Status: Root cause **Strong Evidence**; the transport's innocence is **Confirmed**

## Question

Playing through Windows Controller Link, fast stick movement kept playing out
after the player had physically stopped — the console walked through the old
trajectory before catching up, and button presses landed late while it did.
Buttons alone were fine. Where is the queue?

## Why this was hard to find

Every counter on the Controller Link path stayed perfect the entire time:

```
generated == published == written      coalesced = 0
firmware: received == applied          stale = 0   malformed = 0
maxInFlight = 1                        writeAvgMs = 0.72
```

The transport was behaving exactly as designed while the experience was
unusable. No amount of looking at our own diagnostics could find it, which is
why three separate attempted fixes missed.

## Disproven along the way

Recorded because each was plausible and each cost time.

| Theory | Killed by |
| --- | --- |
| Unbounded queue in our writer | `coalesced = 0`, `maxInFlight = 1` — the mailbox never even held a pending frame |
| FIFO in the adapter | `set_global_gamepad_input()` is a slot overwrite under a critical section; there is no queue downstream of `router_submit_input` |
| Frames queued in the Windows driver below WinRT | Measured: after release, adapter intake fell to the ~10 Hz keepalive **immediately**. Nothing drained |
| Publication rate too high | Raising and lowering the ceiling changed nothing about the replay |

The driver-queue theory was the most attractive, because
`WriteValueWithResultAsync` genuinely does complete when the driver ACCEPTS the
buffer rather than when the radio transmits — so a queue *could* have hidden
there. It did not. The measurement that settled it:

```
while driving        :  59.7 frames/s reaching the adapter
first  474 ms after  :  23.2 frames/s
settled (2 s window) :   8.5 frames/s   <- keepalive
```

If frames had been queued below us they would have kept arriving after the
source went neutral. They did not.

## Root cause

The controller was connected to the PC over **Bluetooth**, so the input made two
radio hops on the same adapter:

```
controller --BT--> PC --BT--> PicoSwitch --USB--> console
             \___________________/
              same AX210 radio
```

Under sustained analog motion those links compete. The controller's own link
loses airtime, Windows delivers its reports late and in bursts,
`Gamepad.GetCurrentReading()` returns stale positions, and the app forwards them
faithfully. The replay is real input, accurately relayed — just old.

**Confirmed by the reporter:** the symptom is present with the controller on
Bluetooth and absent with the same controller on USB.

## What made it worse, and what fixed it

The app was a heavy neighbour on that radio. An analog stick is never still: its
low bits flicker from sensor noise even untouched, so byte-exact change detection
reported movement on nearly every sample and the publisher sent near-continuously
even while the stick was held steady.

Two changes reduced that traffic:

- an axis must now move at least **2/255** before it is worth a frame
  (`ControllerLinkSendPolicy.AnalogEpsilon`), compared against the last SENT
  state so slow drift still trips;
- digital and analog changes have separate ceilings, so a stick cannot set the
  send rate for the whole link.

After these, the reporter no longer observes the backlog **even over Bluetooth**.

Confidence: **Strong Evidence**, not Confirmed. Airtime before and after was not
measured side by side over Bluetooth, so the causal chain
"less traffic -> controller link not starved" is inferred from the two
independent cures (USB, and reduced traffic) rather than directly observed.

## Product response

The app now detects how the controller reaches the PC, from its device instance
id — Bluetooth HID over GATT (`{00001812-...}`), Classic HID (`{00001124-...}`),
or a bare `VID_xxxx&PID_xxxx` for wired — and says so on the Gamepad page when
the selected controller is on Bluetooth. Informational, not a warning: it works,
but the player cannot otherwise deduce that their controller and the adapter are
sharing one radio, and the symptom looks exactly like a bug in this app.

`Unknown` is deliberately treated as wired. Warning about a radio hop that may
not exist is worse than silence.

## Remaining risk

The transport requests `ThroughputOptimized` connection parameters, which makes
our link more aggressive on that shared radio. That helped our own hop's latency
measurably, but it takes airtime from the controller's link, so it works against
the fix above. It has not been shown to reintroduce the symptom, and it should be
the first thing re-examined if the backlog is ever reported again.

## Reproduction

- `tmp/drain.ps1` — drives the on-screen stick, releases, and measures whether the
  adapter keeps being fed (the drain test that cleared the transport).
- `tmp/watch.ps1` — records both sides for a minute of ordinary play and flags any
  interval where the adapter outran what the app produced.

Both read the app's per-second `live:` metrics line, which is a permanent
diagnostic: "how far behind is the link right now" is precisely the question the
Stop-only summary could not answer.

---

# Addendum: on-screen controller face labels disagree with the wire

Date: 2026-09-03
Status: **Confirmed defect, unfixed.** Touch Gamepad only; the physical-controller
path is correct.

## Observation

With the face-layout setting on **Nintendo**, every face button on the on-screen
controller transmits the OPPOSITE letter to the one drawn on it. On **Xbox** they
agree. Measured against `include/switch_pro.h`
(`Y=0x01 X=0x02 B=0x04 A=0x08`), reading the adapter's decoded `mapped[0]`:

| drawn | screen slot | Xbox setting | Nintendo setting |
| --- | --- | --- | --- |
| X | north | `0x02` X — agrees | `0x01` Y — **inverted** |
| Y | west  | `0x01` Y — agrees | `0x02` X — **inverted** |
| A | east  | `0x08` A — agrees | `0x04` B — **inverted** |
| B | south | `0x04` B — agrees | `0x08` A — **inverted** |

Reproduced twice, on two different window positions, with the drawn geometry
confirmed each time (X north, Y west, A east, B south — the Nintendo diamond).

## Why this is a design problem regardless of the mechanism

The face-layout setting describes the **printed legend on the player's physical
controller**. The on-screen controller has no printed legend to describe — its
labels are drawn by this app, and `TouchControlNaming.FaceLayout` fixes them to
Nintendo on the stated grounds that "every controller this surface can emulate is
a Nintendo one".

So one setting currently drives two things it should not drive together:

- `MapPhysicalFaceKey` — correct, and what the setting is for;
- `MapTouchFacePosition` in `ControllerInputState.Publish()` — applied to a
  surface whose labels do not vary.

A preference about someone's Xbox pad should not be able to change what the
on-screen A button sends.

## Not fixed here, deliberately

Two candidate one-line fixes exist — have the renderer use the resolved layout,
or have the touch branch use the same constant the renderer draws with — and
both are provably wrong against the measurements above, which means the model
they rest on is incomplete. `PositionalButtons` is documented as holding raw
POSITIONS for the resolver to interpret once, but the numbers only fit if the
drawn labels are already final letters.

Changing a golden-tested subsystem on a model that does not predict the observed
behaviour is how the earlier attempts in this investigation went wrong. The next
person should start by dumping, for one physical slot: the stored
`TouchControlAction.Face.Position`, the string the renderer actually draws (and
whether it comes from `FaceLabel` or from `spec.Label`), and the
`ControllerButton` that reaches `Publish`. That single trace settles it.

## Impact

Bounded. The physical-controller path uses `MapPhysicalFaceKey` and is correct
under both settings — a player using a real controller is unaffected. Only the
on-screen controller mislabels, and only while the setting is Nintendo.
