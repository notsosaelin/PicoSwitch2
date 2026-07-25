# Tooling — Wishlist & Plan (Archived)

> The canonical home for **all** tooling and infrastructure planning in this project. Part wishlist,
> part roadmap: every tool worth building, why it matters, what it unblocks, what already exists to
> build it on, and the order to build them in.
>
> **Thesis:** we have hit the ceiling of what *passive observation* of an unhacked console can tell
> us. Nearly every open item — console-native motion `0x09`, NFC/amiibo, the Joy-Con 2 rotation
> question, the pile of "reused verbatim / opaque field" bytes — is blocked on the same thing: **we
> cannot see what the console does.** PLAN.md already records this (motion "blocked on a genuine
> console-side trace," NFC "blocked on a genuine transaction capture," "console-side capture
> infrastructure — recommended next investment"). So the highest-leverage work is not a feature; it
> is **turning the console from a pass/fail black box into an instrumented, queryable oracle.** Every
> feature gets cheaper the moment that exists.
>
> Status legend: ✅ Complete · 🟡 In Progress · 🔵 Partial/exists ad-hoc · 🔴 Blocked · ⬜ Not started.

---

## 0. The enabling problem: one USB-C port

The dongle has **one** USB-C port, and while it is being a controller that port is the data link to
the console. The existing PC link (config mode) works by **re-enumerating that same port** as a USB
CDC device (`tud_disconnect()` → reconnect, `usb.c:177`) — so it is **mutually exclusive** with the
controller role. You cannot be a controller to the Switch *and* a CDC device to a PC at the same
time over that port.

**Therefore all live, connected-to-the-Switch observability must be out-of-band.** This is the
single most important design constraint in this document — every Tier-1/2 tool rides on it. The
board helps: stdio is currently compiled out entirely (`pico_enable_stdio_usb 0` /
`pico_enable_stdio_uart 0`), so there is a clean slate, and the board is a **Pico W / Pico 2 W with
CYW43** (WiFi+BT) already initialized for BTstack.

### Out-of-band channel options

| Ch. | Channel | Pros | Cons | Verdict |
|---|---|---|---|---|
| **A** | **UART over GPIO** (2 pins → USB-UART dongle to PC) | Simple, robust, works on every board, no radio contention, deterministic timing | Needs GPIO header/solder access + a $3 adapter | ✅ **ACTIVE ROUTE** — live query channel implemented; full protocol stream remains Tier 1. See §0.1–0.2. |
| **B** | **WiFi telemetry** (CYW43 → UDP/TCP to PC) | **Zero cable conflict**, fully wireless, high bandwidth | Shares the one CYW43 radio with Bluetooth (controllers are BT) → **coex contention**; adds a network dependency | **Best when it works** on W boards; validate BT+WiFi coex under load before trusting timing. |
| **C** | **Companion "sniffer" Pico** (main dongle → GPIO → 2nd Pico → USB CDC to PC) | Clean separation, 2nd Pico can also host the wire tap (Ch. D), offloads formatting | Second board + wiring | Great for a bench rig; overkill for quick iteration. |
| **D** | **Passive USB-C interposer / D+·D- tap** (between dongle and console) | **Ground truth** of the actual wire, independent of our firmware | Captures bytes, **not** firmware-internal decisions; needs a tap board + USBPcap/logic analyzer | Complement, not replacement — pair with A/B for "wire vs. what we thought we sent." |

**Decision (committed):** we are pursuing **UART-over-GPIO (Channel A)** as the trace channel. It is
the simplest to stand up and the broadest in coverage — it works on **every** board (not just the W
variants), needs no radio-coex validation, and gives deterministic timing. WiFi (B) remains a
future opt-in on W boards once coex is proven; the companion Pico (C) and USB-C tap (D) stay in the
bench kit as ground-truth complements. Everything below assumes Channel A carries the data out.

### 0.1 Channel A — chosen hardware & wiring

**Cable:** a **USB-to-TTL 3.3V-signal serial cable** — FT232RL was the original example; the bench
cable obtained for this project identifies as a Silicon Labs CP2102 (`VID_10C4:PID_EA60`) and is
equally suitable once its CP210x VCP driver is installed. Only the **3.3V signal** rating matters;
any 5V supply wire is a power output we do not use. The adapter enumerates as a COM port on
Windows/Linux/macOS.

**Pico 2 W default UART0:** `GP0 = TX` (physical pin 1), `GP1 = RX` (physical pin 2), `GND`
(physical pin 3).

| Cable wire | Pico 2 W pin | Note |
|---|---|---|
| **TX** (out) | **GP1 / pin 2** (Pico RX) | TX→RX — **must cross** |
| **RX** (in) | **GP0 / pin 1** (Pico TX) | RX→TX — **must cross** |
| **GND** | **GND / pin 3** | required — shared reference |
| **VCC / 5V power** | **leave disconnected** | Pico is powered from the Switch over USB-C; connecting VCC back-powers it *and* would put 5V in the circuit |

**Safety:** GPIO safety is set by the *signal* level (3.3V, matching the not-5V-tolerant RP2350),
not the power pin. With VCC left off, 5V is never in the circuit. For clone cables, verify the
cable's **TX idles at ~3.3V** (measure TX→GND) before connecting it to the Pico's RX; the reverse
direction (Pico 3.3V TX → cable RX) is always fine.

**Firmware status:** implemented without enabling stdio. `pico_enable_stdio_uart(..., 0)` remains
unchanged, so the project's many existing `printf` calls cannot flood or block the controller run
loops. `src/ns2_uart_diag.c` owns UART0 directly and performs only bounded FIFO reads/writes from
core 0. Standard NS2 builds enable it with `NS2_UART_DIAG=ON`; `build.ps1 -NoUartDiag` provides an
explicit opt-out.

The dongle stays plugged into the Switch over USB-C the entire time; the UART pins are physically
independent, so controller operation and PC tracing run concurrently.

### 0.2 Implemented live query and retained-trace channel

The UART tooling now covers both the controller-update investigation and the first bounded Tier-1
protocol-trace increment:

- **Transport:** UART0, GP0 TX / GP1 RX, **115200 8N1**, no flow control.
- **Scheduling:** `ns2_uart_diag_task()` runs after TinyUSB service with a maximum of 16 RX and 8 TX
  bytes considered per core-0 loop. It checks FIFO readiness before every byte and never waits for
  the PC or for UART space.
- **Framing:** one ASCII command line in, JSON-lines out. Ordinary queries return one line.
  `trace dump` freezes the retained snapshot and returns its manifest; the PC reader then pulls each
  record individually with `trace read N` and synthesizes the final `trace:end` line. Keeping one
  request and one response in flight avoids sustained-stream loss across shallow USB-UART bridges.
- **Commands:** `ping`, `fwreads` (alias `status`), `clear`, `profile`,
  `profile default`, `profile C.M.m B.M.m D.M.m`, `btversion request`, `btversion`,
  `trace status|clear|start|stop|dump`, internal pull command `trace read N`, `reenumerate`, and
  `help`.
- **Current data:** exact active controller/BT/DSP profile, EP0 firmware-info query count, command
  `0x10` query count, and every unique command-`0x02` memory-read subcommand/address/length/count.
  No memory response bytes and no command-`0x0D` update payload are retained.
- **Live firmware-profile A/B:** `profile C.M.m B.M.m D.M.m` replaces all three version tuples in
  RAM and requests a same-personality 100 ms USB detach/reconnect. Both EP0 and command `0x10`
  are rebuilt from the same active profile. It does not write flash or reset Bluetooth; a power
  cycle or `profile default` restores the compiled identity.
- **Genuine-controller version bridge:** after a real Switch 2 controller is paired to the dongle,
  `btversion request` marshals the read-only native command `0x10/0x01` onto the BTstack thread;
  `btversion` returns its raw 12-byte reply and decoded controller/BT/DSP tuple. It performs no
  controller memory write, configuration change, or firmware-update command.
- **Retained protocol trace:** disabled by default. When explicitly started, USB/core0 copies EP0
  setup/reply, vendor bulk command/reply, and HID output events into a 128-record RAM ring. Each
  record retains its timestamp, direction, active personality, command/report identifiers, original
  length, and first 24 payload bytes. A full ring overwrites the oldest record and counts the loss.
  UART output is paced in eight-byte chunks separated by a nonblocking hardware-idle boundary. The
  PC reader pulls, parses, and validates one record before requesting the next; it verifies sequential
  record numbers, enum values, required fields, and payload framing. No JSON formatting or UART
  access occurs in USB callbacks. High-rate HID input reports are
  deliberately excluded from this first increment so they cannot erase the handshake immediately.
  Trace payloads can contain controller/console addresses, synthetic serial data, calibration
  prefixes, and pairing challenge material; treat dumps as bench data and redact them before adding
  them to public issues or commits.
- **Same-personality re-enumeration:** `reenumerate` uses the validated 100 ms detach path without
  touching Bluetooth, allowing a fresh Pro2, GC, or Joy-Con 2 handshake to be captured while the
  UART cable stays attached.
- **Windows reader:** `tools/read_uart_diag.ps1`; it recognizes FTDI, CP210x, CH34x and PL2303
  adapters automatically when exactly one is present, or accepts an explicit COM port. Its
  `-OutputPath` option writes the validated trace as UTF-8 JSONL for later decoding and comparison.
- **Decoder/differ:** `tools/ns2_trace.py decode` renders a timestamped semantic timeline;
  `tools/ns2_trace.py diff` aligns two traces and distinguishes known fields, unknown bytes, and
  redacted session-specific pairing material. `--strict` restores byte-for-byte prefix comparison.
  See [`../switch2/uart-trace-tooling.md`](../switch2/uart-trace-tooling.md).
- **Hardware validation (2026-07-21):** the pull reader recovered all 63 chronological records from
  a Pro2 same-personality re-enumeration on a real Switch 2 with zero ring overwrites or transport
  corruption. The paired genuine Pro Controller 2 returned raw version tuple
  `020104020C00000000020300`, matching the emulated `2.1.4 / 12.0.0 / 0.2.3` response observed in
  the same console handshake.

Usage while USB-C remains attached to the Switch:

```powershell
.\tools\read_uart_diag.ps1 -List
.\tools\read_uart_diag.ps1 -Port COM5 -Command ping
.\tools\read_uart_diag.ps1 -Port COM5 -Command fwreads
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'profile 255.255.255 255.255.255 255.255.255'
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'profile default'
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'btversion request'
.\tools\read_uart_diag.ps1 -Port COM5 -Command btversion
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'trace clear'
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'trace start'
.\tools\read_uart_diag.ps1 -Port COM5 -Command reenumerate
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'trace stop'
.\tools\read_uart_diag.ps1 -Port COM5 -Command 'trace dump' -OutputPath dumps/pro2-a.jsonl
python tools/ns2_trace.py decode dumps/pro2-a.jsonl
python tools/ns2_trace.py diff dumps/pro2-a.jsonl dumps/pro2-b.jsonl
```

The firmware initializes UART only after the Pico 2 W's selected system clock is applied, so the
validated 300 MHz build still derives the requested baud correctly.

---

## Tier 1 — Observability (build first)

### 1.1 On-device protocol tracer 🔵 *(keystone; retained command tracer implemented)*
- **What:** structured, timestamped log of everything the console solicits and we answer — every EP0
  vendor request (`bRequest` 0x02/03/04), every command `0x01`–`0x18` + subcommand, every report-ID
  selection, feature-mask negotiation (`0x0C`), memory read address/length, and the report cadence.
- **Unblocks:** the Joy-Con 2 rotation/registration question, motion `0x09`, NFC, wake, and every
  "does the console actually read this?" question. Converts *"the console rejected us"* into
  *"console asked X at T+3ms, we answered Y, it stopped soliciting Z."*
- **Current 🔵:** the §0.2 retained ring captures the low-rate, semantically important EP0, vendor
  bulk, and HID-output seams across all four native personalities. Host tests pin disabled mode,
  truncation, wrap order, and overwrite accounting. Remaining increments are selective/sampled HID
  input capture, audio-class control events, compact binary streaming for larger captures, and the
  decoded viewer in §1.2.
- **Sketch:** a ring buffer of fixed-size trace records (`{t_us, dir, kind, addr/cmd, len, first_N
  bytes}`) filled at the EP0/command/report seams, drained over the Ch. A/B channel by a low-priority
  task. Binary framing (not printf) so it survives high rates without flooding core1 (the project has
  already been bitten by printf-flood starving the run loop — `switch2_ble.c:231-242`).
- **Effort:** medium. **Depends on:** §0 channel. **This is the single highest-value tool.**

### 1.2 PC-side trace viewer/decoder 🔵
- **What:** host program that reads the Ch. A/B stream and renders it as a readable, filterable,
  timeline of decoded events (using the Tier-3 decoder library for byte→meaning).
- **Unblocks:** actually *using* 1.1 in real time; correlating on-screen console behavior with wire
  events.
- **Current 🔵:** the dependency-free `tools/ns2_trace.py` validates retained JSONL, renders the
  decoded command timeline, and semantically diffs two captures with sensitive session fields
  redacted by default. A continuously updating UI, filters, and high-rate binary input remain.
- **Sketch:** thin CLI/TUI first (tail + decode + color), optional web UI later reusing the config
  web-disk assets.
- **Effort:** low–medium. **Depends on:** 1.1, 3.1.

---

## Tier 2 — Active experimentation (turn the oracle into a query)

### 2.1 Fault-injection / field-mutation harness 🔴→⬜
- **What:** runtime overrides for individual identity bytes, feature responses, report fields, and
  constants — driven by config-mode commands (or a compile-time table) — so we can change one thing
  and watch the console react via Tier 1.
- **Unblocks:** the project's **deepest uncertainty** — the many "reused verbatim / opaque field /
  semantics unknown" bytes (the `0x13040` blob, `ctrl_info`, identity constants, the derived keys,
  the `01 06 01`/`01 04 01` type bytes). Mutation is the **only** way, short of hacking the console,
  to learn *which bytes the console validates* vs. which are incidental.
