# Companion shell layout (all platforms)

**Status:** agreed with the project owner on 2026-11-06. This is the one layout
that Windows (WinUI), Linux (Avalonia) and Android (Compose) all implement.
A UI change that departs from it must change this document first, for every
platform at once.

| Platform | Status |
|---|---|
| Linux | In Progress: shell with placeable slide-out navigation, always full screen (filling the panel in Game Mode), size scale, Play home screen, controller navigation, page placement, Adapter and Keyboard & Mouse tabs, short-window layout, Touch Gamepad built (reviewed in the screenshot harness, including 832×468, and keyboard / controller navigation driven there with `--keys` / `--pad`; Game Mode fill measured on a ROG Xbox Ally X, 2026-11-08); Customization drawing, testers, link health and tuning not started |
| Windows | In Progress: shell with placeable slide-out navigation, always full screen, size scale, Play home screen, controller navigation, page placement, Adapter tabs and Keyboard & Mouse tabs, short-window layout built (Play, the navigation and Amiibo keyboard focus looked at on a 2560×1600 screen, 2026-11-08; controller navigation not yet run with a real pad); Customization drawing, testers, link health and tuning not started |
| Android | In Progress: shell with placeable navigation, always full screen, size scale, Play home screen, page placement, Customization, Adapter and Keyboard & Mouse tabs, drawn keyboard, short-window layout built (Navigation and Play reviewed on an AYN Odin 2 Mini at 833×468 dp against a live adapter, 2026-11-08); Customization drawing, testers, link health and tuning not started |

## Navigation — agreed 2026-11-08

Replaces the top bar + dock and the first rail (2026-11-07) after the owner
used both on an AYN Odin 2 Mini: the arrow / handle and pin controls were
unwanted chrome, hiding and showing the rail juddered, and Back on a page
while the navigation was hidden closed the app.

- **Placement is the player's** (Settings → Navigation): Automatic, Left,
  Right, Top, Bottom. Automatic is Left on a window wider than tall and Bottom
  otherwise (measured, never a device name). Left / Right show **icons only**;
  Top / Bottom show **full names** (one-word names below 720 wide, never an
  ellipsis).
- Contents: the connection dot (an **indicator only**: not focusable, not
  clickable), the pages (Play, Keyboard & Mouse, Amiibo, Customization,
  Adapter), Settings. **No hide, pin, arrow or handle control.**
