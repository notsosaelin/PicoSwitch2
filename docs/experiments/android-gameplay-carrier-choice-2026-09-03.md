# Should Android gameplay move from Classic HID to the Path C data plane?

Date: 2026-09-03
Branch: `ns2-testing`
Status: **Recommendation — do not move yet.** No code change made either way.

## Question

The Android companion drives the console over **Bluetooth Classic HID**, as a
cross-transport companion. Windows drives it over **Path C**, a binary GATT
characteristic pair on the management BLE ACL. Should Android move to Path C and
the Classic carrier be deprecated, so that a companion holds **one** Bluetooth
link instead of two?

Raised after a report that the Android on-screen controller "drives nothing",
which turned out to be a different defect entirely (below).

## What prompted it, and what it actually was

The Gamepad page offered two console inputs: `onn 8 Core Tablet` and
`Controller Link`. Selecting the tablet worked; selecting Controller Link drove
nothing.

`Controller Link` was **a Windows Path C session that had already ended** — the
PC's companion was not running. `btstack_host_companion_link_stop()` had exactly
one caller, the explicit `clink stop` command, so a companion that simply went
away left `clink.active` true and left a selectable arbiter source with nothing
behind it.

Evidence, from the adapter over UART with the Windows companion killed:

```
input sources -> sources:[ {id:1, conn:254, transport:8, name:"Controller Link"},
                           {id:2, conn:0,   transport:2, name:"onn 8 Core Tablet"} ]
clink         -> active:true subscribed:true frames:{received:2045, applied:2045}
btstate       -> disc.last_handle:"0x000B"   (== clink.handle)
```

Fixed separately; the Path C session now ends with its management link. **This
was not evidence for or against the carrier question** — nothing about the
Classic path was broken.

## Recommendation

**Keep Classic for Android gameplay for now.** Three reasons, in order of weight.

### 1. The stated benefit is already delivered

The motivation offered was "the touch gamepad should only work when there is an
active management session". That is **already true of the Classic carrier**, by
construction and in two places:

- `ns2_bt_companion_classic_admission_allowed(peer_is_cross_transport_companion,
  companion_session_trusted)` refuses to admit a cross-transport companion over
  Classic unless its management session is trusted;
- `classic_companion_release_on_mgmt_loss()` tears the Classic link down when
  management drops.

Moving to Path C buys nothing on this axis. The requirement is a property of the
companion RELATIONSHIP, not of the carrier, and it is already enforced there.

### 2. The Android BLE link is currently tuned for management, not gameplay

Measured on the tablet's live management link (`adb logcat`, this session):

| Observation | Value |
| --- | --- |
| Negotiated ATT MTU | 517 |
| Management command round trip | **65–194 ms** (n≈250, `request.complete elapsedMs`) |
| Negotiated connection interval | **not observed** — see Unknowns |

A management round trip is one write plus one notification, so it is at least
two connection events plus adapter processing. 65–194 ms for that is a link
whose interval is nowhere near gameplay-grade. Classic HID's interrupt channel,
which the tablet uses today, is not subject to a connection interval at all.

This is the crux, and it is the one thing that has already cost this project
dearly once: the Windows latency episode earlier in this pass was radio
contention, and the fix was understanding the carrier rather than tuning around
it. Moving gameplay onto an unmeasured BLE interval would repeat that mistake in
the opposite direction.

### 3. Risk is asymmetric

Android has **no Path C client at all** — the two characteristic UUIDs were added
to `BleManagementContract` on 2026-09-03 for contract completeness and nothing
references them. Moving would mean new code on the gameplay path, replacing a
hardware-validated Classic bridge that already carries buttons, sticks, motion,
battery, rumble and player LED through the canonical 161-byte descriptor.

The benefit that IS real — one radio link per companion instead of two — accrues
mostly on the adapter's radio, where both links already coexist without observed
contention. The contention that hurt was on the *PC's* adapter, which this change
would not touch.

## What would justify the jump

In this order. The first alone may settle it.

1. **The negotiated connection interval on the Android management link**, under
   `CONNECTION_PRIORITY_HIGH` and under gameplay load. If it is ≤ 15 ms the
   latency case is arguable; if it is 30 ms or more it is not.
2. **A/B end-to-end button-to-console latency** on the same tablet, Classic vs a
   Path C prototype, measured the way the Windows qualification was.
3. **Management coexistence under gameplay load on Android**, the equivalent of
   the Windows 117 ms-vs-241 ms figure. Path C puts gameplay and management on
   one ACL; on Windows that was fine, but Windows negotiated its own parameters.

If all three land well, the jump is attractive: one link, one bond, one security
context, and the arbiter stops having to distinguish two companion carriers that
are both called "Controller Link".

## Remaining unknowns

- **The connection interval was not observed.** `BluetoothGattCallback`'s
  `onConnectionUpdated` is a hidden API the app declares defensively; this OEM
  does not dispatch it, so no `gatt.params` line was logged. The adapter does not
  surface the LE connection parameters over UART either. Getting this number
  needs either an adapter-side diagnostic for the LE update event or an HCI
  capture.
- The 65–194 ms round trips include app-side queueing and adapter processing and
  are therefore an **upper bound** on the transport's contribution, not a
  measurement of it.
- No Android latency figure exists for the Classic carrier either. The comparison
  above is structural, not measured on both sides.

## Confidence

**Recommendation: strong** — reason 1 alone removes the stated motivation, and it
is established from source rather than inferred.

**The latency argument: hypothesis.** It rests on management round trips, which
are an upper bound, and on an unobserved connection interval. It is a reason not
to move *without measuring*, not proof that Path C would be slower.