- **Current:** none — `config.c` has no override hooks.
- **Sketch:** a small keyed override map consulted by the identity/command builders; config commands
  `set-override <field> <bytes>` / `clear-override`. Pairs with 1.1 to log "override active → console
  reaction."
- **Effort:** medium. **Depends on:** Tier 1 (to see the reaction). **Second-highest value.**

### 2.2 Variable-sweep "experiment mode" ⬜
- **What:** automate 2.1 — sweep one field across a value set, hold each for N seconds, and log the
  console reaction, so a hypothesis becomes a *runnable* experiment instead of prose in a doc.
- **Unblocks:** systematic resolution of opaque fields and timing thresholds without manual
  babysitting.
- **Sketch:** a scripted driver (PC side over Ch. A/B, or an on-device sequencer) that steps 2.1 and
  timestamps into the 1.1 trace.
- **Effort:** medium. **Depends on:** 2.1, 1.1.

### 2.3 Report replay ⬜
- **What:** replay a captured *genuine* controller session's reports to the console from the dongle.
- **Unblocks:** "does the console accept a byte-exact genuine stream from us?" — isolates
  encoder/mapping bugs from identity/handshake bugs.
- **Sketch:** feed the Tier-3 corpus's decoded report stream into the personality's report path in
  place of live input.
