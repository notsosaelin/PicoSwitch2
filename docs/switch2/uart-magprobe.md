# UART native-motion `magprobe`

`tools/ns2_magprobe.py` is the read-only host analyzer for genuine Switch 2 Pro Controller
length-`0x1E` and length-`0x28` motion PDUs captured through the PicoSwitch2 UART bridge. It is
separate from the production motion path: running it cannot alter controller configuration,
console-facing reports, or firmware timing.

## Evidence boundary

The decoder labels only fields established by current source, host round trips, and direct
captures:

- 12-bit IMU timer and four-bit sub-index;
- primary and secondary sensor status;
- the length-`0x1E` smallest-three quaternion and its omitted-component state;
- the three signed 32-bit acceleration lanes in length-`0x1E`;
- normal length-`0x28`, status-`0x0D` G6/G7/G8 values;
- every remaining unexplained byte and bit, retained by payload-relative offset.

Status-`0x0F` escalation PDUs are never decoded as normal G6/G7/G8 data. Their raw payloads and
status values remain available as separate evidence. Status forms other than normal `0x0D` and
escalation `0x0F` are also counted separately. The `0x28[p16..p27]` middle block is deliberately
unknown: interpreting it as the length-`0x1E` `int32` acceleration layout produces physically
impossible stationary values and signed-32-bit wraps. The analyzer instead interpolates the
bracketing genuine `0x1E` acceleration for movement gating.

The safe label is currently **G6/G7/G8 vector**. It is not raw magnetometer output. Earlier work
called it a body-frame magnetic vector because:

1. a nearly constant vector magnitude while stationary;
2. rotation with the controller in a physical pitch capture;
3. recovery of a nearly constant **within-capture** world-frame vector after applying the
   nearest/interpolated genuine `0x1E` quaternion.

That behavior also fits the vector part of a second, controller-fused quaternion—the source bitmap
itself calls the group “Magneto Quaternion.” Reconstructing a positive scalar as
`sqrt(1 - G6^2 - G7^2 - G8^2)` is numerically possible for every normal sample in the current
corpus. This is a candidate representation, not yet a decode: there is no independently confirmed
component order, scalar sign, multiplication order, or relationship to the live quaternion.

Direct raw-sensor interpretation is ruled out at the wire level. G6/G7/G8 are signed 22/22/20-bit
fields and observed integer values exceed signed 16-bit range by a wide margin. An AK09919C, if
physically present, exposes signed 16-bit axes; controller firmware could still consume and
pre-process such a sensor, so this result rejects only **raw AK09919C samples in `0x28`**, not the
chip itself.

World-frame means from different sessions must not be compared as absolute north until the
quaternion's connection/startup yaw epoch is characterized.

The original field sketch is preserved as
[`assets/pdu-bitmap.png`](assets/pdu-bitmap.png). Third-party discussion exports used as research
leads—not as primary protocol evidence—are stored under `dumps/research/`. Direct UART JSONL
captures under `dumps/BLE CAPTURE/` and reproducible host analysis remain authoritative over those
discussion transcripts.

## Removed DualSense reference encoder — hardware-refuted packet model

The firmware contains an offline-validated codec for the exact G6/G7/G8 packing. A short-lived
UART-gated generator used that codec to select one length-`0x28` packet in four DualSense samples.
Its G6/G7/G8 lanes contained the vector part of a second, gravity-corrected quaternion while all
undecoded leading/middle lanes came from one genuine normal template. The experiment separated two
claims:

- the G6/G7/G8 bit packing is byte-exact and host-tested;
- the complete synthesized `0x28` packet is valid enough for the console.

The first claim remains supported. The second is refuted: enabling the gate in Splatoon 3 produced
immediate random motion even though UART reported selected length 40, valid changing G6/G7/G8
values, and zero representation rejects. Disabling the gate immediately restored emitted
length 30 and normal motion without reflashing.

