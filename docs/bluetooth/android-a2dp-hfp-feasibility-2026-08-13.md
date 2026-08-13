# Bluetooth audio profiles (A2DP / HFP) for the Android companion — investigation

Status: 🔵 INVESTIGATION ONLY — no code changed, nothing implemented, no recommendation to build
Date: 2026-08-13
Supersedes the transport half of: [`android-audio-feasibility-2026-08-13.md`](android-audio-feasibility-2026-08-13.md)
(which closed the question on the basis that WiFi is prohibited and HID cannot carry audio — that
remains true; this document asks the separate question of whether a *Bluetooth audio profile* changes
the answer)

## Question

PicoSwitch2 already runs several Bluetooth services (Classic HID Host, BLE GATT server for
management, HOGP client, SM/bonding). Could it also run **A2DP source/sink** or **HFP**, so the
Android companion carries game audio and/or microphone without WiFi?

## The decisive framing: who must be the sink?

"Audio" is two independent flows, and each pins a Bluetooth *role* on each side:

| Flow | Purpose | Phone's role | Adapter's role |
|---|---|---|---|
| Game audio: console → adapter → **phone** | user hears the game on the handheld | audio **sink** | audio **source** |
| Microphone: **phone** → adapter → console | GameChat voice | audio **source** | audio **sink** |

Both flows together are exactly the role pair of a **headset**: the *phone* would have to behave as
the headset (A2DP sink + HFP hands-free), and the *adapter* as the audio gateway (A2DP source +
HFP AG). That is the inverse of how a phone normally behaves, and it is the crux of this whole
investigation.

## Adapter side — available (verified locally)

The BTstack bundled with Pico SDK 2.2.0 ships everything needed, already on disk:

| Component | Path (verified) |
|---|---|
| A2DP source | `src/classic/a2dp_source.c` |
| A2DP sink | `src/classic/a2dp_sink.c` |
| AVDTP (stream transport, both roles) | `src/classic/avdtp_{source,sink,initiator,acceptor}.c` |
| HFP audio gateway | `src/classic/hfp_ag.c` |
| HFP hands-free | `src/classic/hfp_hf.c` |
| mSBC (16 kHz wideband voice) | `src/classic/hfp_msbc.c` |
| SBC codec | `3rd-party/bluedroid/…` |

The firmware currently links only `pico_btstack_classic`, `pico_btstack_ble`, `pico_btstack_cyw43`,
so none of this is compiled in — but **nothing would have to be written from scratch on our side**.
The user's intuition that "we already run other BT services" is correct, and adding another profile
is architecturally ordinary for BTstack.

## Phone side — the blocker

An ordinary, non-root Android app cannot make the phone act as an audio **sink** or an HFP
**hands-free unit**:

1. **A2DP sink.** `BluetoothProfile.A2DP_SINK` exists as a constant, but the proxy class
   (`BluetoothA2dpSink`) is a system API, and the underlying profile service is **disabled in
   ordinary phone builds** — A2DP sink is shipped mainly for automotive/head-unit images. A phone
   therefore will not accept an incoming A2DP stream from the adapter.
2. **HFP hands-free.** Same shape: `HEADSET_CLIENT` is the hands-free (headset) role, is a system
   API, and is typically not enabled on phone builds. Phones ship the *gateway* side.
3. **No app-level audio injection either.** `BluetoothA2dp` gives an app connection *state*, not
   stream control — there is no public API to connect a device or push PCM into an A2DP stream. Apps
   emit audio through `AudioTrack`/`MediaPlayer` and the **system** routes it to whatever output is
   selected.

**Consequence:** the flow we most want — game audio *to* the phone — requires the phone to be a sink,
which standard Android does not expose. This is a **build/system-level** limitation, not something an
app permission or a clever descriptor can work around, and working around it would require exactly
the privileged/hidden-API territory the companion app rules out.

### The one direction that is *not* obviously blocked