- **Effort:** low–medium. **Depends on:** 3.1/3.2.

### 2.4 Timing/cadence probe ⬜
- **What:** deliberately vary poll interval, jitter, and response latency to map the console's timing
  tolerances (the handshake is known timing-sensitive — wake-from-sleep depends on it).
- **Effort:** low. **Depends on:** Tier 1.

---

## Tier 3 — Capture & analysis (stop reinventing parsing)

### 3.1 Unified protocol-aware decoder library 🔵→⬜
- **What:** one library + CLI that knows the Switch 2 command/report/factory grammar and turns *any*
  input — USBPcap, nRF52840 BLE, SPI dump, or a Tier-1 trace — into structured, diffable, timestamped
  events.
- **Unblocks:** everything analysis-shaped; kills the accreting one-off scripts.
- **Current 🔵:** `ns2_trace.py` is the first shared grammar/CLI for retained UART records and known
  USB command fields. ~15 older scripts in `tools/` (`analyze_sw2_*.py`, `extract_*.py`,
  `pcapng_parse.py`, `verify_*.py`, `switch2_input_viewer.py`) still re-implement USBPcap, BLE,
  input-report, and factory parsing; those formats remain to be consolidated.
- **Effort:** medium. **Highest analysis ROI.**

### 3.2 Indexed capture corpus + manifest 🔵→⬜
- **What:** one provenance-tagged, queryable index of every capture — device, PID, firmware version,
  date, scenario, transport, path.
