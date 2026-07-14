# Claude Code Handoff: Stop the Immediate GameCube Rumble Blast and Build a PC-Side USB Protocol Lab

Read this file completely before acting. Then read `CLAUDE.md`, the current banner and latest dated passes in `DATA.md`, the latest GameCube/rumble entries in `STATUS.md` and `PLAN.md`, all of `docs/switch2-gc/`, the related GameCube experiment documents, and the actual current code/diff. Preserve the dirty worktree. Do not commit, reset, clean, stash, discard, or overwrite unrelated work.

## Owner's hardware report, rewritten precisely

The latest build now successfully outputs as a **Nintendo Switch Online GameCube Controller for Switch 2**, and input/controller recognition is working. However, a new critical regression makes the build unusable:

- As soon as the dongle is switched from Pro Controller 2 mode into NSO GameCube mode, the paired Bluetooth controller immediately begins rumbling at or near full strength.
- The rumble does not stop normally.
- This occurs immediately on personality entry, before a meaningful gameplay rumble effect should be active.
- The previous gameplay USBPcap approach is not available for this investigation.
- A genuine NSO GameCube Controller will remain connected to the Windows PC as a reference device.
- The owner wants a reusable PC-side host/test endpoint that can initialize either the Pico or genuine controller, send the same safe console/Steam-style commands and HID OUT reports, read responses/input reports, and exercise the dongle without another firmware/reflash cycle for every hypothesis.

Treat “immediate full rumble on entering GameCube mode” as the current P0 bug. Stop speculative rumble tuning until it is reproduced and isolated under PC control.

## Critical corrections to the current investigative direction

Do not assume the seventeenth-pass 3 ms `bthid_task()` timer fixed the problem. The owner's new test disproves completion and materially changes the symptom: the motor starts immediately on personality entry and remains active.

Two current assumptions require revalidation:

1. `switch_gc_hid_out_report()` now forwards `data[0]` directly as a 0-255 motor amplitude, but the repository still labels the four-byte semantics Hypothesis and has only eight isolated manual-test samples. A byte ranging around `0x50..0x68` may be intensity, a header/counter, a mode value, or a packed field. “Intensity-like” is not enough evidence to drive a motor directly.
2. Polling `bthid_task()` every 3 ms attempts to avoid missing transient on/off states, but polling faster is not equivalent to preserving events. If the USB side changes on→off between polls, any sampling loop can still lose a transition. If an output driver arms a long-duration effect and only sends on changes, a lost off event remains dangerous. Inspect whether the correct architecture needs a monotonic generation/event queue or explicit stop delivery rather than another timer adjustment.

Do not immediately edit either area. First build the host-side instrument below and establish which packet/state transition actually starts the motor.

## First action: audit and establish a no-rumble baseline

Before creating the test tool, audit the complete current rumble lifecycle:

```text
USB personality transition
  -> switch_gc_reset()/mount()
  -> report_set_rumble()
  -> report.c shared rumble state
  -> feedback_get_state()/dirty generation
  -> fast and normal bthid_task() scheduling
  -> per-controller cached state/change detection
  -> controller-specific Bluetooth output packet and duration/sustain semantics
```

Answer with exact code references:

- Does switching Pro2→GameCube synchronously publish `(0,0)` before reconnect?
- Can a stale Pro2 rumble value survive in `report.c`, `s_fb[]`, or a driver's private cached rumble state?
- Does `switch_gc_init()` run before or after `g_usb_personality` changes, and can the BT task observe an intermediate state?
- Are both the 30 ms control timer and new 3 ms rumble timer calling `bthid_task()` concurrently/reentrantly on the same BTstack run loop?
- Can `rumble_dirty` be cleared by one driver/task pass before another consumer has sent the corresponding physical stop?
- Which connected-controller drivers use long sustain durations, resend-on-change, or cached output suppression?
- Does a personality transition explicitly force an off packet to the physical controller, or merely update shared memory and hope the next poll notices it?
- Does the host send a report `0x03` immediately during initialization, and what exact four bytes does it send?
- Does the same immediate packet appear when the genuine NSO controller is initialized on Windows?

Do not call the cause until one branch is evidenced.

## Main deliverable: a native Windows NSO GameCube USB host laboratory

Build a reusable tool under `tools/` that turns this PC into a controlled host for both:

1. the Pico in NSO GameCube personality; and
2. the genuine NSO GameCube Controller connected simultaneously.