- **Always a slide-out** (owner, 2026-11-08: "the dock should always just be
  a slide out thing", like a console's game mode). It is never docked beside
  the page, so starting or stopping Controller Link never moves it (a docked
  state tied to "touch and not playing" made it jump in on every Stop).
- **Calling it up:** moving focus toward its edge (D-pad left for Left, down
  for Bottom, …) lands on the current page's item; Back brings it in on any
  page. It slides in over the page with a scrim. It leaves by Back, a tap on
  the scrim, focus moving back to the page, or choosing a page.
- **Back:** a page's own Back (an open pane, a dialog, an overlay) first; then
  a **tap** shows or hides the navigation. Back never jumps to Play and never
  leaves the app from inside the shell (owner, 2026-11-08: "Back shouldn't
  launch Play, it should open the dock").
- **Holding Back (0.8 s) while Controller Link streams releases it** (owner,
  2026-11-08), with a haptic tick and a "Controller Link released" notice. Back
  is the one key Controller Link never sends to the console, so the hold
  cannot also press something in the game. Not streaming, a hold is a tap.
  `BackKeyGesture` (unit-tested); reviewed on the Odin with injected key events.
- **Who owns the handheld's controller while streaming:** the console only on
  the bare game screen — Play in front, navigation hidden, no overlay
  (`GamepadNavigation.consoleOwnsPad`, unit-tested). The navigation, any other
  page or an overlay takes the pad, like Steam's overlay, and the console gets
  a neutral report at that moment so nothing stays held in the game. Before
  this, streaming alone decided: a game asked for an amiibo, the library
  opened, and every D-pad press went to the console (owner, 2026-11-08: "I am
  pressing the d-pad and nothing is happening"). ADB `input keyevent`
  injection did not show it — it comes from a virtual device the bridge does
  not claim — so test controller routing with `sendevent` on the gamepad's own
  `/dev/input/event*` node (the Odin's D-pad reports as `ABS_HAT0X/Y` and as
  `BTN_DPAD_*`). On Play while streaming, the D-pad still plays the game: Back
  brings the navigation in, or tap the Amiibo card.
- **Motion without layout:** the page keeps its size; the navigation moves
  only by translation
  (220 ms in, 120 ms out). Animating the rail's width re-measured the page
  every frame — that was the judder. The page itself never moves.
- Page changes cross-fade with a slight rise.

**On Linux and Windows** the same rules, in desktop terms:

- **Escape is Back** (tap shows or hides the navigation; a 0.8 s hold while
  streaming releases Controller Link through the same `BackKeyGesture`). A
  press at the navigation's screen edge calls it up, as a swipe does on
  Android.
- **The controller** is read from the frames the shell already gets (SDL on
  Linux, Windows.Gaming.Input on Windows), because neither UI toolkit delivers
  gamepad keys. `PadOwnership` decides who owns it exactly as on Android;
  while the app owns it, `GamepadUiNavigator` turns frames into menu steps:
  D-pad or stick to move (repeat after 400 ms, then every 110 ms), A to
  confirm on press, B as Back on release, L1 / R1 for the previous / next
  page. On Linux the steps are raised as key events on the focused control,
  so a controller behaves exactly like the keyboard; on Windows they move
  focus with `FocusManager.TryMoveFocus` and confirm through the control's
  automation peer.
- The navigation's items are focusable only while it is shown, and it is
  collapsed once it has left, so focus can never be stranded in it.
- **Play's console buttons** (Home, Capture, C) are held for as long as the
  pointer is down (`SetConsoleButton`), since the console tells a tap from a
  hold; Enter or Space on one taps it. Leaving Play or stopping releases
  anything still held.
- The desktop headset strip keeps a separate microphone toggle: on a desktop
  the microphone is a wish the adapter may refuse.
- **Game Mode (gamescope):** gamescope ignores a full-screen request alone and
  shows the window at its requested size, scaled and letterboxed (owner,
  2026-11-08). Under gamescope (`GAMESCOPE_WAYLAND_DISPLAY` or
  `XDG_CURRENT_DESKTOP=gamescope`) the Linux window also sizes itself to the
  screen's bounds at its origin, when it opens, again 500 ms later, and
  whenever the screens change. Measured on a ROG Xbox Ally X as a non-Steam
  game: the first pass saw 1200×760, the second filled 1920×1080.

| Platform | Navigation status |
|---|---|
| Android | Implementation complete; reviewed on an AYN Odin 2 Mini (Left and Bottom, touch and D-pad) |
| Windows | Implementation complete; keyboard path checked on screen (Left opens, Enter chooses, Amiibo Enter / Escape); controller path not yet run with a real pad |
| Linux | Implementation complete; keyboard and controller paths checked in the screenshot harness (`--keys`, `--pad`); not yet driven on a device |

## Why

The companion exists to turn the device it runs on into the console's
controller. The previous shells were desktop settings apps: a full-height
navigation rail, a page title, and one capped column. That left most of a
handheld screen empty and put the main action (start playing) three levels
down. On SteamOS/Bazzite Game Mode the window is always full screen, so wasted
chrome is wasted on every launch.

## Structure

```text
┌──┬───────────────────────────────────────────┐
│● │                                           │
│▶ │                                           │
│⌨ │               page content                │   navigation at the
│◎ │                                           │   player's chosen edge
│🎨│                                           │   (here: Left, icons)
│⇄ │                                           │
│⚙ │                                           │
└──┴───────────────────────────────────────────┘
```

- **Full screen.** Every platform always runs full screen and has no toggle:
  the companion is a controller, and on a handheld the system bars or window
  frame cost a row of the page. Android hides the system bars with the
  platform's transient behaviour, so an edge swipe still shows them; Linux and
  Windows open their window full screen, and **F11** leaves and re-enters it
  so a desktop session is never trapped. Escape in the Touch Gamepad never
  leaves full screen, and leaving the Touch Gamepad never leaves full screen.
- No page titles in the navigation. **Diagnostics** stays inside Settings.
- When Settings is open, Settings is the selected item; while an overlay
  (Diagnostics) is open, none is.

### Play (home)

The app opens here on every platform.

Laid out as a console home screen (owner direction, 2026-11-07/08: "play
should be like the homescreen", "game oriented in a clean aesthetic"; Android
first). Words only where they carry news; no on/off labels a control already
shows.

- **Left column:** the **headset strip** on top (96 tall), the **controller
  card** below taking the rest.
  - Headset: one tap plugs the headset in (LIVE badge), one tap pulls it out.
    The microphone comes with it whenever the device can record; the only
    microphone control is **mute**, which never changes the accessory the
    console sees (companion audio experiment record, ninth pass). The title
    gives way before the LIVE badge does.
  - Controller card: the controller this device reads, with a Change /
    Refresh menu; a one-phrase state (Ready to play, Connecting…, On the
    console, Not ready) with a quiet **Player N** pill beside it when the
    console has given this controller a player number (its player LEDs, from
    the input sources reply); one line under it only when something blocks
    starting; then, at the bottom edge, one row of equal actions — Home,
    Capture, C while streaming (held, not clicked), otherwise Touch Gamepad and
    Wake (or the adapter's next step) — and **Start playing / Stop** under that
    row, the same size.
- **Right column: the Amiibo**, full height, so its artwork is the picture on
  the home screen: the figure the adapter holds as art and a name (never a
  UID), and one full-width action — Present / Eject when one is loaded,
  Library otherwise. A tap anywhere else opens the library. While the console
  has an NFC scan running (a game is waiting for an amiibo) the tile turns the
  accent colour with a **GAME WANTS AN AMIIBO** badge and its action becomes
  the primary one.
- Every action on Play is the one action button (52 tall, radius 14): no
  small or squeezed buttons inside tiles.
- The adapter's state is the navigation's dot; Play has no adapter card.
- Gamepad: the D-pad moves between tiles and actions; the focused one lifts
  (scale and outline) only while a gamepad or keyboard is driving.
- Fits without scrolling when the columns fit; narrower or shorter windows
  stack and scroll.

### What lives where

Each control sits on the page whose subject it is, so nothing is reachable
only from a place that does not explain it.

| Page | Holds |
|---|---|
| Play | Start/Stop, the controller at a glance, console buttons while streaming, Wake console, Touch Gamepad, the headset, the adapter's Amiibo |
| Keyboard & Mouse | Three tabs under one header and one toolbar (below) |
| Amiibo | The library. Under a controller the focused card **is** the highlighted one (one outline, no second focus overlay), A opens its details and focus moves into them, and the carousel keeps the focused card centred; by touch, tap highlights and double tap opens (owner, 2026-11-08: focus "messed up" in every view) |
| Customization | How the emulated controller looks: its colours. The clickable controller drawing replaces the colour rows when it exists. |
| Adapter | The adapter as a device. A header (name, firmware, Connect/Disconnect; no state words, because the navigation's dot already shows them, and only a message that explains a failed connect) over three tabs: **Console** (the controller on the console, the emulated controller, the input source; on Android also the Amiibo the adapter is serving, because the library's inspector there cannot show a tag that is not in the library), **Controllers** (pair a controller; connected, paired and recent controllers), **Adapters** (add an adapter; the adapters this device remembers, with rename, repair, remove; with none remembered there is only the add row, never a separate "none yet" line). Opens on Adapters when none is remembered yet. On a short window the tabs join the header row and the adapter's name is dropped. |
| Settings | This app's own preferences: theme, face buttons, the door to Diagnostics, and (Android and Linux) the version. One column of rows on every platform; Android also has the accent colour. |

### Keyboard & Mouse

- Header: **Keyboard & mouse** with one on/off switch, and one line under it
  (the effective mode, or what to do: pair a keyboard or mouse; "Off ·
  keyboards and mice are ignored"). The switch replaced a four-way mode menu
  (Automatic / Controller / Keyboard / Keyboard and mouse; owner, 2026-11-08):
  on is Automatic, where the adapter picks the layout from the devices it
  sees; off is Controller. The firmware's explicit Keyboard and Keyboard and
  mouse overrides remain on the wire, unused by the companion.
- Mapping (without More keys): the card **hugs** the drawn keyboard and sits
  centred in the page, so there is no empty card around the keys.
- One toolbar row: tabs on the left; on the right the **open profile** (which
  profile, which layout, saved or not, Save, Discard). The open profile shows
  on every tab because Mapping and Profiles both act on it, and an unsaved
  edit must never be hidden by a tab switch.
- Tabs:
  - **Mapping**: the keyboard drawn in the system's layout. ISO and
    international keys sit behind a **More keys** toggle because few keyboards
    have them. Reset mapping.
  - **Mouse**: mouse-button mapping (in the keyboard-and-mouse layout) and the
    adapter's mouse tuning. Tuning belongs to the adapter, not to a profile.
  - **Profiles**: the library (New, Duplicate, Rename, Delete), the profiles on
    the adapter, profile switch keys, and Reset everything.
- Profiles is two columns on a wide window (the library and the adapter's
  positions on the left; Reset everything and the switch keys on the right),
  with the counters below, and one column on a narrow window.
- On the drawn keyboard a long name on a one-unit key (Home, Pause, Down)
  shrinks to fit the cap; it is never cut to an ellipsis.
- The drawn keyboard sizes its key unit to fit both the width and the height
  it is given, so the whole keyboard is on screen without scrolling; it scrolls
  only below a minimum key size. **More keys** switches the card to the ISO and
  Japanese keys (and any bound key the picture does not draw) rather than
  stacking them under the keyboard, and counts the bound ones it hides.
- The open profile's **Save** button says whether there is anything to save
  ("Saved", disabled; "Save"; "Save as new" for the built-in Default) instead
  of a status beside it. The longer status is its tooltip, and the states that
  need explaining keep their notice.
- The **Mouse** tab is the mouse buttons (as tiles like the keys) beside the
  tuning, a third and two thirds, one column on a narrow window. The tuning
  is two columns when the card has the width (the stick's feel; then
  anti-deadzone and the two invert toggles, as chips on one row).
- **On a short window** (under 560 of window height, in dp on Android and
  logical pixels on the desktops, the same number everywhere) the header and
  toolbar give the keyboard their height: the tabs move up into the header row
  in place of the page title, the open profile takes the toolbar's left and the
  Mapping commands (More keys, Reset mapping) its right, the header text keeps
  to two trimmed lines, and the vertical padding and section gaps tighten. The
  keyboard then fits the height left as well as the width. Profiles still
  scrolls on such a window.

### Other pages

- Headers are compact, without a large title above the first card.
- Settings-like pages use the platform's own settings-row control (on Windows,
  the Community Toolkit `SettingsCard`): a name and short description on the
  left, the control on the right, in one centred column of capped width.

### Fitting the screen

- Every page fits the window it is in without scrolling, at ordinary window
  sizes and full screen. Pages that would not fit are split into tabs
  (Keyboard & Mouse, Adapter) rather than made to scroll.
- The exception is **Amiibo**: the library grid scrolls, because it is a
  collection of any length. Its header and filters do not.
- Each page keeps a scroll container as a last resort for windows too small
  for any layout. Reaching it on an ordinary window is a layout bug.

### Sizes

One scale on every platform, so a row of controls from two pages lines up.
Values are in the platform's device-independent unit (epx on Windows, px on
Avalonia, dp on Android).

| Token | Value | Used for |
|---|---|---|
| Control height | 36 | every button, dropdown, toggle and text box |
| Large control height | 44 | Play's secondary actions, status-card actions |
| Action height | 52 (radius 14) | every action button on Play: Start / Stop, Home, Capture, C, quick actions, Present / Eject, Library — one size, told apart by colour only (owner: Start / Stop was "too big"; 2026-11-07) |
| Navigation | side 80 wide; top / bottom 64 tall; items 52 | the navigation (see Navigation) |
| Command min width | 96 | a labelled command button |
| Selector min width | 220 | a dropdown in a toolbar |
| Card selector width | 280 | a dropdown that is a settings row's control (fixed, so the right edges line up) |
| Page max width | 1000 | settings-like pages |
| Wide page max width | 1240 | Play, Keyboard & Mouse |
| Page padding | 32, 12, 32, 24 | left, top, right, bottom |

- An icon-only button is square at the control height.
- On Android, Material keeps its 48 dp minimum touch target around every
  control whatever its drawn height, so the scale sets what is drawn, not what
  is touchable.
- A filter's "everything" row names the filter ("All game series", "All
  types"), never a bare "All".
- Explicit sizes outside the scale remain only where the size is the content:
  the drawn keyboard's keys, colour swatches, the navigation.

## Rules that do not change

- No layout rule names a device, resolution or aspect ratio. Size is decided by
  measuring the space offered.
- The Touch Gamepad remains a mode that replaces the shell, not a page in it.
