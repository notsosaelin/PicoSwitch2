# DualSense audio captures — 2026-07-18

These recordings preserve the acoustic evidence used during the live
Windows-PCM-to-DualSense investigation. They are diagnostic captures, not source
audio or release assets.

| File | Experiment |
|---|---|
| `tone-22_5ms.m4a` | Fixed-tone report-pair cadence at 22.5 ms; approximately 2.07 s tone / 110 ms gap |
| `tone-23_25ms.m4a` | Fixed-tone cadence refinement; approximately 1.33 s tone / 74 ms gap |
| `tone-26ms.m4a` | Fixed-tone underrun-side cadence; approximately 642 ms tone / 86 ms gap |
| `live-initial-garbled.m4a` | First live reference-clock build; approximately 9.2% audible duty |
| `live-150mhz.m4a` | Continuous 1 kHz source through the 150 MHz timer-worker build; 38.2% audible duty |
| `live-200mhz.m4a` | Same source through the 200 MHz timer-worker build; 87.1% audible duty |

The cadence-fitting captures were superseded by source verification against
DS5Dongle and daidr/dualsense-tester: the controller consumes nominal 480-sample
Opus frames at an effective 45 kHz clock. The 150/200 MHz captures remain useful
because their audible-duty ratios independently match the firmware's exact
encoded-versus-dropped PCM accounting.

The final 300 MHz foreground-worker run was validated from direct listening and
firmware counters rather than another recording: 13,225 complete PCM blocks
encoded, zero dropped, zero Opus errors, and 6,612 two-frame reports submitted.