This is the “endpoint on the PC” the owner has repeatedly requested. The Bluetooth controller still pairs to the Pico dongle; the PC tool acts as the USB host that replaces Steam/the console for controlled testing. Use precise terminology in docs/UI so “pairing” is not confused with opening USB interfaces.

### Preferred implementation

Prefer a small native Windows executable using the APIs already available on the machine:

- SetupAPI / Configuration Manager for safe device/interface discovery
- WinUSB for vendor interface control and bulk transfers
- Windows HID API or the already-working `pywinusb` path for HID input/output

A C/C++ or C# implementation is acceptable if it builds reproducibly with the installed toolchain. A Python implementation is acceptable only if its WinUSB/HID backend is verified first and it does not require replacing drivers or installing a libusb filter. Do not leave a half-working dependency experiment as the deliverable.

Do not bind a different driver to the genuine controller, detach its existing HID interface, use Zadig, or modify system-wide USB bindings. The Pico's vendor interface is already WinUSB-bound through its MS OS descriptor.

### Device identity and safety

Both devices share VID:PID `057E:2073`, so selecting by VID/PID alone is unsafe. The tool must enumerate and print:

- complete device instance path
- container/location path where available
- VID/PID
- raw `bcdDevice`
- serial string
- interface number/class
- endpoint addresses/types
- product/manufacturer strings

Known discriminator:

- genuine controller: captured `bcdDevice` `0x0101`
- Pico: intentionally uses `bcdDevice` `0x0111`

Require an explicit target selector such as `--target pico` or `--target genuine`. Before any write, print the resolved device and refuse if its `bcdDevice` does not match the requested target. Never fall back silently to “first matching VID/PID.”

Default operation must be discovery/read-only. Mutating/output behavior requires an explicit subcommand.

### Required interfaces and operations

The tool must independently open and exercise:

- EP0 control transfers
- vendor bulk OUT `0x02` / IN `0x82`
- HID interrupt OUT `0x01`
- HID interrupt IN `0x81`

Implement these commands, with names adjusted sensibly:

```text
gcusb list
gcusb describe --target pico|genuine
gcusb init --target pico|genuine [--profile steam|console-capture]
gcusb read-input --target pico|genuine [--count N|--duration S] [--decode]
gcusb send-command --target ... --hex "..."       # allowlisted safe commands only by default
gcusb rumble --target ... --raw "..."             # explicit opt-in
gcusb rumble-sweep --target ...                    # bounded safe sweep, explicit opt-in
gcusb stop-rumble --target ...                     # unconditional best-effort off/ZLP sequence
gcusb compare --left genuine --right pico --script <file>
gcusb replay --target ... --script <allowlisted-script>
```

The exact CLI can differ, but the capabilities may not be replaced with another prose-only plan.

### Transport logging

Log every operation directly from the tool—no USBPcap required:

- monotonic timestamp, preferably microseconds
- target/device/interface
- transfer type and endpoint
- direction
- setup packet for control transfers
- exact requested bytes
- exact response bytes
- result/status/error
- elapsed time

Support NDJSON output so traces can be diffed programmatically. Preserve human-readable output too.

This tool only observes transfers it initiates/reads; do not falsely describe it as a passive console-bus sniffer.

### Safe command allowlist

Default allowlisted behavior should include only already observed, non-destructive operations required for enumeration, initialization, report selection, feature reads, input, and rumble testing.

Explicitly reject by default:

- firmware update commands
- SPI/factory writes
- calibration writes
- pairing-key or identity writes
- bond mutation
- safe-mode entry
- unknown vendor writes

Raw arbitrary writes, if implemented at all, must require an unmistakable `--unsafe` flag and are not needed for this task.

## Rumble laboratory requirements

The rumble subcommands must be designed so a failed experiment cannot leave either the genuine controller or paired Bluetooth controller buzzing indefinitely.

Requirements:

- Begin every rumble experiment by sending/establishing off.
- Use low amplitudes and short durations first.
- Cap each pulse duration independently of host refresh.
- Always issue the best-evidence stop sequence in a `finally`/scope-guard path.
- Handle Ctrl+C, exceptions, timeout, device removal, and normal exit by attempting stop.
- Provide a standalone `stop-rumble` command that does not depend on prior tool state.
- Do not begin with `0xFF` or a full-range sweep.
- Print a warning and require explicit confirmation/flag before driving the genuine controller's physical motor.
- For Pico tests, confirm a Bluetooth controller is connected before sending rumble, where the firmware exposes that status.

