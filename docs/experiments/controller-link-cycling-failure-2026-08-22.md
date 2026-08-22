# Controller Link cycling failure — 2026-08-22

Status: **root cause Confirmed, and it is not in PicoSwitch2.** The failure is a Bluetooth
sleep/wake fault in the *tablet's* Qualcomm controller. Adapter-side changes in this pass add
margin against it; they do not remove it.

## Question

The Android Controller Link normally connects, but repeatedly running the full lifecycle
(management connect → Touch Gamepad → exit → stop Controller Link → end management) eventually
produces a failure in which Controller Link will not connect, the management session drops at the
same moment, the app shows `0x85`, and everything recovers on its own without rebooting the
adapter. What fails?

## Environment

- Adapter: Pico 2 W, firmware profile controller `2.1.4` / bluetooth `12.0.0` / dsp `0.2.3`,
  personality `pro2`. The build under test is the **currently flashed release build**; none of this
  pass's uncommitted changes were on it.
- Tablet: onn 8 Core (`onn8Core`), Qualcomm Bluetooth, app `dev.picoswitch.companion.debug` 2.0.0-debug.
- Adapter Classic BD_ADDR ends `77:78` (`name:PicoSwitch2` in Fluoride logs).
- Access: UART on COM11, ADB over TCP. No physical interaction, no reflash.

## Method

Two independent bodies of evidence:

1. **Maintainer's manual run** (~10 min of deliberate cycling) plus the retained `logcat` buffer
   covering it.
2. **Automated reproduction**: 10 scripted lifecycle cycles driven entirely over ADB
   (`am force-stop` to end management → relaunch → `Reconnect` → `Touch Gamepad` → poll UART
   `btlife` until `classic_ready > 0` → `BACK` → `Stop playing`), with human-scale pacing and
   per-cycle UART `btlife` / `bthealth` snapshots. `logcat` was cleared first, so the capture
   covers exactly these cycles.

## Results

### Automated reproduction

| cycle | link established | `reject_window` | `disc.hci` | `disc.last_reason` | `control_tick_max_gap_ms` |
|-------|------------------|-----------------|-----------|--------------------|---------------------------|
| 1 | 6.1 s | 1 | 1 | `0x13` | 851 |
| 2 | 6.1 s | 1 | 2 | `0x13` | 851 |
| **3** | **failed (>25 s)** | 1 | 4 | **`0x08`** | 851 |
| **4** | **failed (>25 s)** | 1 | 4 | **`0x08`** | 851 |
| 5 | 6.3 s | 1 | 4 | `0x08` | 851 |
| 6–10 | 6.1–6.3 s | 1 | 5–9 | `0x13` | 851 |

The failure reproduced on cycle 3 of 10, matching the maintainer's "4–5 cycles".

### What the reproduction rules out

- **Not admission (Mode 2).** `admission.reject_window` stayed at **1** across all ten cycles,
  including both failures. The adapter never rejected the tablet.
- **Not core-1 starvation.** `core1.control_tick_max_gap_ms` stayed at **851 ms** — its
  pre-existing high-water mark — and never moved. Nothing approached a supervision timeout.
- **Not the bounded HCI/CYW43 recovery.** `hci.recovery.attempts = 0`, `reboot.requests = 0`
  for the whole session. That path has still never fired.
- **HCI itself was healthy**: `probes {sent:443, ok:443, failed:0, timeouts:0}`.

Successful cycles ended `0x13` (`REMOTE_USER_TERMINATED_CONNECTION`, the normal app-driven
teardown). Failing cycles ended `0x08` (`CONNECTION_TIMEOUT`).

### Cause, from the tablet's own logs

**Reproduced failure (14:18:00)** — Android's Bluetooth stack process crashed:

```
14:17:52.213 vendor.qti.bluetooth@1.0-uart_transport: SocRxDWakeup: Flow off->Change UART
                                                      baudrate to 38.4kbs->send 0x00->...
14:17:52.235 vendor.qti.bluetooth@1.0-ibs_handler:   WakeRetransTimeout: Writing HCI_IBS_WAKE_IND
14:17:52.245 vendor.qti.bluetooth@1.0-ibs_handler:   WakeRetransTimeout: Writing HCI_IBS_WAKE_IND
14:18:00.205 vendor.qti.bluetooth@1.0-uart_controller: UartController::Cleanup,
                                                       soc_need_reload_patch=1
14:18:00.208 ActivityManager: ... BluetoothManagerService.resetAdapter(...:1954)
14:18:00.210 ActivityManager: Process com.android.bluetooth (pid 2815) has died: psvc PER
14:18:00.210 BluetoothSystemServer: Package [BluetoothSystemServer] requested to [Disable].
                                    Reason is CRASH
```

