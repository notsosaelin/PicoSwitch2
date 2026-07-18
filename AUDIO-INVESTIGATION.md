# DualSense Bluetooth Audio — Investigation Log

> **Status (2026-07-18): 🔴 unsolved.** Speaker activation, `0x39` transport, Opus
> decode, routing, volume, and Windows mute forwarding are all confirmed working on
> hardware, but audio arrives with a **periodic ~57 ms dropout every ~560 ms**. This
> file records what was tried, what was measured, and what was ruled out, so the
> remaining work starts from evidence instead of re-running solved experiments.
>
> **Important:** the implementation and tooling described here were developed on
> `ns2-testing` and then **reverted** (commit `e6c43b0`) because one change broke
> pairing — see [§9](#9-what-broke-pairing--the-clean-build-lesson). Everything is
> preserved in git history; the commit hashes are in [§10](#10-commit-map). The
> working baseline is `7d9d47d`.

---

## 1. Goal

Bridge the Pro Controller 2 USB audio (UAC1) stream out through a **paired
DualSense's own speaker** over Bluetooth. The structurally identical reference is
[`awalol/DS5Dongle`](https://github.com/awalol/DS5Dongle) (MIT), a Pico 2 W firmware
that does the same bridge with a PC as USB host instead of a Switch 2. It streams
cleanly on the same hardware, so a working target exists.

---

## 2. What works (✅ confirmed on hardware)

The audio path is almost entirely functional. Confirmed on a Pico 2 W + real
DualSense + Windows PC:

- Speaker **activation** (extended `0x32` `AllowAudioControl` transaction).
- `0x39` **stream report** delivery and CRC.
- **Opus decode** on the DualSense (audible tone, not noise).
- **Internal-speaker routing** (`0x13|0xC0`) and non-zero volume.
- **Windows mute** forwarding (muting the Windows endpoint silences the controller).
- Controller **input, BOOTSEL, and the Windows endpoint stay stable** during audio.

The only defect is **continuity**: the audio is chopped by a periodic gap.

---

## 3. The symptom, precisely measured

Early descriptions were subjective ("garbled and dropping", "010101", "Morse-like",
"beep beep beep"). A recording of the fixed diagnostic tone
(`dumps/beeps.m4a`, ~21 s) analyzed with `tools/analyze_audio_capture.py` turned that
into numbers:

| Quantity | Value |
|---|---|
| BEEP (tone on) | mean 478 ms, median 503 ms (clusters ~462 / ~503) |
| GAP (silence) | **mean 55.7 ms, median 57 ms, sd 7 ms** (very consistent) |
| PERIOD | mean 538 ms, median 560 ms |
| Duty cycle | **89 %** |
| Gap rate | ~1.9 / s |
| Tone frequency | ~925 Hz (Goertzel), ~937 Hz (zero-cross) — the real ~1 kHz tone |

**Interpretation:** this is **not** a per-report (20 ms) throughput problem. The
transport delivers ~25 reports back-to-back perfectly, then **something stalls for a
fixed ~57 ms**, ~1.9 times a second. The tone frequency confirms we are measuring the
genuine embedded tone, not an artifact.

The test conditions that produced this measurement: **PC host** (not a Switch 2),
fixed-tone build, recording taken ~1 minute after pairing (well past the 30 s pairing
window).

---

## 4. Ruled out (with evidence)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| **Live Opus encoder / its scheduling** | ❌ Ruled out | The **fixed-tone** build (no encoder, just replays two pre-encoded frames) has the *same* gaps. The encoder is not in the path that fails. |
| **Clock drift / DualSense buffer under-/overflow** | ❌ Ruled out (physics) | A 57 ms gap per 560 ms is ~10 % of the audio missing. Crystal mismatch is ~0.01 %, not 10 %. A rate deficit cannot produce this; it is a genuine **stall**, not a buffer drain. |
| **Classic sniff mode** (periodic radio anchors) | ❌ Not the cause | Sniff was globally enabled for idle power; it was disabled in audio builds (`gap_set_default_link_policy_settings` role-switch only). The gaps persisted. |
| **Config flash writes** (a `multicore_lockout` freeze fits ~57 ms) | ❌ Ruled out on PC | The only two `save_requested` triggers are the Switch 2 wake-identity `0x15` handshake (needs the console; has no callers on the PC path) and a config-mode CDC command. Neither fires during a PC audio test. |
| **ACL TX pipeline depth** | ❌ Doesn't apply | Deepened `MAX_NR_CONTROLLER_ACL_BUFFERS`/host buffers 3→12 for audio builds; no change. Root reason: the producer only ever holds **one report** (see §6), so nothing ever queues deep enough for buffer depth to matter. |
| **Background scan/inquiry as simple radio contention** | ❌ Not cleanly the cause / entangled | A gentle "stop scan when connected" (`bc647ae`) stopped scanning by the 1‑minute mark, yet the beep was unchanged. A "hard-suppress scan at connect" instead **killed all audio** — stopping scan during connection setup breaks the DualSense link entirely. So scanning is *entangled with connection establishment* but is not the periodic-gap source, and must **not** be aggressively stopped. |

> **Update 2026-07-18:** the 1-dongle-1-controller scope landed (`0798012`, `b6bc3c9`):
> discovery now idles once a controller is HID-ready and resumes at zero connections, keyed on
> `hid_ready` so it never stops scan mid-handshake (the flaw that made the "hard-suppress" attempt
> kill audio). Hardware-confirmed with no regressions (Classic + BLE reconnect, wake, wipe/re-pair).
> This removes the general scanning contention, but per the row above it is **not expected to
> resolve the 57 ms / 560 ms periodic stall** — that was shown independent of scan state. The stall
> meter (§8) remains the decisive next step.

---

## 5. Strong-evidence clue not yet exploited

The period ~540 ms is almost exactly **18 × the 30 ms control-timer tick**
(`CONTROL_TICK_MS`), and the jitter (~520–560 ms) is ±1 tick. That points at a
**periodic blocking operation reached from the control-timer path** (or something
else on the core-1 BTstack run loop) that takes ~57 ms roughly every 18 ticks. The
57 ms duration is characteristic of a flash erase+program under
`multicore_lockout_start_blocking()` (freezes both cores) — but config flash is ruled
out on PC (§4), so the candidate would be **btstack's own TLV/bond flash bank**
(`btstack_tlv_flash_bank`, backing link-key + LE device DB) or another
`multicore_lockout` user. This was **not** confirmed — it is the top hypothesis.

---

## 6. Key architectural facts learned

- **The producer holds only one report of slack.** Fixed-tone uses a single
  `test_tone_pair_ready` bool; live Opus caps `encoded_count` at 2 (= one `0x39`
  report). It produces one report, sends it, waits, produces the next — **zero
  jitter tolerance and no pipelining.** DS5Dongle instead fills a **deep speaker
  FIFO** and drains it opportunistically (`audio_bt_task` sends whenever ≥2 frames
  exist and BT can accept), which absorbs link jitter. This is the biggest
  architectural gap vs. the working reference.
- **The DualSense uses the direct-L2CAP path**, not HID-host: it is tracked in
  `wiimote_conn` with `hid_cid == 0xFFFF`, drained via `L2CAP_EVENT_CAN_SEND_NOW`
  into a bounded `direct_output_queue`. A 548-byte `0x39` report *requires* this path
  — the HID-host path caps reports at an 80-byte buffer.
- **Report `0x39` header bytes match DS5Dongle** exactly (`0x91`, `0xD2`,
  `buffer_length`×4 = 64, packet counter += 2 per report). Protocol framing is not
  the problem.
- **This is a BT-only firmware** (`pico_cyw43_arch_none`, no WiFi). The conservative
  `MAX_NR_CONTROLLER_ACL_BUFFERS 3` + controller-to-host flow control in
  `btstack_config.h` are pico-examples defaults sized for WiFi+BT SPI-bus
  coexistence, which does not exist here. (Relaxing them did not fix the gap, but the
  observation stands for future tuning.)
- **Stopping scan/inquiry at `ds5_init` (connection time) breaks the link.** Any
  scan-management change must run from a safe point (control timer) and never during
  connection setup.

---

## 7. Remaining hypotheses (unresolved, ranked)

1. **Core-1 freeze from a periodic blocking op** (btstack TLV/bond flash write, or
   another `multicore_lockout` user) — best fit for the 57 ms duration + 18-tick
   period. **Not confirmed.**
2. **Radio / L2CAP send stall** — core 1 stays live but delivery of the 548-byte
   report periodically stalls.
3. **DualSense-side behavior** — the controller itself pauses/re-syncs its speaker
   every ~560 ms in response to how we stream (e.g. buffer management, a periodic
   status expectation). Least explored.

The **decisive next measurement** distinguishes #1 from #2: instrument the interval
between core-1 audio-timer ticks. If it spikes to ~57 ms → core-1 freeze (#1). If it
stays ~2 ms while audio still gaps → send/radio stall (#2). If neither stalls →
controller-side (#3). That instrument was built (§8) but **not yet read on hardware**.

---

## 8. Tooling built for this investigation (in git history)

| Tool | What it does | Commit |
|---|---|---|
| `tools/analyze_audio_capture.py` | Decodes any recording (ffmpeg) and reports beep/gap/period/duty + tone frequency, pure stdlib. Turns "it beeps" into numbers. **Validated** against a synthetic 20/20 ms 1 kHz pattern. | `3ef8e4c` |
| **Stall meter** | `core1MaxGapUs` (core-1 tick interval) vs `sendMaxGapUs` (interval between `0x39` sends), fed from core 1. Read in config mode via CDC `audiostat` / `audiostat reset` and a "DualSense Audio Diagnostics" web-UI panel with a plain-English verdict (CORE-1 FREEZE / RADIO-SEND STALL / controller-side). | `d0ea159` |
| `docs/switch2/dualsense-audio-bridge.md` | Durable as-built reference: build modes, data flow, `0x39` layout, rate contract, threading. | `4a669c7` |
| `build.ps1 -Tone` / `-Audio` | Reproducible builds of the fixed-tone and live-Opus variants (RP2350 only, dedicated build dirs). | `e8f740d` |

**Recommended next step:** re-apply the stall meter (`d0ea159`) in isolation — it
never touched pairing — flash the `-Tone` build, let it beep ~15 s, then read the
web-UI verdict. One number settles core-1-freeze vs radio-block and ends the
guessing.

---

## 9. What broke pairing + the clean-build lesson

Commit `bc647ae` ("stop background scan/inquiry while a controller is connected") — a
shared BT scan/pairing state-machine change — broke DualSense pairing on hardware.
The whole `7d9d47d..d0ea159` range was reverted (`e6c43b0`).

Two process lessons:

- **A change to the shared BT scan/pairing state machine must be validated in
  isolation on hardware before stacking anything on top.** Four more commits were
  built on `bc647ae`, which made the regression hard to unwind.
- **After a `git revert`, always do a `-Clean` build.** An incremental rebuild
  reused stale ninja objects and produced a uf2 that still contained the reverted
  code, which looked exactly like "the revert didn't take." `build.ps1 <board>
  -Clean` produced a pure binary and pairing worked immediately. Verify the binary
  when in doubt: `strings build/<board>/*.elf | grep <reverted-symbol>` must be empty.

---

## 10. Commit map

All on branch `ns2-testing`:

| Commit | Contents | State |
|---|---|---|
| `7d9d47d` | Battery passthrough + experimental DualSense audio bridge (project owner's work) | ✅ **working baseline** |
| `4a669c7` | Audio-bridge as-built docs | reverted, recoverable |
| `e8f740d` | BT ACL pipeline tuning + `build.ps1 -Tone/-Audio` | reverted, recoverable |
| `bc647ae` | Scan-while-connected gating — **broke pairing** | reverted |
| `3ef8e4c` | Acoustic beep/gap analyzer | reverted, recoverable |
| `d0ea159` | Audio stall meter | reverted, recoverable |
| `e6c43b0` | Revert of the above back to `7d9d47d` | current HEAD |

To recover a reverted change without its regressions, cherry-pick the specific
commit onto a fresh branch and hardware-validate it alone.

---

## 11. Cross-references

- `docs/switch2/audio-passthrough-research.md` — original feasibility research and
  the DS5Dongle protocol/licensing survey.
- `docs/experiments/2026-07-17-dualsense-live-opus-failure.md` — the chronological
  live-Opus debugging narrative that preceded this summary.
- `nso-gc-refs/DS5Dongle/src/{audio,bt,btstack_config}.*` — the working reference
  implementation (MIT).
- `src/ds5_audio_bridge.c`, `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c`,
  `.../sony/ds5_audio_packet.c` — the bridge, transport, and report builder (at
  `7d9d47d` / reverted commits).