- **Unblocks:** "do we have a capture of X?" as one query instead of `find`/`grep` across ≥4 trees.
- **Current 🔵:** captures scattered across `usbpcaptures/`, `dumps/`, `nso-gc-refs/.../captures/`,
  `docs/experiments/*-captures/`. No index.
- **Sketch:** a `captures/manifest.(json|md)` + a tiny query CLI; adopt a naming/tagging convention.
- **Effort:** low. **Do this early — it's cheap and compounds.**

### 3.3 Semantic capture/trace differ 🔵→⬜
- **What:** field-aligned diff of two captures/traces (genuine vs emulated), reporting *semantic*
  deltas, not hex noise.
- **Unblocks:** the A/B methodology that has already produced the highest-yield findings
  (`docs/experiments/2026-07-19-usb-command-ab-diff.md`) — as a standing tool, not per-experiment
  scripts.
- **Current 🔵:** retained UART traces have a field-aware differ with address-aware alignment,
  session-material redaction, strict raw-prefix mode, and live two-capture hardware validation.
  Cross-format USBPcap/BLE comparison awaits the remaining 3.1 adapters.
- **Effort:** medium. **Depends on:** 3.1.

### 3.4 SPI factory-data decoder 🔵→⬜
- **What:** structured decode of controller SPI dumps (identity, serial, calibration, colours, keys)
  with confidence tags — promote the repeated ad-hoc analyses into one tool.
