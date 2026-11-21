# Keyboard & Mouse Link

Play the Switch 2 with the keyboard and mouse of the computer that runs a
companion, forwarded through the adapter.

- **Status:** Design. Nothing here is implemented. Implementation needs the
  maintainer's go-ahead (see [`PLAN.md`](../../PLAN.md)).
- **Drafted:** 2026-11-11.
- **Reads with:** [`../bluetooth/keyboard-mouse-input.md`](../bluetooth/keyboard-mouse-input.md)
  (the adapter's KB/M model), [`kbm-profile-system-hld.md`](kbm-profile-system-hld.md)
  (profiles), [`../bridge/PROTOCOL.md`](../bridge/PROTOCOL.md) (Controller Link),
  `firmware/src/management/ns2_companion_link.h` (the data plane this extends).

Claims about platform behavior are marked **Confirmed** (read in this
repository or in the platform's own documentation), **Strong Evidence**,
**Hypothesis**, or **Unknown**, as the engineering principles require. Every
Hypothesis and Unknown has a spike or a hardware gate in §14.

---

## 1. Summary

The adapter already turns a Bluetooth keyboard and mouse into a controller: it
holds the mapping, the profiles, the mouse-to-stick translator and the native
Joy-Con 2 pointer path, and all three companions already edit those profiles.
The companions already forward a PC's game controller over Controller Link.

KB/M Link joins the two. The companion captures the computer's keyboard and
mouse, sends **raw key and mouse state** on the existing Controller Link data
plane, and the adapter feeds it into the **existing** KB/M model as if a
keyboard and mouse were paired to it. No mapping logic is duplicated on the PC.

The part that needs care is the computer, not the adapter. A keyboard and mouse
are how the PC itself is operated, so forwarding them must never also operate
the PC, and the PC must never be left with its input swallowed. The design
answer is a **dedicated play surface with an explicit engaged state**:

- KB/M play is a mode the user enters on purpose, on its own full-screen
  surface, like Touch Gamepad.
- The surface draws the keyboard and the mouse, lights what is pressed, and
  labels each key with the Switch control it is bound to.
- Input reaches the Switch **only while the surface is engaged**. Engaged means
  the keyboard and mouse are captured: nothing reaches other applications or
  the desktop.
- One chord releases capture at any time. Losing focus, losing the link, or the
  companion dying also releases it, and the adapter neutralizes on its own.

## 2. Goals and non-goals

Goals:

1. A PC keyboard and mouse drive the Switch through any personality, using the
   adapter's existing layouts, profiles and mouse tuning unchanged.
2. While engaged, no keystroke, click or pointer movement acts on the computer.
3. The user can always get their computer back, with one chord, and without the
   companion's cooperation if it hangs.
4. No stuck input on the console and no stuck modifier on the computer, on any
   exit path.
5. One wire format and one behavior on Windows, Linux, the web portal and
   Android, pinned by shared fixtures.
6. Native Joy-Con 2 mouse mode works with a PC mouse.

Non-goals for this feature:

- Macros, turbo, key sequences. The adapter's model is one source to one
  destination and stays that way.
- Typing text into the console's on-screen keyboard.
- Mapping on the PC, or any second mapping format.
- Background capture: forwarding input while another application has focus.
- A controller and KB/M from the same computer at once (§13, F3).
- Presenting a USB mouse or keyboard to the console (§13, F2).

## 3. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | The adapter maps. The companion sends HID usages, mouse buttons and motion counters. | One mapping system. Profiles, profile-switch keys, the velocity translator and the native pointer path are already built, host-tested and hardware-validated on the adapter. Mapping on the PC would need the translator re-implemented in C#, Kotlin and JavaScript and could never reach the native pointer. |
| D2 | A new opcode on the existing CL-IN characteristic. No new characteristic, service, bond or command verb. | Inherits the trust gate, the session, the stale watchdog and the latest-state writer. Old firmware counts an unknown opcode and ignores it. |
| D3 | Frames carry **complete state**: the full held-key set and **cumulative** mouse counters. | The writer coalesces frames (latest state wins). Complete state survives coalescing, duplication and reordering. Per-frame deltas would not: a replaced frame would lose movement. |
| D4 | The companion declares which roles it supplies; the adapter admits it through the same role table as Bluetooth peers. | Mode override, the one-keyboard-one-mouse rule, composite ownership and the Joy-Con 2 exception all keep working without a parallel rule set. |
| D5 | Capture is foreground-only and exists only while engaged. | The safest possible failure: if the surface is not in front, nothing is captured. No global hook outlives the engaged state. |
| D6 | Engaging is a deliberate act, never automatic. | The user asked for this. Starting the app, selecting the source or reconnecting must not take the keyboard. |
| D7 | Keys are identified by physical position (scan code), never by character. | The adapter's identity is the HID usage. A French AZERTY user pressing the key in the W position must get the W binding, as a Bluetooth keyboard would. |
| D8 | Keystrokes are never logged, stored or shown outside the live surface. | The frames are the user's typing. Diagnostics count frames; they do not record keys. |

### Alternatives rejected

- **Map on the PC and send an ordinary controller frame.** No firmware change,
  which is attractive. Rejected for D1's reasons, and because the realized
  profile lives on the adapter: the PC would have to mirror it and stay in sync.
- **A second GATT characteristic for KB/M.** Buys nothing over an opcode and
  costs an ATT database change, which invalidates every host's GATT cache.
- **Extending the Classic bridge HID descriptor with keyboard and mouse
  collections (Android).** Changes the 161-byte contract descriptor, bumps the
  bridge contract, and makes the bridge a composite gamepad-plus-keyboard device
  — the exact shape `contracts/composite_gamepad_keyboard_hid.h` exists to keep
  off the keyboard driver.
- **Global hooks so play continues with another window focused.** That is a
  keylogger's architecture, it trips anti-malware heuristics, and it makes
  "the PC is doing random things" the default failure instead of an impossible
  one.

## 4. Architecture

```text
 COMPUTER                                             ADAPTER
 --------                                             -------
 OS keyboard + mouse
     |  platform capture backend (only while engaged)
     v
 KbmCaptureController     Off -> Armed -> Engaged
     |  usage down/up, mouse buttons, dx/dy/wheel
     v
 KbmLinkState             held set, press latch,
     |                    cumulative counters
     v
 KbmLinkDataPlane.Encode  41-byte frame, opcode 0x02
     |
     v
 ControllerLinkWriter  --- CL-IN (write without response) --->  clink_on_frame()
   latest-state mailbox                                          |  opcode 0x02
   one write in flight                                           v
                                                        ns2_companion_kbm_decode()
                                                                 |
                                                                 v
                                                  ns2_kbm_runtime_submit_companion()
                                                     roles (companion peer key)
                                                     ns2_kbm_state_t
                                                     ns2_kbm_resolve()  <- active profile
                                                                 |
                                         native pointer (Joy-Con 2) or right stick
                                                                 v
                                                          console report
```

Everything below `ns2_kbm_runtime_submit_companion()` exists today.

## 5. Wire contract

### 5.1 Frame

A new opcode beside `NS2_COMPANION_LINK_OP_STATE` (0x01). The header is the
existing one.

```text
NS2_COMPANION_LINK_OP_KBM = 0x02      companion -> adapter

[0]      version            NS2_COMPANION_LINK_VERSION (1)
[1]      opcode             0x02
[2..3]   sequence           uint16 LE, shared with opcode 0x01 in a session
[4]      flags              bit0 ENGAGED   input is captured and live
                            bit1 KEYBOARD  the companion supplies the keyboard role
                            bit2 MOUSE     the companion supplies the mouse role
                            bits3..7       must be 0; a frame with any set is rejected
[5]      mouse buttons      bit n-1 = HID button n, n = 1..5; bits5..7 must be 0
[6..7]   motion X counter   int16 LE, cumulative, free to wrap
[8..9]   motion Y counter   int16 LE, cumulative, free to wrap
[10]     wheel counter      int8, cumulative, free to wrap
[11]     reserved           0
[12..40] key bitmap         29 bytes; bit (u % 8) of byte (u / 8) = usage u held,
                            Usage Page 0x07, u = 0x00..0xE7
```

Frame length 41 bytes. Minimum ATT MTU 44 (41 + 3), measured at `clink start`
exactly as the existing 33 is. The bitmap is the adapter's own
`ns2_kbm_state_t.keys` truncated at `NS2_KBM_KEY_USAGE_MAX`; the decoder
zero-extends it to `NS2_KBM_KEY_BITMAP_BYTES`.

`NS2_COMPANION_LINK_VERSION` does not change: the layout of opcode 0x01 is
untouched, and support for 0x02 is advertised as a capability (§5.4), which is
the mechanism an older peer can actually read.

### 5.2 Semantics

- **Complete state.** Every frame says everything. The adapter never needs the
  previous frame to interpret the current one, except the motion baseline.
- **Motion counters.** The companion adds every mouse count to a running
  counter and sends the counter. The adapter subtracts the last counter it
  applied, as a signed 16-bit difference, and feeds the result to
  `ns2_kbm_state_mouse_report()` as one report's delta. A coalesced, duplicated
  or dropped frame therefore loses nothing and adds nothing.
- **Baseline.** The adapter takes the counters of the first ENGAGED frame after
  a session is armed, or after a frame with ENGAGED clear, as the baseline: that
  frame's delta is zero. Engaging can never produce a jump.
- **Per-frame bound.** The companion advances a counter by at most ±16383 per
  frame it hands to the writer and discards the excess. That keeps the
  difference unambiguous even if one frame is replaced in the mailbox, and the
  discarded movement is far beyond full stick deflection (8.53 counts/ms at
  default sensitivity) and beyond what a native pointer report carries.
- **ENGAGED clear** means "the companion is present but not capturing". The
  adapter clears the keyboard and mouse state it holds for the companion and
  publishes the result. Bitmap, buttons and counters in such a frame are
  ignored.
- **Role bits.** KEYBOARD and MOUSE say which roles the companion offers for
  this session. They may differ between sessions and are not expected to change
  inside one; a change is treated as release and re-admission.
- **Invalid usages.** Bits for usages 0x00..0x03 are ignored by the decoder
  (never physical keys, as in the Bluetooth path).
- **Liveness.** A KB/M frame feeds the same 300 ms stale watchdog as a
  controller frame. The companion resends unchanged state every 100 ms, as it
  does today.

### 5.3 Why the press latch exists

The writer sends the newest state when the link can take it. A key pressed and
released between two sends would vanish. `KbmLinkState` therefore **latches
presses**: a key that went down since the last frame was taken is reported down
in the next frame even if it is already up, and reported up in the frame after.
The same applies to mouse buttons. Releases are not latched (a release that is
overtaken by a re-press is just a held key).

The minimum hold the console sees is one frame interval. **Hypothesis:** that is
long enough for every game to register a tap, since it spans at least one
console poll; gate H1 checks it with a scripted 5 ms tap.

### 5.4 Control plane

No new command verb, so `config_wireless_command_allowed()` does not change.

- `clink status` gains an object:
  `"kbm":{"supported":true,"frame_bytes":41,"min_att_mtu":44,"mtu_ok":true,`
  `"engaged":false,"roles":"none|kb|mouse|kbm","admit":"ok|mode|duplicate|none",`
  `"frames":0,"rejected":0,"baseline_resets":0}`
- `info` gains `"kbm_link":1`. A companion offers KB/M Link only when it is
  present. Absent means older firmware: the companion says "update the adapter".
- `kbm status` gains `"keyboard_src"` and `"mouse_src"`: `"bt"`, `"companion"`
  or `"none"`, so a role held by the companion is not shown as a Bluetooth peer.
- `input sources` lists the companion KB/M source like any other, named
  "Keyboard & Mouse (companion)".

`contracts/management/protocol-v1.json` gains the new reply shapes.

### 5.5 Fixtures

| Fixture | Guards | Consumers |
|---|---|---|
| `contracts/kbm_link_goldens.csv` | state -> the 41 frame bytes, including latch, counter wrap, the per-frame bound and ENGAGED clear | C decoder test, C#, Kotlin, JavaScript |
| `contracts/kbm_link_keymap.csv` | physical key -> HID usage for each platform identifier: Windows scan code (with E0/E1), Linux evdev code, `KeyboardEvent.code` | C#, Kotlin, JavaScript |
| `contracts/companion_link.h` (or constants static-asserted against `ns2_companion_link.h`) | opcode, frame length, flag bits | C, parity script |

The keymap fixture matters more than it looks. The three platform tables are
each about 110 rows, they are transcribed from different sources, and one wrong
row is a key that silently does the wrong thing on one platform only.

## 6. Adapter design

### 6.1 Decode (pure, host-tested)

`firmware/src/management/ns2_companion_link.{h,c}` gains:

```c
#define NS2_COMPANION_LINK_OP_KBM 0x02u
#define NS2_COMPANION_KBM_PAYLOAD_BYTES 37u
#define NS2_COMPANION_KBM_FRAME_BYTES (NS2_COMPANION_LINK_HEADER_BYTES + 37u)

typedef struct {
    bool engaged, keyboard, mouse;
    uint8_t buttons;                       // HID buttons 1..5
    int16_t counter_x, counter_y;
    int8_t counter_wheel;
    uint8_t keys[NS2_KBM_KEY_BITMAP_BYTES]; // zero-extended, 0x00..0x03 cleared
} ns2_companion_kbm_frame_t;

// False for a short payload or any reserved bit set. Never partially filled.
bool ns2_companion_kbm_decode(const uint8_t *payload, uint16_t len,
                              ns2_companion_kbm_frame_t *out);

// Motion baseline: turns counters into one report's deltas.
typedef struct { bool valid; int16_t x, y; int8_t wheel; } ns2_companion_kbm_baseline_t;
void ns2_companion_kbm_delta(ns2_companion_kbm_baseline_t *baseline,
                             const ns2_companion_kbm_frame_t *frame,
                             int16_t *dx, int16_t *dy, int8_t *dwheel);
```

`ns2_companion_link_parse()` today returns `NS2_COMPANION_FRAME_OPCODE` for
anything but 0x01. It gains an out-parameter for the opcode and accepts 0x02
with its own length check, so sequence handling stays in one place.

### 6.2 Runtime ingress

`ns2_kbm_runtime` is bound to `bthid` today: `peer_key_for_connection()` reads
the Bluetooth device table. The companion is not a Bluetooth HID device and
must not be given a fake slot (the rule in `ns2_companion_link.h`). The runtime
gains a second way to name a peer:

```c
// The companion session as a KB/M peer. `generation` is the clink session
// generation, so a stale session can never release its successor's roles.
bool ns2_kbm_runtime_companion_submit(uint32_t generation,
                                      bool keyboard, bool mouse, bool engaged,
                                      const uint8_t *usage_bitmap,
                                      uint16_t buttons,
                                      int16_t dx, int16_t dy, int8_t dwheel);
void ns2_kbm_runtime_companion_release(uint32_t generation);
ns2_kbm_admit_t ns2_kbm_runtime_companion_admit_result(void);
```

- **Peer key:** `conn_index = 0xFE` (the existing `CL_SOURCE_ADDR`),
  `addr_valid = 0`, `connection_generation = generation`. `peer_key_for_connection`
  is split so the Bluetooth lookup and the synthetic key share everything after
  the key is built.
- **Primary role:** KEYBOARD and MOUSE both set -> `NS2_KBM_PRIMARY_COMBO`;
  one set -> that role. This is a positive declaration by the peer, which is
  the standard the model already sets for COMBO. No capability inference.
- **Admission:** `ns2_kbm_roles_admit()` with the ordinary `role_policy()`.
  Consequences that fall out, none of them special-cased:
  - mode override `controller` -> `REJECT_MODE`; the companion offers to change
    the mode;
  - a Bluetooth keyboard already holds the keyboard role -> the companion gets
    the mouse role only, or `REJECT_DUPLICATE` if both are held;
  - on a Joy-Con 2 personality under `auto`, the mouse-alone exception does not
    block the companion, because it supplies the keyboard role in the same
    admission and the composite forms.
- **Arbiter:** events carry `transport = INPUT_TRANSPORT_COMPANION`,
  `dev_addr = 0xFE`, `instance = 1` (the controller source is instance 0), and
  the roles' `group_id`. Class `NS2_INPUT_SOURCE_CLASS_BRIDGE`, as the
  controller link is: a directly paired controller still wins automatic
  selection, and an explicit selection is never overridden.
- **State:** the existing `ns2_kbm_state_set_keys()` and
  `ns2_kbm_state_mouse_report()`. Profile-switch keys, the switch mask, the
  neutral boundary on a mapping change and `publish_locked()` are reused as
  they stand, by routing through the same code as `submit_keyboard` /
  `submit_mouse` after admission.
- **Identity:** the console slot is labelled "Keyboard & Mouse (companion)".

### 6.3 Session, release and watchdog

| Event | Adapter action |
|---|---|
| First 0x02 frame of a session | admit roles; set baseline; nothing published until ENGAGED |
| ENGAGED set | apply keys, buttons, deltas; publish |
| ENGAGED cleared | clear companion keyboard and mouse state; publish neutral for those roles; drop baseline |
| No data frame for 300 ms | existing stale path, extended: also clear the KB/M roles' state |
| `clink stop`, management link lost, bond removed | `ns2_kbm_runtime_companion_release()`: roles freed, state cleared, arbiter told with the session generation |
| Controller frame (0x01) in the same session | independent source, instance 0. The companion does not send both kinds while engaged (§7.6) |

Releasing through the generation means a companion that is killed and replaced
cannot have its successor's roles released by a late teardown — the same
protection Bluetooth peers have.

### 6.4 Cost

Estimated, to be measured at implementation: about 1.5 KB of flash, under
100 bytes of RAM (one baseline, one decoded frame on the stack, a few
counters). No new timer: the 3 ms core-1 tick already services the translator.
Available on both boards; nothing here depends on audio or the RP2350.

### 6.5 Timing

The translator was tuned for Bluetooth mice reporting every 8 to 15 ms:
`NS2_KBM_MOUSE_VELOCITY_MS` is 40 and `NS2_KBM_MOUSE_IDLE_MS` is 64. A companion
sends at most one motion frame per connection event (7.5 to 15 ms once the
link is in its short-interval band), which is the same cadence. **Hypothesis:**
no retuning is needed; gate H2 compares the feel against a Bluetooth mouse on
the same profile.

## 7. Capture model

This section is the answer to "playing the game must not do random things on
the computer".

### 7.1 States

```text
        open the surface            click "Play", or the engage chord
  Off ---------------------> Armed ---------------------------------> Engaged
   ^                          ^  |                                      |
   |      close the surface   |  |        release chord, focus lost,    |
   +--------------------------+  +<------ link lost, surface closed ----+
```

| State | PC input goes to | Sent to the adapter | Surface shows |
|---|---|---|---|
| Off | the PC | nothing | not open |
| Armed | the PC, and the surface as a **tester**: keys light up, nothing is forwarded | frames with ENGAGED clear (keepalive only) | "Click Play or press <chord> to start. Your keyboard and mouse still control this computer." |
| Engaged | **only** the adapter | live frames | "Playing. Press <chord> to release." always visible |

Armed is deliberately useful on its own. It is the KB/M tester that
[`PLAN.md`](../../PLAN.md) lists as not started: the user can see which key is
which, check the profile's labels and confirm the mouse is seen, with nothing
captured and nothing sent.

### 7.2 Engage rules

1. Engaging needs a user gesture on the surface: the Play button, a click on
   the keyboard drawing, or the engage chord while the surface has focus.
2. Engaging is refused, with the reason shown, unless: the link is trusted,
   `info.kbm_link` is present, the MTU is sufficient, `clink start` succeeded,
   and admission returned a role.
3. **Keys already down are ignored until released.** The click or chord that
   engages is in progress when capture starts. A key that was down at that
   moment is neither forwarded nor swallowed until its next fresh press. This
   prevents both failure directions: the Switch does not receive a phantom
   press, and the PC is never left believing a key is held because its key-up
   was swallowed.
4. The motion baseline is taken at the first engaged frame (§5.2).

### 7.3 Release rules

Any one of these releases, and all of them run the same path:

| Trigger | Notes |
|---|---|
| Release chord | Handled in the companion, before forwarding. Default in §7.5 |
| Surface loses focus, is minimized, or is closed | Includes anything the OS does over our head: Ctrl+Alt+Del, Win+L, a UAC prompt, a notification stealing focus, a remote-desktop switch |
| Session lock, sleep, display off, user switch | From the OS session notifications |
| Link not trusted, `clink` stopped, write failures past a threshold, adapter gone | **The PC gets its input back the moment the link cannot carry it.** Capturing input that goes nowhere is the worst state this feature can be in |
| Admission lost (mode changed to Controller from another client) | Read from `clink status` on the existing heartbeat |
| Watchdog: no input event and no successful write for a bounded time while engaged | Belt and braces against a hung publisher |

The release path, in order:

1. Stop forwarding. Build and hand over one frame with ENGAGED clear.
2. Remove OS capture: unhook, unclip, show the cursor, leave pointer lock.
3. Stop swallowing. Keys still physically down had their key-down swallowed, so
   the OS sees only a key-up for them. An orphan key-up is harmless.
4. Return to Armed. Never to Off: the surface stays, so the user sees what
   happened and can re-engage with one click.

Step 1 happens before step 2 so that the console is neutral before the keys the
user is still holding start reaching the desktop.

If the companion process dies, steps 2 and 3 are done by the operating system
(hooks, clips and locks belong to the process), and the adapter's 300 ms
watchdog does step 1. **Hypothesis** for the cursor clip on Windows; gate H3
kills the process while engaged and checks the pointer.

### 7.4 What "captured" means per platform

| | Keys reach other apps | System shortcuts | Pointer | Cannot be captured (and so releases) |
|---|---|---|---|---|
| Windows | No: the surface is the foreground window | Swallowed by a low-level keyboard hook that exists only while engaged | Hidden and clipped to the surface; movement read as raw input | Ctrl+Alt+Del, Win+L, the power button, UAC secure desktop |
| Linux (X11) | No: keyboard grab | Swallowed by the grab | Grabbed; relative motion | VT switch (Ctrl+Alt+Fn), compositor-reserved keys |
| Linux (Wayland) | No: focused surface | Inhibited if the compositor grants it; some compositors keep an escape combo | Locked through pointer constraints; relative motion | Whatever the compositor keeps |
| Web | No: fullscreen, focused page | Keyboard Lock captures most, including Alt+Tab and the Windows key, in fullscreen on Chromium | Pointer Lock | Ctrl+Alt+Del, Win+L, and Esc held for two seconds (the browser's own exit) |
| Android | No: focused activity | Not interceptable | Pointer capture | Home, Recents, Meta shortcuts, Alt+Tab |

A shortcut that cannot be captured takes focus away, which is a release
trigger. So the rule is uniform even where the capability is not: **either the
companion has the input, or the user has their computer.**

A setting, "Capture system shortcuts", defaults on. Off means the Windows key
and Alt+Tab reach the OS, which then takes focus and releases. Some users will
want Alt+Tab to stay theirs.

### 7.5 The release chord

Requirements: reachable on a 60 % keyboard, one-handed, not something a game
binding wants, and never forwarded as a chord.

- **Default: Ctrl + Alt + Shift + Z.** It is the chord Moonlight uses to toggle
  capture, so the audience most likely to use this already has it in their
  hands. Configurable.
- The chord also engages from Armed.
- The chord is recognized in the companion. The modifier keys are forwarded as
  they go down (they are ordinary bindable keys: Left Ctrl is L3 by default),
  and when the chord completes, the frame that goes out is the ENGAGED-clear
  frame. The final key of the chord is never forwarded.
- The surface shows the chord at all times while engaged. A hint the user
  cannot see is not a way out.
- The web portal adds the browser's own exit (Esc held) and cannot remove it.

### 7.6 One source at a time

The Play page's source list gains "This computer's keyboard & mouse". Selecting
it does not capture anything; it makes the KB/M surface what Play opens. While
that source is selected the companion sends no controller frames, and switching
source runs the existing neutralize-on-source-change rule. The adapter would
tolerate both opcodes in one session (they are separate arbiter sources); the
companion simply does not do it.

### 7.7 Privacy

- Raw keys exist in three places only: the capture backend, `KbmLinkState`, and
  the encrypted link. They are sent only while engaged.
- The diagnostic log, the diagnostics export and `clink status` carry counts
  and rates. Never usages, never text.
- The low-level hook and any grab exist only between engage and release. The
  companion does not observe the keyboard while it is in the background.
- The adapter keeps the held set in RAM, as it does for a Bluetooth keyboard,
  and persists none of it.
- Engaged is visually unmistakable (§8), so nobody types a password into a
  surface that is forwarding it.

## 8. The play surface

A full-window page with the same standing as Touch Gamepad.

```text
+--------------------------------------------------------------------------------+
|  <- Back    Keyboard & Mouse      Profile: Default (KB+M)      * Playing       |
|             Pro Controller 2  -  mouse -> right stick          Ctrl+Alt+Shift+Z|
+--------------------------------------------------------------------------------+
|                                                                                |
|  [Esc ][F1][F2][F3][F4] [F5][F6][F7][F8] [F9][F10][F11][F12]      .---------.  |
|   HOME                                              CAPT          | L  | R  |  |
|  [ ` ][1 ][2 ][3 ][4 ][5 ][6 ][7 ][8 ][9 ][0 ][- ][= ][ Bksp ]    | ZR | ZL |  |
|        ZL      ZR                                        -        |----+----|  |
|  [Tab ][Q ][W#][E ][R ][T ][Y ][U ][I ][O ][P ][[ ][] ][ \  ]     |   (M)   |  |
|         L   ^   X   R                                             |    R3   |  |
|  [Caps ][A#][S ][D ][F ][G ][H ][J ][K ][L ][; ][' ][ Enter ]     |         |  |
|          <   v   >   A                                  +         |  ( o-> )|  |
|  [Shift  ][Z ][X ][C ][V ][B ][N ][M ][, ][. ][/ ][  Shift  ]     '---------'  |
|   Y              C                                                 stick 62 %  |
|  [Ctrl][Win][Alt][          Space           ][Alt][Fn][Ctrl]                   |
|   L3         R3              B                                                 |
|                                                                                |
+--------------------------------------------------------------------------------+
|  Link 9 ms  -  110 frames/s  -  0 dropped     [ Lights out ]  [ Release ]      |
+--------------------------------------------------------------------------------+
```

- **Keyboard.** Drawn from the geometry the profile editors already use
  (`KeyboardLayout.cs`, `KeyboardGeometry.kt`; the portal gets the same table).
  A held key is lit. Under each key is the Switch control it is bound to in the
  **realized** profile, read with the existing `kbm` profile commands. Unbound
  keys are dim. A profile-switch key shows the profile it selects.
- **Mouse.** A drawn mouse with five buttons that light, a wheel tick, and a
  motion indicator: a dot displaced by current velocity inside a ring. When the
  personality has no native pointer, the ring is the stick's range and the
  readout is the deflection the adapter will produce ("stick 62 %"); on a
  Joy-Con 2 personality it reads "pointer".
- **Header.** Profile and layout in force, personality, where mouse movement is
  going, and the state with the release chord.
- **State colour.** Armed is neutral. Engaged recolours the frame and header.
  The difference has to be readable from across a room, since the user is
  looking at a television.
- **Lights out.** Optional: while engaged, fade the surface to near-black with
  only the release hint. The PC's monitor is often next to the game.
- **Footer.** Link health from the existing Controller Link metrics.
- **Controller navigation.** The surface is reachable and operable with a pad
  in Armed, as every page is; engaged, it is keyboard-and-mouse by definition.

What the surface deliberately does not do: edit bindings. The editor exists; the
surface links to it. An editor on a surface that is swallowing the keyboard is
a conflict of interest.

The deflection readout is computed on the PC from the profile's mouse settings
for display only. It is not sent. The adapter remains the only thing that
decides the stick.

## 9. Shared companion core (C#)

In the portable projects under `companions/windows/src/`, shared by Windows and
Linux, with no OS types (enforced by `LayeringGuardTests`).

| Type | Project | Role |
|---|---|---|
| `KbmLinkDataPlane` | Bridge.Core/Protocol | Frame constants and `Encode(state, sequence, destination)`. Mirrors §5.1 |
| `KbmLinkState` | Bridge.Core/Core | Held-usage set, mouse buttons, press latch, cumulative counters with the per-frame bound, `TakeFrame()`. Pure, deterministic, no clock |
| `HidUsage` / `KeyMap` | Bridge.Core/Core | Usage constants; `FromWindowsScanCode`, `FromEvdev`. Generated from or tested against `contracts/kbm_link_keymap.csv` |
| `KbmCaptureController` | Companion.Services | The Off/Armed/Engaged machine: engage preconditions, keys-down-at-engage masking, chord recognition, every release trigger, release ordering |
| `IKbmCaptureBackend` | Companion.Platform/Input | The OS seam (below) |
| `KbmLinkService` | Companion.Services | Owns a session: `clink start`, the publisher loop, keepalive, heartbeat on `clink status`, metrics. Shares `ControllerLinkWriter` and the data plane with `ControllerLinkService`; the two are mutually exclusive on one data plane |
| `KbmSurfaceModel` | Companion.Services/Presentation | What the surface draws: lit keys, per-key destination labels, state, deflection readout |

```csharp
public interface IKbmCaptureBackend : IAsyncDisposable
{
    KbmCaptureCapabilities Capabilities { get; }      // system keys, raw motion, relative pointer
    event Action<KbmKeyEvent> Key;                    // (usage, down) - physical, layout-independent
    event Action<KbmMouseEvent> Mouse;                // (dx, dy, wheel, buttons)
    event Action<KbmCaptureLost> Lost;                // focus, session lock, OS refusal
    void Observe(object surfaceHandle);               // Armed: events only, nothing swallowed
    bool TryCapture(KbmCaptureOptions options);       // Engaged: swallow + confine
    void Release();                                   // idempotent; must not throw
}
```

Send policy, reusing the constants `ControllerLinkService` already justifies:
a key or button edge may go out 2 ms after the previous send; a frame whose
only change is motion is limited to one per 8 ms; unchanged state is resent
every 100 ms. Motion needs no epsilon, because cumulative counters make an
unsent frame free.

## 10. Platform backends

### 10.1 Windows

**Confirmed** from Microsoft's documentation unless marked.

- **Data: Raw Input.** Register the keyboard (usage page 1, usage 6) and mouse
  (usage page 1, usage 2) against the surface's window, without
  `RIDEV_INPUTSINK`, so input arrives only while the window is foreground.
  `RAWKEYBOARD.MakeCode` plus the E0/E1 flags is the physical key;
  `RAWMOUSE.lLastX/lLastY` are unaccelerated device counts and
  `usButtonFlags` carries five buttons and the wheel. A mouse reporting
  `MOUSE_MOVE_ABSOLUTE` (remote desktop, some tablets) is converted to deltas
  or refused with a message.
- **System keys: `WH_KEYBOARD_LL`,** installed on engage and removed on
  release, on a dedicated thread with a message pump. It returns nonzero for
  everything while engaged, so Win, Alt+Tab, Alt+Esc, Ctrl+Esc and Alt+F4 never
  reach the shell. It does no other work: Windows removes a low-level hook
  that exceeds `LowLevelHooksTimeout` (at most 1000 ms) **silently**, with no
  notification. The capture controller therefore treats the hook as
  best-effort and still releases on focus loss, which is what a dropped hook
  turns into.
- **Pointer:** `ClipCursor` to the surface and hide the cursor. Clicks cannot
  land on another window because the pointer cannot leave ours.
- **WinUI 3:** the window handle comes from `WindowNative`; `WM_INPUT` is
  received by subclassing that window. Neither needs a new dependency.
- **Known wrinkles to encode in the keymap test:** Pause (E1 1D 45),
  PrintScreen (E0 2A E0 37 and the Alt variant), the fake shifts (E0 2A / E0 AA)
  around navigation keys, and NumLock's effect on the numeric pad codes.
- **Not interceptable:** Ctrl+Alt+Del and Win+L. Both take focus; both release.
- **Hypothesis:** security software may flag the low-level keyboard hook. It
  exists only while engaged and only in the signed companion process. Gate H4
  runs engaged under Defender with default settings.

Rejected: SDL3 on Windows. It would work, but it adds a native dependency to a
build that has none, to replace about 400 lines of P/Invoke.

### 10.2 Linux

The shell is Avalonia, which is an X11 client today (through XWayland on a
Wayland desktop, and under gamescope in Game Mode). The companion already
loads SDL3 for controllers.

Three candidates:

| Option | Relative motion | Swallows shortcuts | Works under | Cost |
|---|---|---|---|---|
| A. SDL3 capture window (`SDL_SetWindowRelativeMouseMode`, `SDL_SetWindowKeyboardGrab`) | Yes; pointer-constraints and relative-pointer on Wayland | X11 grab; on Wayland, keyboard-shortcuts-inhibit, which a compositor may partly decline | X11, Wayland natively | The engaged surface is an SDL window, so the keyboard is drawn a second time with SDL primitives. SDL scancodes are HID usages already, so no keymap is needed |
| B. X11 grabs on the Avalonia window (`XGrabKeyboard`, `XGrabPointer`, XInput2 raw motion) | Yes on X11 | Yes on X11; under XWayland only if the compositor allows X clients to grab | X11, gamescope (**Hypothesis**) | One renderer. Weak on GNOME/KDE Wayland |
| C. evdev with `EVIOCGRAB` | Yes | Yes, at the kernel, under anything | Everything | Needs read access to keyboard devices, which distributions withhold from ordinary users precisely to prevent keylogging. An install step that weakens the system |

**Recommendation: A, decided by a spike before any Linux code is written**
(gate L0). The spike is a 100-line SDL3 program run on the Ally X in Desktop
Mode and Game Mode and on one X11 session, answering: is motion relative, are
Alt+Tab and Super swallowed, what happens on focus loss. Option C is not
recommended at all. Option B is the fallback if SDL's window cannot coexist
with gamescope.

**Unknown:** how gamescope treats a grab or a pointer lock from a non-game
window in Game Mode. The repository's own Game Mode findings (the fill and
scale work in `companions/linux/README.md`) show it does not behave like a
desktop compositor.

### 10.3 Web portal

Chromium only, which is already the portal's constraint (Web Bluetooth).

- **Carrier:** the CL-IN characteristic on the Web Bluetooth session the portal
  already holds, `writeValueWithoutResponse`, one write in flight, newest frame
  wins. Web Bluetooth rejects overlapping operations with "GATT operation
  already in progress", so the single-flight writer is mandatory and must also
  serialize against management commands.
- **Keys:** `KeyboardEvent.code` is the physical key. **Keyboard Lock**
  (`navigator.keyboard.lock()`), effective in fullscreen, captures system
  shortcuts. Holding Esc exits fullscreen and cannot be overridden; that is
  an extra release path, not a problem.
- **Pointer:** `requestPointerLock({ unadjustedMovement: true })` for raw
  counts, falling back to adjusted movement where the option is refused.
  **Unknown:** whether Chrome on Linux honours `unadjustedMovement` today; the
  only statements found are from 2020 ("Linux will follow").
- **Not available over Web Serial.** The data plane is a GATT characteristic.
  On USB the portal offers the Armed tester only.
- **Unknown:** the connection interval. A page cannot request one, and the
  operating system's default for a browser's GATT client may be much longer
  than the 7.5 to 15 ms a native companion asks for. The adapter can ask
  (`gap_request_connection_parameter_update`, as it already does for companion
  audio) when a KB/M session starts; whether the host grants it is gate W1.
  If it does not, the portal's KB/M Link is labelled as higher latency.
- **Motion and rumble** are not part of KB/M Link, so the portal loses nothing
  relative to a native companion here.

### 10.4 Android

A phone or handheld with a keyboard and mouse attached (dock, DeX-style
desktop, Chromebook). Lowest priority.

- **Keys:** `KeyEvent.getScanCode()` is the Linux evdev code, so the evdev
  column of the keymap serves Android too.
- **Pointer:** `View.requestPointerCapture()` (API 26). Captured events arrive
  as relative motion with the cursor hidden. The system can take capture away
  at any time and reports it, which maps onto the Lost event.
- **System keys** cannot be intercepted. They release.
- **Carrier:** Android's Controller Link is the Classic HID bridge, and its
  descriptor must not gain keyboard collections (§3). KB/M Link on Android
  therefore uses CL-IN over the management GATT session Android already has.
  **Known risk, Confirmed in
  [`companion-multiplane-ble-architecture-research-2026-09-04.md`](companion-multiplane-ble-architecture-research-2026-09-04.md) §6.2:**
  `BluetoothGatt` allows one outstanding operation per device across all
  characteristics, so a management command and a frame contend. During play
  management is nearly idle, which is why this is a risk and not a blocker.
  KB/M Link would be the first Android user of that data plane, so it also
  serves as the experimental carrier that document recommends prototyping.

## 11. Failure modes

| Failure | Prevented or handled by |
|---|---|
| Key stuck on the console | Complete-state frames; ENGAGED-clear frame on release; 300 ms watchdog; role release by generation |
| Modifier stuck on the PC | Keys down at engage are not swallowed until re-pressed (§7.2 rule 3); release never swallows a key-up of a key whose key-down the OS saw |
| PC input swallowed with nowhere to go | Link loss is a release trigger; the hook and clip die with the process; the engaged watchdog |
| Stick drifts after the mouse stops | Existing: velocity estimate plus the 64 ms idle deadline, ticked on core 1 regardless of frames |
| Movement lost when frames coalesce | Cumulative counters |
| A fast tap never reaches the console | Press latch |
| Jump on engage or reconnect | Baseline on the first engaged frame |
| Wrong key on a non-US layout | Physical scan codes; shared keymap fixture |
| Companion steals the console from a real controller | Bridge class in the arbiter; explicit selection respected |
| Second keyboard confusion | Role table: duplicates rejected and reported |
| Old firmware | `info.kbm_link` absent -> feature not offered. An 0x02 frame sent anyway is counted as an unknown opcode and ignored |
| Old companion, new firmware | Nothing changes for opcode 0x01 |
| Typing a password while engaged | Unmistakable engaged state; capture only on the surface |
| Hook silently removed by Windows | Hook does no work; focus loss still releases |

## 12. Validation plan

Software, per phase:

| Area | Tests |
|---|---|
| Firmware decode | `test_ns2_companion_kbm`: goldens fixture, reserved bits, short frames, usage 0..3 cleared, counter wrap, baseline rules, ENGAGED clear |
| Firmware runtime | `test_ns2_kbm` extended: companion admission in every mode, duplicate with a Bluetooth keyboard, mixed composite, release by generation, stale generation ignored, Joy-Con 2 native path, profile-switch key from the companion |
| Arbiter | `test_ns2_input_arbiter`: companion KB/M group, class ordering against a direct controller |
| Wire parity | The parity script extended to the new constants; goldens read by C, C#, Kotlin, JS |
| Shared core | `KbmLinkStateTests` (latch, counters, bound), `KbmCaptureControllerTests` (every row of §7.2 and §7.3 against a fake backend), `KeyMapTests` (fixture) |
| Management | `protocol-v1.json` conformance in C# and Kotlin; parity check |
| Portal | A Node test for the frame encoder and keymap against the fixtures |

Hardware gates. None may be skipped or replaced by a software test.

| Gate | Question | Method |
|---|---|---|
| H1 | Does a latched 5 ms tap register in a game? | Scripted tap through the backend; observe on console and in `input status` |
| H2 | Does mouse-to-stick feel the same as a Bluetooth mouse on the same profile? | A/B in the game used for the 2026-08-18 validation; compare `kbm status` recenters and the UART trace |
| H3 | If the companion is killed while engaged, does the PC recover and the console go neutral? | Kill the process; check pointer, modifiers, `clink` neutralizations |
| H4 | Does the low-level hook survive a session, and does Defender object? | 30 minutes engaged; check the hook still swallows Win |
| H5 | Native pointer on Joy-Con 2 L/R with a PC mouse | A mouse-mode game; `kbm status` shows `native_mouse` |
| H6 | Latency added over a Bluetooth mouse | The existing link metrics plus a high-speed capture if available. Estimate before measuring: 10 to 25 ms |
| H7 | Every release trigger in §7.3, on each platform | Checklist run by a person |
| L0 | Which Linux capture option works under Desktop Mode, Game Mode and X11? | The spike in §10.2 |
| W1 | What connection interval does Chrome get, with and without the adapter's request? | `caudio status`-style interval readout during a portal session |

## 13. Phases

Each phase is independently shippable and leaves the tree green.

| Phase | Contents | Validation to close it |
|---|---|---|
| 0. Contract | Constants, both fixtures, generator and parity script, protocol JSON, this document moved to "accepted" | Fixtures consumed by a failing-then-passing test in each language |
| 1. Firmware | Decode, runtime ingress, session and release, status fields | Host tests; both boards build; `clink status` shows `kbm` on hardware |
| 2. Shared core | `KbmLinkState`, data plane, capture controller, service, surface model, with a fake backend | C# tests on Windows and Linux |
| 3. Windows | Raw Input backend, hook, clip, the surface page, source selection | H1 to H7 on Windows |
| 4. Linux | L0 spike, then the chosen backend and surface | H7 on the Ally X, both modes |
| 5. Web portal | Encoder, keymap, Pointer Lock and Keyboard Lock surface | W1, H7 in Chrome and Edge |
| 6. Android | Pointer capture, scan codes, GATT carrier | H7 on a device with a keyboard and mouse |

Phase 1 can be exercised before any companion exists: a small script writing
0x02 frames through the lab's existing BLE tooling is enough to drive the
console and prove the firmware half.

Follow-ups, not part of this feature:

- **F1. Mouse as gyro.** In a gyro-aim game the console integrates angular
  rate, so mouse counts mapped to gyro rate give true one-to-one aim, with none
  of the stick's saturation or deadzone. It would be a third mouse output
  beside "native pointer" and "right stick". It touches the motion path, which
  has its own invariants; it needs its own design and hardware evidence.
- **F2. A USB mouse personality.** Press coverage reports that the Switch 2
  accepts a USB mouse and gives it priority over Joy-Con 2 mouse mode
  (**Strong Evidence**, not observed here). An adapter that also enumerated a
  USB mouse would give native pointer input on every personality. That is a
  USB descriptor and personality change and belongs to a separate
  investigation.
- **F3. Controller and KB/M together** from one computer (pad for movement,
  mouse for aim). The adapter's arbiter is one-owner by design; this would be a
  composite of a different shape.
- **F4. Per-game profile switching** from the companion, now that the companion
  knows when KB/M play starts.

## 14. Open questions

| # | Question | Status | Resolved by |
|---|---|---|---|
| Q1 | Is one frame interval a long enough hold for every game? | Hypothesis | H1 |
| Q2 | Does the translator need different constants for companion cadence? | Hypothesis: no | H2 |
| Q3 | Does Windows drop the cursor clip when the owning process dies? | Hypothesis: yes | H3 |
| Q4 | gamescope's handling of grabs and pointer lock | Unknown | L0 |
| Q5 | `unadjustedMovement` on Chrome for Linux | Unknown | W1 session |
| Q6 | Connection interval for a browser GATT client | Unknown | W1 |
| Q7 | Android single-flight contention in practice | Risk, Confirmed mechanism | Phase 6 |
| Q8 | Is Ctrl+Alt+Shift+Z the right default chord? | Maintainer preference | Review of this document |
| Q9 | Should "Capture system shortcuts" default on? | Maintainer preference | Review |

## 15. Sources

Platform facts not taken from this repository:

- Microsoft, [LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc): hook timeout and silent removal.
- Microsoft, [Using Raw Input](https://learn.microsoft.com/en-au/windows/win32/inputdev/using-raw-input), [RAWMOUSE](https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-rawmouse), [RAWINPUTDEVICE](https://learn.microsoft.com/en-us/windows/desktop/api/winuser/ns-winuser-rawinputdevice).
- Microsoft, [Disabling Shortcut Keys in Games](https://msdn.microsoft.com/fi-fi/library/ee416808.aspx).
- SDL, [SDL_SetWindowKeyboardGrab](https://wiki.libsdl.org/SDL3/SDL_SetWindowKeyboardGrab), [SDL_SetWindowRelativeMouseMode](https://wiki.libsdl.org/SDL_SetWindowRelativeMouseMode).
- Wayland, [keyboard-shortcuts-inhibit](https://wayland.app/protocols/keyboard-shortcuts-inhibit-unstable-v1): the compositor may keep some shortcuts.
- Chrome for Developers, [Better full screen mode with the Keyboard Lock API](https://developer.chrome.com/blog/better-full-screen-mode).
- web.dev, [Disable mouse acceleration](https://web.dev/articles/disable-mouse-acceleration); MDN, [requestPointerLock](https://developer.mozilla.org/docs/Web/API/Element/requestPointerLock).
- Chromium web-bluetooth group, [operations that must not run in parallel](https://groups.google.com/a/chromium.org/g/web-bluetooth/c/QQP6ExsKHRI).
- Android, [Track touch and pointer movements](https://developer.android.com/training/gestures/movement) and the [InputFlinger pointer-capture note](https://android.googlesource.com/platform/frameworks/native/+/5cc1a69bd2/services/inputflinger/docs/pointer_capture.md).
- Moonlight, [release notes](https://github.com/moonlight-stream/moonlight-qt/releases?after=v0.6.1&page=3): the capture toggle chord and "capture system keyboard shortcuts".
- Video Games Chronicle, [Switch 2 supports USB mouse controls](https://www.videogameschronicle.com/news/switch-2-supports-usb-mouse-controls-developer-reveals/); Nintendo Life, [mouse-mode compatible games](https://www.nintendolife.com/guides/nintendo-switch-2-all-mouse-mode-compatible-games).