Microphone (phone → adapter) is the flow where the phone is a **source**, which is its normal role:
- the adapter would run **A2DP sink** (BTstack has it), and
- the app would capture the mic and play it as ordinary media audio, which the system routes to the
  connected adapter.

Even so, this is unattractive: selecting the adapter as the media output means **all** phone audio
goes there (notifications, etc.), the phone can no longer play anything else, A2DP adds ~100–200 ms
of latency to a voice path, and it does nothing for game audio. It solves the half of the problem
nobody asked for.

## Adjacent option worth recording: adapter → ordinary Bluetooth headphones

While investigating, one variant appeared that has **no phone-side blocker at all**: the adapter runs
**A2DP source** and streams console game audio directly to the user's own Bluetooth headphones. Both
halves exist (BTstack A2DP source + SBC; headphones are sinks by definition), so this is feasible in a
way the phone path is not.

It is recorded as an idea, not a proposal — it carries its own real costs (A2DP latency is poor for
games, radio contention with Classic HID input and BLE management, SBC encode CPU on top of the
existing audio path, and a pairing/UX surface) and it is unrelated to the Android companion. Flagged
so the finding is not lost.

## Cost on the adapter, even if the phone side worked

Worth stating so a future reader does not assume "BTstack has it" means "cheap":

- **Radio contention.** A2DP/SCO is continuous, bandwidth-hungry Classic traffic sharing one CYW43
  radio with Classic HID input, BLE management advertising/connection, and (on Pro2) the existing
  DualSense audio path.
- **CPU/RAM.** SBC encode/decode on top of a Pico 2 W audio budget that already runs Opus SRAM-
  resident on a dedicated core at 300 MHz. Pico W was already **rejected** for audio.
- **Quality ceiling on HFP.** Even in the best case HFP is 8 kHz (CVSD) or 16 kHz mono (mSBC) — the
  right shape for chat, poor for game audio.

## Conclusion

**No change to the existing decision: the Android companion does not get audio.** A2DP/HFP does not
rescue it, because the required phone-side roles (A2DP sink / HFP hands-free) are not available to a
non-root app on ordinary phone builds. The blocker moved from "no transport with enough bandwidth"
to "the phone cannot be an audio sink" — but it is still a hard blocker, and this time it is on the
side we do not control.

## Cheap way to make this empirical rather than inferred

The Android-side claims above are the standard behaviour of ordinary phone builds; they are **not
verified on the specific target handhelds**. The project has already used exactly the right tool for
this — the read-only ADB audits that confirmed `hid.device.enabled=true` on the Retroid Pocket
Classic and the AYN Thor. The same kind of one-line, non-invasive property read would settle it:

```
adb shell getprop | grep -iE "a2dp_sink|hfp.hf|headset_client|profile.*enabled"
adb shell dumpsys bluetooth_manager | grep -iE "A2dpSink|HeadsetClient|Profile"
```

If a handheld unexpectedly reports A2DP sink enabled, the mic direction becomes worth a second look
and the game-audio direction becomes genuinely possible on that device. If not — the expected result —
this question is closed on evidence rather than on general knowledge.

## Confidence

| Claim | Basis |
|---|---|
| BTstack ships A2DP source/sink, AVDTP, HFP AG/HF, mSBC, SBC | **Verified** — files listed in the local Pico SDK 2.2.0 tree |
| Firmware links none of them today | **Verified** — `CMakeLists.txt` links only classic/ble/cyw43 |
| The role pair needed is phone-as-headset, adapter-as-gateway | **Derived** from the direction of each audio flow |
| A2DP sink / HFP HF are system APIs and normally disabled on phones | **General Android platform knowledge, not device-verified** — the ADB probe above is the check |
| `BluetoothA2dp` gives no stream/connection control to apps | **General Android platform knowledge**; consistent with the documented class surface |
| Radio/CPU contention costs | **Inferred** from the measured DualSense audio budget (300 MHz, dedicated core, Pico W rejected) |