- **Current 🔵:** `tools/analyze_sw2_*.py` + per-experiment markdown decodes (serial/colour/motion).
- **Effort:** low–medium. **Depends on:** 3.1 (share the grammar).

---

## Tier 4 — Architecture & dev tools

### 4.1 Capability/profile authoring + validation ⬜
- **What:** tooling for the **declarative controller-profile** layer PLAN.md already wants — author a
  source→normalized→personality mapping as data, validate it covers every capability, and flag
  **lossy drops**.
- **Unblocks:** collapses the hardcoded per-driver × per-personality C matrix; would have
  immediately surfaced the SL/SR and paddle drops the Joy-Con 2 audit found by hand.
- **Effort:** medium–high (couples to the architecture work). **Depends on:** a capability-typed
  normalized model.

### 4.2 Mapping visualizer ⬜
- **What:** render, per profile, exactly which source control lands on which personality output —
  a wiring diagram that makes dropped/renamed controls obvious.
- **Unblocks:** mapping audits become a glance, not a code read.
- **Effort:** low. **Depends on:** 4.1 (or the current hardcoded tables).

### 4.3 Host-side conformance harness (expand what exists) 🔵
- **What:** replay genuine captured sessions through our pure encoders and **assert byte-equivalence**;
  run on every build to catch drift, especially after console firmware updates.
- **Current 🔵:** strong host golden tests already exist (`tools/test_*.c`, e.g.
  `test_switch_joycon2_report.c`, `test_ns2_pairing_crypto.c`). Extend from hand-written vectors to
  **corpus-driven** vectors (3.2).
- **Effort:** low–medium. **Depends on:** 3.1/3.2.

### 4.4 Firmware-version stamping in traces/captures ⬜
- **What:** every trace record and capture carries the firmware git rev + board, so results are
  reproducible and comparable across versions.
- **Effort:** trivial. **Do it as part of 1.1.**

---

## Tier 5 — Hardware harnesses (bench rig)

### 5.1 Passive USB-C interposer / D+·D- tap ⬜
- **What:** a breakout between dongle and console exposing the data lines to USBPcap / a logic
  analyzer — ground truth of the actual wire.
- **Unblocks:** "wire vs. what we thought we sent"; validates Tier 1 against reality.
- **Effort:** low (hardware); off-the-shelf USB-C breakouts exist. **= Channel D.**