The Bluetooth PID changed 2815 → 28621. The app then logged
`transport/HID profile: service disconnected` followed by five
`management/error: background: android.os.DeadObjectException` over the next 25 s. Cycle 4 failed
immediately afterwards (`HID connection rejected: elapsedMs=5222`) while the stack was still
re-initialising — a consequence of the crash, not an independent fault.

**Maintainer's production failure (13:00:01)** — same subsystem, different outcome. The SoC slept
through its own live links:

```
12:59:58.791 ibs_handler: DeviceSleep: TX Awake, Sending SLEEP_IND
12:59:58.791 ibs_handler: SerialClockVote: vote for UART CLK OFF
             ... 2.35 s with no host<->SoC traffic, two ACLs live ...
13:00:01.141 ibs_handler: ProcessIbsCmd: Received IBS_WAKE_IND: 0xFD
13:00:01.145 bt_shim_hci: disconnection ... handle: 0x07, reason: 0x08
13:00:01.148 bt_shim_hci: disconnection ... handle: 0x06, reason: 0x08
```

Handle `0x06` was the LE management link and `0x07` the Classic HID link. **Both dropped together
because there is one SoC and it was asleep.** A separate wake-retransmit storm at 12:07:25 (dozens
of `WakeRetransTimeout` at 10 ms intervals) shows the same wake path failing repeatedly on this
device.

### `0x85`

`0x85` is **our own label**, not a raw platform code. The app logs
`management/error: input: Bluetooth write failed (GATT status=0x85 ANDROID_GATT_ERROR)` for Android
GATT `status=133`. Established from our own log line paired with `management/command.write:
... status=133` on the same write — not from the `0x85 == 133` coincidence.

It is a **downstream consequence**: the GATT write was already in flight when the peer's stack
went away. Note that Fluoride *also* prints an unrelated `0x85` constantly
(`bta_dm_pm.cc: Current power mode:UNKNOWN[0x85]`, a BTM power-mode state during routine sniff
negotiation); that one is noise and must not be confused with ours.

## Interpretation

One radio on the tablet serves both transports, so a fault in its sleep/wake path takes down the
LE management session and the Classic Controller Link simultaneously. Two observed outcomes:

- SoC sleeps through live links → both drop on supervision timeout (`0x08`);
- host cannot wake the SoC → vendor SSR, `com.android.bluetooth` restarts (~30–60 s).

Both are consistent with every symptom the maintainer reported: majority of cycles fine,
simultaneous management + Controller Link loss, `0x85`, self-recovery, and **no adapter reboot ever
required** — the adapter was never in a bad state.

## Adapter-side response

We cannot fix the tablet's firmware. We can stop a *transient* peer stall from becoming a dropped
session. The adapter is the LE peripheral on the management link and previously requested nothing,
so Android's chosen supervision timeout decided how long a stall had to be to kill it.
`ns2_bt_mgmt_link_params()` now requests 15–50 ms interval, latency 0, **6 s supervision timeout**.
The 13:00:01 stall was ~2.35 s and would have been ridden through.

This is not invented: JoypadOS hit the same class on a single-radio dongle running LE and Classic
together and fixed it the same way — see the lineage note below.

## Confidence

- Android Bluetooth process crash and vendor SSR: **Confirmed** (ActivityManager process death,
  PID change, vendor HAL trace).
- Simultaneous dual-transport loss caused by the peer's SoC sleeping through live links:
  **Confirmed** for the 13:00:01 event (HAL sleep/wake trace bracketing both disconnects).
- `0x85` as a downstream consequence rather than a cause: **Confirmed**.
- That a 6 s supervision timeout prevents this class of drop: **Strong** — the arithmetic is
  decisive against the captured 2.35 s stall, and JoypadOS reports the same remedy working, but it
  has not yet been physically validated on this hardware.
- Whether the app's 5 s `background-input-poll` cadence contributes by forcing repeated
  sleep/wake transitions: **Hypothesis**. Not tested.

## Remaining unknowns

- What provokes the SoC wake fault. Adapter workload correlates, but nothing here isolates a
  trigger, and the fault is in vendor firmware we cannot instrument.
- Whether other Android hosts show it. This is one tablet.
- Whether the requested parameters are accepted; Android may refuse the update. The adapter logs
  the request status, so this is answerable on the next flashed run.

## Related

- Radio-fairness change to idle Classic inquiry: see `ns2_bt_inquiry_restart_delay_ms()`.
  Motivated by the earlier Mode-1 `PAGE_TIMEOUT` reports, **not** validated by this experiment —
  Mode 1 did not occur in these ten cycles.
- Cross-transport Classic trust: see `ns2_bt_classic_trust_present()`. Closes a real admission gap
  introduced 2026-08-20, but `reject_window` proves it was **not** the failure captured here.
- Refuted paths recorded in [`refuted-hypotheses.md`](refuted-hypotheses.md).
