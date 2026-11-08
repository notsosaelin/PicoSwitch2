# PicoSwitch2 Companion for Linux

The Linux companion: pair a PicoSwitch2 adapter, manage it, and use any
controller Linux can see — including a Steam Deck's own controls, with motion —
as the adapter's controller (Controller Link). Shipped as one self-contained
executable for x86_64 and for arm64; the primary target is SteamOS-style
handhelds in Game Mode.

**Status: implementation complete for v1; hardware validation pending.** No part
of this companion has yet been run against a real adapter, a real BlueZ stack
with a radio, or a Steam Deck. What *has* been validated is listed in
[§5](#5-validation).

## 1. Scope

| Area | v1 | Notes |
|---|---|---|
| Adapter discovery, pairing, repair, remove | Yes | BlueZ over the system D-Bus |
| Management: personality, colours, input source, wake, peers, controller pairing | Yes | Same shared Services as Windows |
| Controller Link from any SDL gamepad | Yes | Buttons, sticks, triggers, rumble, battery |
| Controller Link motion (gyro + accelerometer) | Yes | First C# backend with motion; see §4 |
| Steam Deck-format controllers SDL does not drive (e.g. InputPlumber's emulated Deck) | Yes | Read from hidraw: back buttons and motion included; see §4 |
| Diagnostics: log, support bundle, bond table, input-path counters | Yes | |
| Keyboard & Mouse: mode, profile library and editor, adapter banks, switch keys, mouse tuning | Yes | Port of the Windows page over the shared `KeyboardMouse*` projections |
| Amiibo: library (import, folder import, ZIP export, rename, initialize, delete, bulk select), adapter slot (send, present, eject, sync, clear, copy choice) | Yes | Port of the Windows page over the shared `Amiibo*` projections; key store below |
| Shell: slide-out navigation at the edge the player chooses, Play home, controller navigation, always full screen (F11 leaves and re-enters; Game Mode fills the panel) | Yes | The shared layout, `docs/architecture/companion-shell-layout.md` |
| Touch Gamepad: play surface, menu, layout editor, named layouts (import/export) | Yes | Port of the Windows surface over the shared `TouchGamepadService`; Avalonia renderer in `Touch/` |
| Companion audio: Play → Headset (console audio here) and Mic (this computer's microphone as the headset's) | Yes, experimental | The shared `CompanionAudioService`; SDL3 playback/capture in `Audio/`, BlueZ planes in `Bluetooth/BluezCompanionAudio.cs`; see §4 |
| Decky Loader plugin for Game Mode | Later | Starts once the app is polished |

## 2. Layout

```text
companions/linux/
  src/PicoSwitch.Companion.Linux/       platform backend: the ONLY project that touches
                                        BlueZ/D-Bus or SDL (ICompanionPlatform)
    Bluetooth/                          management GATT transport, Path C data plane,
                                        pairing gateway, radio probe, error mapping
    Input/                              SDL3 gamepad source + pure mapping; Steam Deck
                                        hidraw report reader (SteamDeckReport/Sources, Hidraw)
    SingleInstance.cs                   one process per user session
  src/PicoSwitch.Companion.Linux.App/   Avalonia shell (C#, no AXAML): pages paint the
                                        shared Services.Presentation projections
    Shell/                              Tokens (theme), Ui (layout grammar), Icons, MainWindow
  tests/PicoSwitch.Companion.Linux.Tests/
  tools/PicoSwitch.Companion.Linux.Screenshots/
                                        headless renderer: every page x size x theme to PNG
  packaging/                            build-linux.sh (x64 + arm64 executables), .desktop, icon generator
```

The shared layers are compiled from `companions/windows/src/` (Bridge.Core,
Management.Core, Companion.Platform, Companion.Services); see
[`docs/bridge/PLATFORM_BACKEND.md`](../../docs/bridge/PLATFORM_BACKEND.md) §2.1.
A behaviour rule belongs there, not here, so both companions get it.

## 3. Build, test, run

Needs a .NET 9 SDK. Everything except launching against real hardware also
works on Windows.

```sh
dotnet test companions/linux/tests/PicoSwitch.Companion.Linux.Tests
dotnet run --project companions/linux/src/PicoSwitch.Companion.Linux.App
bash companions/linux/packaging/build-linux.sh [--arch x64|arm64|all]   # Linux only
```

`build-linux.sh` runs the tests, then publishes ONE self-contained single-file
executable per architecture (arm64 is cross-published, so one x86_64 machine
builds both): `companions/linux/artifacts/PicoSwitch2Companion-<git describe>-linux-<arch>`,
each with a `.sha256`. Every native dependency (SDL3, Skia, HarfBuzz) ships a
linux-arm64 build in its NuGet package. Two builds of the same tree gave
identical hashes for both architectures (2026-11-06, WSL, same clone with its
intermediate output kept; a clean-clone comparison has not been run). CI builds
both in the `linux` job.

**Why not an AppImage.** It was the first packaging and it does not start in
Steam Game Mode: the Game Mode client runs with `NoNewPrivs=1`, which every game
inherits, and that stops the setuid `fusermount` the AppImage runtime mounts
itself with ("Cannot mount AppImage, please check your FUSE setup"; Bazzite
2026-11-06, `steamrt64/steam -gamepadui`, reproduced with
`setpriv --no-new-privs`). Its only cure was a per-shortcut launch option
(`APPIMAGE_EXTRACT_AND_RUN=1`), and the project does not ask users to set one.
A single-file executable mounts nothing. On its first launch it unpacks its
native libraries (about 16 MB) once into `~/.net/PicoSwitch2Companion/` and
reuses them afterwards.

### Companion audio without an adapter

`tools/PicoSwitch.Companion.Linux.AudioCheck` drives the shipped audio
service, decoder, SDL3 sink and microphone against a simulated adapter, and
exits non-zero when a check fails. Clock drift, BLE event timing, outages,
the decoded level, and splices in the device output are all checked.
Publish it, then run it on the device:

```sh
dotnet publish companions/linux/tools/PicoSwitch.Companion.Linux.AudioCheck -c Release -r linux-x64 --self-contained -o out
./out/PicoSwitch.Companion.Linux.AudioCheck            # all scenarios + microphone
./out/PicoSwitch.Companion.Linux.AudioCheck --control 20 --gap 4 --scenario ideal,stall --seconds 20 --no-mic   # listening test
```

The results and the listening procedure are in the companion audio
experiment record (sixth pass).

### UI layout and the screenshot harness

The shell and page placement are the shared contract in
[`docs/architecture/companion-shell-layout.md`](../../docs/architecture/companion-shell-layout.md):
the page, and a slide-out navigation (Play, Keyboard & Mouse, Amiibo,
Customization, Adapter, with the adapter's dot and Settings) at the edge chosen in
Settings → Navigation, with the same size scale as Windows (`Sizes` in
`Shell/Tokens.cs`). Play is the home screen (`Pages/PlayPage.cs`): the headset
strip over the controller card, and the Amiibo tile beside them. Within the other pages there is one layout grammar (`Shell/Ui.cs`): a
header plus cards (sections) of rows, and tabs (`TabBar`) where a page would
otherwise scroll. It is **sized by measurement, not by device**: a row puts its
controls under its label only when they do not fit; the column is capped and
centred; Play stacks and scrolls below 600x280; the navigation shows short names below a 720 px
window. The window opens at a proportion of the screen's working area (or
maximised when that is under 1280x800). No layout rule names a device or
aspect ratio, so do not add one.

Review a UI change across the matrix, both themes, before shipping it:

```sh
dotnet run --project companions/linux/tools/PicoSwitch.Companion.Linux.Screenshots -- <out-dir>     [--pages adapter,kbm,amiibo] [--sizes 720p,minimum] [--themes dark] [--amiibo tests/fixtures/amiibo]
    [--placement left|right|top|bottom] [--keys Left,Escape] [--pad Down,A,B]
```

Sizes are logical (device pixels / display scale): 720p (a 1920x1080 handheld
at 1.5x), 800p, 1080p, 1440p, ultrawide, 4x3, portrait, minimum, short (832x468,
under the 560 short-window height, where the tabs join the header), and tall (a
whole scrolling page). A page name may end in `:<tab>` to open that tab
(`kbm:1` is Keyboard & Mouse on Mouse). `--amiibo` seeds the library from a
fixture folder. A `+nav` suffix (`play+nav`) captures the page with the
navigation out. `--keys` presses keyboard keys and `--pad` sends controller
steps (D-pad directions or `ControllerButton` names, as SDL frames through the
input session) before capture, printing where focus landed after each: it is how
the controller navigation is checked without hardware.
The Touch Gamepad is not a page: `--pages touch-pro2,touch-gamecube,touch-joyconl,touch-joyconr`
opens it with that layout (the personality is set, there being no adapter), and a
`-menu` or `-edit` suffix opens its menu or drags a control in the editor.
The harness points `PICOSWITCH2_DATA_DIR` at `<out-dir>/.data`, so it never
reads or writes real companion data; the same variable works for the app.

### Steam Deck

1. Desktop Mode: download the executable for your architecture (`linux-x64`
   on a Steam Deck or an x86 handheld), `chmod +x` it (or Properties →
   Permissions → executable), and run it once.
2. For Game Mode: Steam → Add a Non-Steam Game → choose the executable. No
   launch options. In Game Mode the window sizes itself to the whole screen
   gamescope gives it, because gamescope ignores the full-screen request alone
   and would show the default window size scaled and letterboxed. That screen
   is the game resolution set in the shortcut's Properties → Game Resolution
   (Default is the panel's native resolution). gamescope publishes no UI scale,
   so the app sets one itself (`Display/GamescopeScale.cs`): one unit is the
   physical size of a dp on the AYN Odin 2 Mini (about 191 per inch), measured
   from the monitor size gamescope reports, with a floor of 1080 units of height
   for a TV. The ROG Xbox Ally X comes out at 1.7x (1129x635 units). An
   `AVALONIA_GLOBAL_SCALE_FACTOR` in the launch options overrides it.

**Dual-mode BlueZ picks the wrong bearer.** The adapter is dual-mode (Classic
for controllers and Controller Link, LE for management). BlueZ chooses the
bearer for both `Device1.Pair` and `Device1.Connect` in `select_conn_bearer()`
(`src/device.c`, 5.87): the bonded bearer if only one is bonded, otherwise the
one it saw the device on most recently, **with a tie going to BR/EDR**. An LE
advertisement whose Flags lack "BR/EDR Not Supported" also counts as a BR/EDR
sighting (`src/adapter.c`), so every advertisement produced a tie.

The adapter firmware advertised Flags `0x02` until 2026-11-08. On the Ally
(BlueZ 5.87, Game Mode) that made everything go Classic (**confirmed**: agent
over SSH, BlueZ and companion logs plus the BlueZ 5.87 source):

- Pair ran Classic SSP, which the adapter declines as an unadmitted Classic
  attempt, and BlueZ reported `AuthenticationRejected` even with the pairing
  window open. (The firmware log line is "Rejecting unadmitted Classic SSP
  confirmation", not the management Just Works one.)
- A Pair that did succeed left a Classic bond and a Classic link with no GATT.
  After that, every Connect failed with `br-connection-refused` or
  `BREDR.ProfileUnavailable`, even right after a fresh LE advertisement.

The firmware now advertises Flags `0x06` (LE General Discoverable + BR/EDR Not
Supported), so advertisements count only as LE sightings. A bond made by the
older firmware still pins BR/EDR, because the bonded rule comes first: remove
the adapter from the computer (`bluetoothctl remove <address>`) and pair again.
`PreferredBearer` and `Bearer.LE1.Connect` would name the bearer outright, but
they are experimental and absent on BlueZ 5.87. The transport also judges a link
by whether the management service is present, drops a link without it, and runs
a short LE-only scan just before Connect so the LE sighting is the freshest.

Settings live in `~/.local/share/PicoSwitch2` (`$XDG_DATA_HOME`).
Amiibo backups are in `amiibo/` there. **Amiibo keys** are in `amiibo-private/`,
a separate 0700 directory holding a 0600 file: owner-only permissions, not
encryption (Windows uses DPAPI; Linux has no equivalent guaranteed in Game
Mode, so this is the Android model). The key is validated before it is stored
and is never logged. Pairing
requires Bluetooth to be on; BlueZ performs it, so a desktop pairing prompt
may appear (see §6).

## 4. Platform decisions, and why

- **BlueZ over D-Bus (Tmds.DBus.Protocol 0.21.3)**, mirroring the Windows
  transport rule for rule: identity verified before Connected, per-attempt
  trust evidence that survives teardown, bond proven only by a completed
  subscribe, one-session teardown, a data plane that cannot outlive its
  carrier. The version is pinned to the one Avalonia.FreeDesktop requires.
- **Error mapping is conservative by design.** The shared stale-bond signature
  offers Repair on `AccessDenied`, so only BlueZ `NotAuthorized` and ATT
  security errors (0x05, 0x08, 0x0C, 0x0F) map to it. Anything unrecognised is
  `ProtocolError` — an unknown error must never manufacture a Repair offer. A
  lost system-bus connection is also `ProtocolError`: it says nothing about the
  adapter.
- **Exit releases the link explicitly.** BlueZ keeps an LE connection up after
  the D-Bus client that requested it has gone, and the adapter has room for one
  management session, so a leftover link would lock out a phone or another PC.
- **One instance per session**, via an abstract Unix socket: released by the
  kernel when the owner dies (no stale lock), and a second launch brings the
  first window forward instead of opening a second BlueZ session.
- **SDL3 for input** (ppy.SDL3-CS, libSDL3 bundled): one API for every pad,
  including the Deck's built-in controls through SDL's HIDAPI driver. Steam's
  virtual pad (28de:11ff) is excluded because it duplicates a physical one.
- **Steam Deck-format hidraw reader.** SDL's Deck driver takes only 0x1205, so
  a Valve-id controller that speaks the Deck report under another product id
  (InputPlumber's `deck-uhid` on Bazzite handhelds, 28de:12fd) is undriven,
  and Game Mode then offers only Steam's virtual Xbox 360 pad: no back
  buttons, no motion (its evdev key bitmap carries the eleven Xbox buttons
  only; Ally X, 2026-11-06). Any Valve hidraw node other than 0x1205 that
  streams Deck state reports (`01 00 09 40`) is therefore read directly; an
  SDL pad with the same Valve ids is its mirror (Steam's virtual pad copies
  them in Game Mode), excluded from the list and kept open only for rumble and
  battery. Back buttons are high-word bits 9/10 (**Confirmed** by capture);
  the remaining layout is SDL's (`controller_structs.h`, Strong evidence).
  Motion uses SDL's Deck conversion, then the one SDL → canonical transform.
- **Motion frame (Hypothesis).** SDL reports sensors in its gamepad frame
  (+X right, +Y up out of the face, +Z toward the player). The canonical frame
  is X = sdlX, Y = −sdlZ, Z = sdlY, for accelerometer and gyro alike. Unit tests
  pin the gravity anchor (flat reads AccelZ = +8192) and the sense of every
  rotation, but the mapping has not been checked against a real pad.
- **Companion audio uses SDL3 audio**, the library already bundled for
  input, so no native dependency is added. SDL picks PipeWire, PulseAudio or
  ALSA. Every buffering decision is the shared `CompanionAudioPlayout`'s; the
  rationale and simulations are in
  [`docs/experiments/companion-audio-downlink-prototype-2026-11-07.md`](../../docs/experiments/companion-audio-downlink-prototype-2026-11-07.md).
  - The microphone gets no echo cancellation from the app. Use headphones, as
    with a real headset.
  - The firmware asks for a 15 ms connection interval while audio runs,
    because BlueZ otherwise keeps its 30–50 ms default.
- **Dialogs are in-window overlays**, not separate windows: gamescope (Game
  Mode) shows one window at a time, so a modal window can land behind the app.
- **No ICU** (`InvariantGlobalization`): SteamOS is not guaranteed to ship it,
  and nothing here formats culture-sensitively.

## 5. Validation

| What | How | Result |
|---|---|---|
| Linux backend unit tests (115) | `dotnet test`, Windows and WSL Ubuntu 24.04 | Pass |
| Shared portable suites on Linux (Bridge.Core 275, Management.Core 166, Services 678) | WSL Ubuntu 24.04 | Pass |
| Real system bus without bluetoothd | `SystemBusTests`, WSL | Prompt `ServiceUnknown`, no hang |
| App launches and renders | WSLg, from source; the linux-x64 single-file executable with no system .NET (2026-11-06) | Pass |
| Second launch hands off to the first | AppImage under WSLg (before the single-file switch) | Pass; not re-run on the single-file build |
| Repeatable build | Two single-file builds of one tree, same clone, sha256 compared | Identical (x64 and arm64); clean-clone comparison not run |
| linux-arm64 executable | Built (aarch64 ELF) | **Not yet run on arm64 hardware** |
| Pairing, management, Controller Link against a real adapter | — | **Not yet validated** |
| Steam Deck: SDL HIDAPI access to the built-in controls | — | **Not yet validated** |
| Game Mode input via Steam's virtual pad (ROG Xbox Ally X, Bazzite) | User, 2026-11-06 | Buttons and sticks work; back buttons absent (no codes on that pad) |
| Game Mode window fills the panel (non-Steam game on the ROG Xbox Ally X, Bazzite, gamescope 3.17) | Agent over SSH, 2026-11-08: launched with `steam://rungameid`, X window geometry and the app's own `[window] full screen` log | 1920x1080 at +0+0 on a 1920x1080 screen (first pass 1200x760, filled by the 500 ms pass); not yet confirmed by eye |
| Game Mode UI scale (same device) | Agent over SSH, 2026-11-08: the app's startup line | `monitor 1920x1080 px, 150x90 mm -> scale 1.7 (1129x635 units)`; window still fills the screen; owner to judge the size |
| Management connect over LE on dual-mode BlueZ | `BluezManagementTransportTests` (fake BlueZ that connects Classic until an LE advertisement is seen, i.e. firmware advertising Flags `0x06`) | Pass; **not yet confirmed against the adapter**. Against Flags `0x02` firmware the LE scan did not help (Ally, 2026-11-08: still `br-connection-refused` right after a fresh advertisement), which is how the advertising flag was found |
| Pair and connect over LE (adapter Flags `0x06`) | Owner and agent over SSH, 2026-11-08 | Pass: paired at once with the window open, management connected over LE (`connected ... contract=4`), Controller Link and companion audio ran |
| Controller and keyboard navigation of the shell and Amiibo | Screenshot harness `--keys` / `--pad` | Pass |
| Controller navigation in Game Mode (Ally, Bazzite) | Agent over SSH, 2026-11-08: app log | Before the fix every press logged `press ignored: window not active` (Avalonia never activates under gamescope). After the fix: works (owner, 2026-11-08) |
| Drop-downs in Game Mode | Agent over SSH, 2026-11-08: app log | Each tap logged a deactivate/activate pair and the list closed at once; popups now drawn inside the window under gamescope; works (owner, 2026-11-08) |
| Steam Deck-format hidraw reader (back buttons, motion) | Unit tests on captured Ally reports; user on the ROG Xbox Ally X, 2026-11-06 | Back buttons and gyro reach the console; gyro reported "very off" (open) |
| Motion frame mapping on a real pad | — | **Not yet validated** (Hypothesis) |
| Keyboard & Mouse and Amiibo pages against a real adapter | — | **Not yet validated**; layout reviewed in the screenshot harness only |
| Touch Gamepad on a touchscreen against a real adapter | — | **Not yet validated**; drawing, menu and an editor drag reviewed in the headless screenshot harness only |
| Companion audio planes over BlueZ (AUDIO-DOWN notify, MIC-UP write-without-response) | `BluezCompanionAudioTests` against the scripted bus | Pass; **not yet validated** against a real adapter |
| SDL3 playback and capture on a real Linux audio stack | `AudioCheck`, WSL Ubuntu 24.04 (WSLg PulseAudio), 2026-11-07 | Pass: 0 losses, underruns and splices on a clean link; drift held at ±500 ppm; microphone 50.00 frames/s. **Not yet run on SteamOS/PipeWire** |

## 6. Open questions for the first hardware pass

- Does SDL's HIDAPI reach the Deck's built-in controls while Steam owns them
  in Game Mode, or does Steam need its controller layout disabled for this app?
- Does SteamOS show a pairing confirmation for the adapter's Just Works
  pairing, and does the app surface it adequately?
- Does a stale bond on BlueZ really surface as an ATT security error or
  `NotAuthorized`, so the shared Repair signature fires?
- Is `WriteValue` (type `command`) low-latency enough for Controller Link, or
  is `AcquireWrite` worth adopting?
- Does the SDL → canonical motion frame hold on a DualSense and on the Deck's
  own IMU?
- Does BlueZ honour the adapter's 15 ms interval request during companion
  audio (`caudio diag`), and does SDL's PipeWire backend keep the playout
  depth and open the microphone from Game Mode?

## 7. Traps

- **Never copy a Tmds `MessageWriter`.** It is a struct whose buffer
  `CreateMessage()` detaches only from the instance it is called on. An early
  version copied it (`var w = writer;`) to pass a `using` variable by ref; the
  original's disposal then recycled the buffer the outgoing message still
  owned, bluetoothd never replied, and every BlueZ call hung forever.
  `DBusBluezBus.Build` is the one place a writer is used.
- **Never set `MatchRule.PathNamespace`** (Tmds.DBus.Protocol 0.21.3). It is
  written as `pathNamespace=` instead of the spec's `path_namespace=`, and the
  bus rejects the whole rule with `MatchRuleInvalid`. This broke pairing on the
  first hardware run (Bazzite, dbus-broker, 2026-11-06). Filter the path in the
  handler instead. `MatchRuleTests` enforces this until the library is fixed.
- **`.gitignore` `*.app` matches `PicoSwitch.Companion.Linux.App/`** under
  `core.ignorecase`. `companions/linux/.gitignore` re-includes it; keep that
  negation above the build-output rules.
- **BlueZ caches a bonded adapter's GATT database.** After flashing a build
  that adds a characteristic (AUDIO-DOWN, MIC-UP), the app may report the
  plane missing until BlueZ re-reads the database (Service Changed, or
  remove and re-pair).
- **Packaging scripts must stay LF** (`.gitattributes`): a CRLF `AppRun` is
  `/bin/sh^M` and the AppImage will not start.