Test both candidate stop mechanisms separately:

- a genuine zero-length interrupt OUT/ZLP, if Windows APIs permit it on this endpoint
- report `0x03` with a four-byte zero field

Record whether each target accepts the transfer and whether its motor actually stops.

## Deterministic investigation sequence

Use the tool to answer the P0 bug with the fewest variables.

### Experiment 1: personality-entry baseline, no host rumble writes

1. Pair/connect a controller to the Pico in Pro2 mode.
2. Explicitly force shared/physical rumble off while still in Pro2 mode using existing safe behavior.
3. Switch to GameCube mode.
4. Have `gcusb` enumerate/init only as required for input, but send **no HID OUT rumble report**.
5. Observe whether the motor starts.

Interpretation:

- Starts before any `0x03` write: stale shared/driver state or transition/forwarding bug.
- Starts only after a particular init command: that command is being misrouted/interpreted as rumble or changes state indirectly.
- Starts only after an actual HID `0x03`: rumble decode/field semantics are wrong.

The tool log must establish the boundary, not recollection.

### Experiment 2: exact packet bisection

Replay the known-good Steam or console-capture initialization sequence one operation at a time against the Pico, with rumble off asserted between steps. Identify the first transfer after which the motor starts.

Do not replay pairing/identity writes or destructive commands. Use the smallest safe sequence that reaches input streaming.

### Experiment 3: genuine-controller differential test

Run the same safe initialization script against the genuine controller. Compare:

- responses
- selected report ID
- immediate HID OUT traffic generated by the script
- input cadence
- motor state

Then send the same low, short, explicitly bounded rumble packet to genuine and Pico targets and stop each. If identical bytes cause materially different behavior, the error is downstream translation/BT forwarding. If the genuine device itself blasts on the same supposed “low” packet, the byte interpretation/script is wrong.

### Experiment 4: four-byte semantic sweep

Only after a safe stop path is proven, vary one byte at a time while holding the other three at their captured neutral/reference values. Start with a tiny bounded set, not 0-255:

- zero
- the minimum observed value
- one or two nearby values
- one known captured manual-test value

Measure/record observable motor on/off and relative strength. Do not label a byte “amplitude” merely because strength changed once; distinguish header/mode/counter dependencies and repeat trials.

### Experiment 5: forwarding integrity

On the Pico target, drive a known sequence with off transitions that occur faster and slower than 3 ms, 30 ms, and the physical driver's send cadence. Determine whether every off generation reaches the driver.

If state transitions can be lost, replace sampling with an evidence-backed mechanism such as:

- monotonic rumble generation counter consumed exactly once per change
- small bounded SPSC event queue carrying on/off/amplitude transitions
- explicit high-priority stop event that cannot be coalesced away

Choose the smallest correct design after measuring. Do not merely reduce a timer again.

## Immediate firmware safety fix criteria

Do not produce another firmware build until the PC tool names the first bad transition. Once it does, implement the smallest evidenced fix.

Regardless of root cause, enforce these invariants:

- Entering or leaving any USB personality publishes rumble off.
- A personality reset/mount cannot inherit a prior rumble command.
- Disconnecting the USB host or Bluetooth controller forces a physical stop.
- Unknown/malformed/short HID OUT reports can never turn rumble on.
- Initialization/vendor-bulk commands can never be routed into the HID rumble decoder.
- A stop transition cannot be lost merely because on/off changed between polling ticks.
- The physical driver receives an explicit zero/off output when required, not only a shared-state update.
- Pro2 rumble behavior remains hardware-compatible.

If `data[0]` is disproven as direct amplitude, revert that interpretation and keep the four bytes opaque until the tool establishes a safer mapping. Prefer bounded off/no-rumble over a “mostly working” decoder that can latch full motor output.

## Sources and implementation ideas to use

Use these as inputs, not authorities to copy blindly:

1. `docs/experiments/gc-stage-d-steam-diagnosis-2026-07-13.md` — real Steam bulk sequence and device-selection evidence.
2. `docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md` — raw genuine-controller transfers, eight manual rumble samples, ZLP observations, and timing re-analysis.
3. `docs/switch2-gc/protocol.md` — current evidence grades and endpoint/report layouts; correct stale contradictions discovered by the tool.
4. `E:\nso-gc-refs\switch2_controller_research\captures\usb\rumble-procon-gccon.pcapng.gz` — raw primary reference capture. Parse/replay exact safe transactions rather than relying on summary tables.
5. `tools/extract_report09_timeseries.py` and existing USBPcap analysis tools — reusable endpoint-aware parsing and NDJSON conventions.
6. `tools/switch2_input_viewer.py` — existing project pattern for a host-side interactive protocol tool; reuse UX/logging ideas where relevant, not its transport assumptions.
7. `switch_pro2.c` and the GameCube dispatcher — known working command envelopes and centralized HID OUT normalization.
8. The existing MS OS/WinUSB binding code in `switch_gc.c` — confirms the Pico vendor interface is intentionally available to a Windows host.
9. `docs/experiments/gyro-differential-re.md` — the project's own established “genuine vs Pico, same PC, same script, diff the first divergence” methodology and its report-replayer recommendation.
10. Dycool's Windows-side relay/capture code already audited in this project — useful ideas for HID/WinUSB device enumeration, timestamped transfer logging, and replay. Refresh/read the actual relevant source before borrowing code, and record its SHA/license.
11. Official Microsoft SetupAPI, Configuration Manager, HID, and WinUSB documentation or installed Windows SDK headers — primary source for interface discovery and transfer semantics.

Do not build a Pico USB MITM, install a filter driver, or require gameplay capture for this task. The Windows host harness is the lower-risk instrument that directly fits the owner's available hardware.

## Tests and acceptance criteria

Host tool tests:

- deterministic device selection with genuine and Pico connected simultaneously
- refuse mismatched `bcdDevice`
- read-only default
- allowlist enforcement
- control/bulk/HID transfer framing
- timeout/device-removal behavior
- NDJSON validity
- stop-rumble cleanup on success, failure, timeout, and Ctrl+C where testable
- replay script validation without touching hardware

Firmware tests after root cause:

- personality-transition rumble reset
- malformed/short/unknown report remains off
- ZLP and zero report stop paths
- event/generation delivery if introduced
- existing HID OUT normalization tests
- Pro2 and GC rumble regression vectors
- complete established build matrix

Hardware acceptance:

1. Pair a controller with the Pico.
2. Confirm it is not rumbling in Pro2 mode.
3. Switch to GameCube mode with `gcusb` in discovery/no-rumble mode.
4. Motor remains off throughout enumeration and input initialization.
5. Send one short low rumble pulse; it starts at a bounded low level.
6. Send stop; it stops immediately and stays stopped.
7. Repeat several times without cumulative/latching behavior.
8. Disconnect/exit mid-pulse; cleanup stops the motor.
9. Confirm normal input still streams.
10. Confirm Pro2 rumble still starts/stops normally.

Do not call this fixed because it compiles, because a watchdog eventually stops it, or because one host behaves differently. The motor must remain off on GameCube entry and obey deterministic start/stop from the PC harness.

## Documentation and handoff

Create a focused experiment document for this investigation containing question, competing hypotheses, method, raw tool logs, result, and conclusion. Update current docs only after evidence changes them.

At the end, replace `DATA.md` with a concise current handoff. The present file has accumulated seventeen passes plus a long duplicated historical body and is no longer efficient as a handoff. Preserve history in `STATUS.md`/experiment docs, but make `DATA.md` contain only:

1. Current working state.
2. Immediate-rumble root cause with exact evidence.
3. PC host-lab tool path/build/usage.
4. Genuine-vs-Pico differential result.
5. Firmware change, if any.
6. Tests/builds.
7. Hardware validation.
8. Remaining unknowns.
9. Single highest-value next action.

Nothing should be committed.

## Priority order

1. Preserve/reassess the worktree and audit the exact current rumble state path.
2. Build and verify safe deterministic genuine/Pico device discovery.
3. Implement the PC host lab with direct transfer logging and unconditional rumble cleanup.
4. Reproduce GameCube personality entry with no rumble writes.
5. Bisect initialization and HID OUT transfers to identify the first motor-start event.
6. Differentially test the same bounded sequence on the genuine controller.
7. Validate or refute `data[0]` as amplitude and the 3 ms polling hypothesis.
8. Implement the smallest evidence-backed firmware safety/root-cause fix.
9. Run host tests, firmware tests, full builds, and the deterministic hardware test.
10. Update focused docs and replace bloated `DATA.md` with a concise handoff.

Lead with the instrument and evidence. Do not ask the owner for another blind reflash before the PC harness can reproduce and control rumble on the current build.