Therefore the leading/middle lanes cannot be held at a static genuine template. The console
consumes or cross-validates changing information there, so those lanes must be decoded and made
dynamically coherent before another synthetic `0x28` trial. See the permanent negative-result
entry in [`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md).

The generator, secondary quaternion, UART commands, and runtime gate were then removed from source.
This prevents accidental reactivation of a packet model already rejected by hardware. The passive
analyzer, captures, and exact G6/G7/G8 codec remain available for safe offline work.

## Capture

With the real Pro Controller 2 connected to the dongle, the dongle connected to the Switch 2, and
UART connected to the PC:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'motionpair capture' `
  -CaptureMs 850 -OutputPath 'dumps/BLE CAPTURE/pro2-mag-baseline.jsonl'
```

The on-device ring captures exact native PDUs before UART transfer. `CaptureMs` is limited to 850
ms because the ring holds 127 paired-motion records; always require the final `dropped` count to
be zero.

The `ds5_*` fields in this JSONL format belong to the earlier paired-controller experiment. They
are not used by `magprobe`; stale DualSense metadata therefore cannot contaminate a genuine-Pro2
analysis.

## Analyze one capture

```powershell
python tools/ns2_magprobe.py analyze `
  'dumps/BLE CAPTURE/pro2-mag-baseline.jsonl'
```

The report includes:

- integrity, duration, `0x1E`/`0x28` counts, cadence, and status forms;
- body- and world-frame magnetic means, magnitude stability, and within-capture direction spread;
- quaternion alignment quality;
- every unexplained byte lane's mask, range, unique count, and changed-bit mask;
- ranked correlations between unexplained bytes/bits and time, interpolated `0x1E` quaternion and
  acceleration, body magnetic vector, and world magnetic vector.

Correlations are explicitly exploratory. A high value in one short stationary capture can mean
that both fields drift with time; it is not a semantic assignment.

Use `--json` for machine-readable output. Use `--csv` to preserve one decoded row per `0x28` PDU:

```powershell
python tools/ns2_magprobe.py analyze baseline.jsonl `
  --csv baseline-decoded.csv --correlations 20
```

CSV rows contain the exact native PDU, decoded status/timing/magnetic fields, interpolated `0x1E`
quaternion and reference acceleration, the three raw signed views of the still-unexplained
`0x28[p16..p27]` middle block, world-frame vector, and every unexplained packing bit.

Historical full `blecap dump` JSONL is accepted directly too. The analyzer selects complete input
notifications on handle `0x000E`, validates the embedded length at report byte `0x0E`, and extracts
the native PDU at report byte `0x0F`. Other handles and capture records are ignored rather than
being mistaken for motion.

## Passive corpus analysis

`corpus` combines existing `motionpair` and `blecap` captures without averaging world-frame
directions across connection-local yaw epochs:

```powershell
python tools/ns2_magprobe.py corpus `
  dumps/pro2-native-stationary-live-2026-07-24.jsonl `
  'dumps/BLE CAPTURE/sw2_native_passthrough_live_2026-07-21.jsonl'
```

It reports per-session integrity and directional spread, aggregate G6/G7/G8 wire ranges and norm,
and the implied angle if the group is provisionally treated as a quaternion vector part. It also
states the evidence boundary explicitly: failure to fit signed-int16 rejects a direct raw-sensor
wire format but cannot identify the physical component.

## Controlled A/B comparison

For a magnetic-stimulus test, do not move the controller between captures:

1. Secure the controller in a fixed support.
2. Capture the baseline.
3. Bring the stimulus near the controller without touching it and capture again.
4. Remove the stimulus and take a recovery capture.

```powershell
python tools/ns2_magprobe.py compare baseline.jsonl stimulus.jsonl
python tools/ns2_magprobe.py compare baseline.jsonl recovery.jsonl
```

The comparison reports:

- mean quaternion-angle change;
- acceleration direction and magnitude change;
- body- and world-frame magnetic direction/change;
- normalized changes in unexplained byte lanes;
- occupancy changes for every unexplained individual bit.

The movement gate warns conservatively if quaternion orientation changes by more than one degree,
acceleration direction changes by more than two degrees, or acceleration magnitude changes by more
than three percent. A magnetic stimulus could itself change an onboard fused quaternion, so a
quaternion-only warning is a confound flag rather than proof that the fixture physically moved.
Exit status is `0` for a valid comparison within those limits, `1` when the gate warns, and `2`
for malformed input or an I/O error.

## Passive-corpus validation, 2026-07-24

A new stationary real-Pro2 capture produced 116 records over 862.787 ms with zero drops:

| Measurement | Result |
|---|---:|
| Combined stream | 133.289 Hz |
| `0x1E` / `0x28` | 86 / 30 |
| Normal / escalation / other `0x28` | 30 / 0 / 0 |
| Body G6/G7/G8 mean | `[+0.017425, -0.060423, +0.367037]` |
| Vector magnitude | `0.372386 +/- 0.000229` |
| `0x28` interval range | 14.975–37.930 ms |

Twenty-nine of the 30 `0x28` records were bracketed by genuine `0x1E` quaternions and aligned by
SLERP; one boundary record used the nearest quaternion. The earlier physical-pitch corpus also
decoded successfully while separating 25 normal, two escalation, and two other-status `0x28`
records. Its reconstructed mean world magnetic vector was approximately
`[-0.153959, -0.016456, +0.338478]`, consistent with the prior independent analysis.

The stationary capture's strongest unexplained-byte lead is `p11`, which correlates with the
interpolated quaternion at `|r|` near 1.0. This is a lead, not a decode: quaternion components and
time co-vary in that short capture. The passive raw-feature-channel comparison below is the next
discriminator; a magnetic-stimulus or controlled static-pose matrix would add evidence later but
is not required for this step.

The passive eight-capture corpus contains 1,327 PDUs, including 357 normal `0x28` forms and two
historical moving sessions. Aggregate results:

| Measurement | Result |
|---|---:|
| G6/G7/G8 decoded integer range | min `[-304160, -257061, 249206]`, max `[751947, 233664, 288962]` |
| Direct signed-int16 representation | rejected |
| Vector norm | `0.375832 +/- 0.010063` |
| Norm range | `0.358105..0.460048` |
| Implied quaternion rotation | `44.154 +/- 1.255 degrees` |
| World rotation reduced mean directional spread | 2 / 2 moving captures |

This strengthens “controller-processed/fused representation” and weakens “raw magnetic field
sample.” It does not yet distinguish a normalized reference vector from a second quaternion.

### Stationary epoch result

Four dropped-free native snapshots were captured while the genuine Pro Controller 2 remained
stationary, separated across roughly 90 seconds:

```powershell
python tools/ns2_magprobe.py epochs `
    'dumps/BLE CAPTURE/pro2-mag-epoch0-2026-07-24.jsonl' `
    'dumps/BLE CAPTURE/pro2-mag-epoch1-2026-07-24.jsonl' `
    'dumps/BLE CAPTURE/pro2-mag-epoch2-2026-07-24.jsonl' `
    'dumps/BLE CAPTURE/pro2-mag-epoch3-2026-07-24.jsonl'
```

| Epoch | Live quaternion delta | G6/G7/G8 direction delta | Reconstructed candidate delta |
|---:|---:|---:|---:|
| 0 | 0.000° | 0.000° | 0.000° |
| 1 | 4.652° | 0.054° | 0.044° |
| 2 | 9.882° | 0.036° | 0.034° |
| 3 | 16.740° | 0.099° | 0.077° |

G6/G7/G8 held norm `0.3723` while the live quaternion advanced. Treating those three bounded
components as the vector part of a unit quaternion and reconstructing the positive scalar with
`sqrt(1 - x² - y² - z²)` produces a state **216.7 times more stable** than the live quaternion
over the final epoch. Quaternion sign is physically equivalent, so the positive-scalar convention
is sufficient for this stability test.

This is strong evidence for the image's “Magneto Quaternion” label: G6/G7/G8 is the vector part of
a stable corrected/reference quaternion, while G0/G1/G2 is the drifting live orientation. It still
does not establish whether the correction comes from a physical magnetometer, another internal
reference, or synthetic firmware fusion, and component-axis semantics remain unassigned.

## Separate raw-magnetometer channel

Current public controller software enables feature bit `0x80` and reads three signed 16-bit values
from ordinary handle-`0x000A` report offsets `0x19..0x1E`. Historical research independently says
that channel is absent from handle `0x000E`. That is a separate experiment from the interleaved
length-`0x28` PDU and is the cleanest next UART-only comparison: temporarily select the raw
magnetometer feature, capture full `0x000A` reports, then restore the validated `0x27` native-motion
profile.

The firmware exposes that experiment only through UART and leaves it disabled at startup:

```powershell
./tools/read_uart_diag.ps1 -Port COM11 -Command 'magraw on'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'magraw status'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap start'
Start-Sleep -Seconds 2
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap stop'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'blecap dump' `
    -OutputPath 'dumps/BLE CAPTURE/pro2-magraw.jsonl'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'magraw off'
./tools/read_uart_diag.ps1 -Port COM11 -Command 'magraw status'
python tools/ns2_magprobe.py rawmag 'dumps/BLE CAPTURE/pro2-magraw.jsonl'
```

`magraw on` replays the public implementation's exact Pro Controller 2 initialization sequence,
including its `0x94` configure/enable mask, and verifies every controller ACK before advancing.
The unsupported `0x01/0x01` step is omitted exactly as that implementation does for PID `0x2069`.
`magraw off` runs the complete validated native-motion profile (`0x27`, calibration reads, report
rate, and native CCC) rather than assuming a mask change alone can restore state. Neither mode is
entered automatically, and the production startup path is unchanged. A stationary two-second
capture is enough to establish whether the channel exists, whether its lanes are nonzero, and
whether it has plausible sensor noise; calibration and heading semantics remain separate
questions.

An initial reduced experiment that sent only the `0x94` configure/enable pair was accepted but did
not change the stream: both the native baseline and reduced experiment retained only handle
`0x000E`. This is why the positive control now reproduces all twelve applicable reference
commands instead of attributing the negative result to the sensor.

### Pro Controller 2 positive-control result

The completed PID-`0x2069` test was also negative, with the ambiguity removed:

- all twelve applicable reference commands returned controller response `0x01`;
- the post-initialization handle-`0x000A` CCC write completed with ATT status `0x00`;
- a dropped-free capture retained 72 input reports, all on handle `0x000E`;
- handle `0x000A` emitted zero reports;
- the `0x000E` stream remained the normal interleaved `0x1E`/`0x28` native format;
- `magraw off` restored the complete `0x27` profile, and `motionusb` immediately confirmed fresh,
  owned native motion.

A same-pose corpus comparison contained 145 native PDUs (107 `0x1E`, 38 normal `0x28`). The
baseline and `0x94` sessions had G6/G7/G8 mean norms `0.372290` and `0.372405`, respectively.
Therefore feature bit `0x80` plus the reference initialization does **not** expose the public
software's signed-int16 raw channel on this genuine Pro Controller 2. That parser/profile may be
effective for Joy-Con 2 or another Switch 2 controller while yielding no raw input on PID
`0x2069`.

This result does not prove that the Pro Controller 2 lacks a physical magnetometer. It proves the
documented public raw-report route does not expose one. The interleaved `0x28` G6/G7/G8 data is
independent of that route and remains controller-processed/fused data, not direct sensor samples.

No public board source currently identifies the Pro Controller 2 sensor. Nintendo's FCC internal
photos expose the radio/NFC assembly but no readable IMU marking, public board inventories name the
MT3689BCA and PN71602 only, and iFixit's published specification lists an accelerometer and
gyroscope without naming a magnetometer. The Joy-Con 2's AK09919C is therefore not assigned to the
Pro Controller 2 by assumption.

## Calibration limitation

The known factory block at `0x13100` contains three float32 magnetometer-bias values followed by
three accelerometer-bias values. The presently dumped Pro Controller 2 has zero magnetometer bias,
so applying it would not change these results. `magprobe` does not silently assume that every
controller has the same calibration; support for per-capture calibration metadata should be added
before comparing different physical controller units.

## Automated coverage

```powershell
python tools/test_ns2_magprobe.py -v
```

Coverage includes exact genuine `0x1E` carrier decoding, exact normal-`0x28` G6/G7/G8 and raw
middle-block fixtures, `motionpair` and full-`blecap` ingestion, 32-bit Pico timestamp rollover,
malformed-capture rejection, normal versus escalation separation, aggregate wire-format
classification, the independent handle-`0x000A` raw-channel decoder, stationary-epoch
live-versus-reference stability, and the A/B movement gate.
