# Windows Controller Link (Path C) — hardware qualification record

Date: 2026-09-03
Firmware: `9cc2511f` (Pico 2 W / RP2350), flashed over the UART `bootsel` path
Companion: branch `codex/windows-controller-link`
Radio: Intel AX210, Windows-owned, one normal Bluetooth adapter
Adapter USB: connected to a Switch 2 console

All figures below were read from the adapter over UART or from the companion's
own counters. Nothing here is inferred from the UI.

## Transport

| Property | Result |
| --- | --- |
| Negotiated ATT MTU | 527 (minimum required 33) |
| Frame size | 30 bytes, data-plane version 1 |
| `received` vs `applied` | equal in every run |
| `stale` / `short` / `version` / `opcode` | 0 |
| arbiter-rejected | 0 |
| `MaximumInFlight` | 1 |
| Second ACL / HOGP advertising / remote pairing | none — `cble.advertising=false`, `controller_connected=false` |

Sustained rates measured at the adapter: 250 Hz applied over 45 s with 100 %
applied at the fixed cadence, and ~10 Hz at idle once the publisher moved to
send-on-change with a 100 ms keepalive.

## Controls (§13)

All 13 labelled digital controls on the on-screen layout produce **distinct**
bits at the adapter, none dead, none colliding, and the layout returns to exact
neutral afterwards:

```
ZL 0x00000040   L 0x00000010   R 0x00000020   ZR 0x00000080
X  0x00000004   Y 0x00000008   A 0x00000001   B  0x00000002
-  0x00000100   + 0x00000200   L3 0x00000400  R3 0x00000800
C  0x00040000
after everything: raw=0x00000000 sticks=[0,248,127,0,248,127]
```

Left stick travels correctly in all four directions (right/left/down/up produce
distinct, opposite-signed deflections) and re-centres.

**One defect found and NOT fixed**: the four face buttons transmit the opposite
letter to the one drawn when the face-layout setting is Nintendo. See the
addendum in `windows-controller-link-analog-backlog-2026-09-03.md`. The
physical-controller path is unaffected.

## Console buttons

Home / Capture / C are enabled only while Controller Link streams and inject as
virtual buttons through the same input session. A Capture tap was observed at the
adapter as `raw=0x00010000 mapped=[0,16,0]`.

## Management while streaming (§8)

A real management read (bond slots) over the same BLE session, with gameplay
streaming throughout:

```
streaming  n=3  median= 117 ms   [57, 117, 56]
idle       n=3  median= 241 ms   [240, 239, 241]
gameplay during: applied +399   stale 0   neutralizations 0
```

Management is **not** degraded by streaming — it is faster, because the link is
already active on a short connection interval rather than relaxed. Gameplay did
not stall, no frame went stale, and no gameplay frame entered the JSON path.

## Stale-input watchdog (§11A)

Non-neutral input held, then gameplay publication stopped by suspending the
companion process while ordinary management BLE stayed up (killing the app would
have dropped the ACL and tested something else):

```
held           : raw=0x00000002 mapped=[8,0,0]
after 3 s      : raw=0x00000000 mapped=[0,0,0]
neutralizations: 0 -> 1        (exactly once)
neutralized    : True          active: True
after resume   : neutralized=False, frames applied again
```

Neutral is published once, the runtime stays capable, management stays
connected, and valid input resumes **without any reconnect or re-pair**.

Held input alone does not trip it: a 6 s hold (20× the 300 ms timeout) kept
`mapped[0]=8` throughout with 0 neutralizations, because the 100 ms keepalive
refreshes the adapter even when the state has not changed.

## Stop and restart (§11B/C)

```
after stop : clink active=False   adapter input raw=0x00000000 sticks=[0,248,127,0,248,127]
             management still connected: True
restart    : generation=4  received=51  applied=51  stale=0   no re-pair required
```

## App loss (§10A)

Companion killed while the link was up:

```
adapter input : raw=0x00000000 sticks=[0,248,127,0,248,127]
clink         : active=True neutralized=True
controls neutral: True
```

The watchdog neutralized, nothing stuck, and the app reconnected and restarted
Controller Link normally afterwards.

A related firmware defect was found and fixed during this work: a companion that
dies without sending `clink stop` used to leave the adapter holding its sequence
high-water mark, so the replacement's frames were all rejected as stale while
both ends reported a healthy stream. `clink start` now re-arms the ordering.

## Not covered

- **Rumble output latency**: the reverse path is proven working
  (`outputs sent=36 failed=0`, Windows decoded `output=4/4 malformed=0`, and the
  reporter confirms rumble works on the console), but amplitude cannot be
  commanded from this bench — only the console requests it, so start/stop/change
  timings were not measured.
- **Android Classic comparative baseline**: no Android device was attached
  (`adb devices` empty), so the performance-parity comparison in the closeout
  plan could not be run. The Classic sender's architecture was read from source
  and is recorded in the analog-backlog writeup.