### 5.2 Companion "sniffer" Pico ⬜
- **What:** a second Pico that ingests the main dongle's GPIO trace and presents USB CDC to the PC;
  can also host 5.1's tap.
- **Effort:** low–medium. **= Channel C.**

### 5.3 Console-response capture rig ⬜ *(ambitious, transformative)*
- **What:** HDMI capture of the console + automated button injection → the console's *reactions*
  become machine-observable (e.g., "did the controller icon appear? did the cursor move?").
- **Unblocks:** **closes the loop** — turns Tier 2 sweeps into fully automated, unattended
  experiments with a machine-readable oracle. The endgame for RE velocity.
- **Effort:** high. **Depends on:** 2.2 for the input side.

### 5.4 Automated source-input injector ⬜
- **What:** drive the *source-side* BT controller (or a synthetic BT HID device) programmatically for
  reproducible inputs, so experiments aren't gated on a human pressing buttons.
- **Effort:** medium. **Pairs with:** 5.3.

---

## Build order (dependency-ranked)

1. **§0 out-of-band channel (UART-over-GPIO, chosen)** — ✅ live query/A-B/BLE-bridge channel
   implemented and hardware-validated over the bench CP2102 cable while attached to a Switch 2.
2. **1.1 on-device tracer** — 🔵 retained command tracer and pull transport are hardware-validated;
   sampled input/audio and high-rate binary streaming remain.
3. **3.2 capture corpus index** — cheap, compounds immediately, do it in parallel.
4. **2.1 fault-injection harness** — highest-value *active* tool; needs 1.1 to read reactions.
5. **3.1 decoder library** + **1.2 trace viewer** — 🔵 retained UART trace path implemented;
   USBPcap/BLE/factory adapters and a live UI remain.
6. **3.3 semantic differ** — 🔵 UART-to-UART comparison implemented; cross-format comparison remains.
   **4.3 corpus-driven conformance** and **2.2 sweep mode** follow as force-multipliers.
7. **Tier 4 profile tooling** — alongside the architecture work.
8. **Tier 5 bench rig** (5.1 tap → 5.2 companion → 5.4 injector → 5.3 loop) — as time/hardware allow.

**If only one thing ever gets built:** §0 + 1.1 (a trace channel and a tracer). It is the difference
between guessing from captures and *watching the console think*.

---

## What already exists (build on, don't rebuild)

- **Host golden tests** — `tools/test_*.c` (encoders, pairing crypto, player LED, wake, rumble). Solid
  foundation for 4.3.
- **Ad-hoc analyzers** — `tools/analyze_sw2_*.py`, `extract_*.py`, `pcapng_parse.py`,
  `verify_*.py`, `switch2_input_viewer.py`. Knowledge to fold into 3.1/3.4.
- **Config-mode CDC + web disk + live views** — `config.c`, `get_global_raw_report`/`get_global_device`.
  Reusable for 1.2 and 2.1's command surface (but **port-exclusive**, §0).
- **Capture sources** — USBPcap (`usbpcaptures/`), nRF52840 BLE (`dumps/BLE CAPTURE/`,
  `nso-gc-refs/.../captures/`), SPI dumps (`dumps/`). Inputs for 3.1/3.2.
- **A/B-diff experiment** — `docs/experiments/2026-07-19-usb-command-ab-diff.md`. The pattern 3.3
  productizes.

## References

- PLAN.md "Console-side capture infrastructure" (🟡 recommended next investment), "Console-native
  motion", "NFC / amiibo" (both 🔴 blocked on genuine console-side captures).
- `src/usb.c:177` — config mode re-enumerates the single USB-C port (the §0 exclusivity).
- `CMakeLists.txt:16-20,216-219` — Pico W / Pico 2 W board + CYW43 (WiFi telemetry option),
  `:227-228` — stdio compiled out (clean slate for a trace channel).
- `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch2_ble.c:231-242` — the printf-flood-starves-
  core1 lesson (why 1.1 must be binary/rate-limited).
- `JOYCON2-AUDIT.md` §5/§9 — the rotation question that 1.1 + 2.1 would settle;
  `../switch2/command-surface.md` — the command surface 1.1 would trace.
