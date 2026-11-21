# Web portal

`index.html` is the browser companion: one self-contained page, no build step,
served from GitHub Pages and from `tools\run_config_portal.ps1`. It talks to the
adapter over USB Web Serial (configuration mode) or the bonded, encrypted BLE
management link, and is not embedded in firmware. Chrome or Edge only.

`diagnostic.html` is a browser-only Virtual Amiibo test harness. It never opens
a transport.

## Pages

The portal follows the companion shell
([`docs/architecture/companion-shell-layout.md`](../docs/architecture/companion-shell-layout.md)):
the same pages, in the same order, holding the same things.

| Page | Holds | Needs |
| --- | --- | --- |
| Play | Controller Link from a controller attached to this computer, the console buttons (Home, Capture, C), Wake, who is driving the console, the adapter's amiibo | Bluetooth for Controller Link |
| Keyboard | The on/off switch, the drawn keyboard with every key's binding, mouse buttons and mouse tuning, the profiles on the adapter, profile switch keys | An adapter |
| Amiibo | The library (below). Works with no adapter connected | Nothing |
| Customize | Body and Joy-Con accent colours | An adapter |
| Adapter | Emulated controller, controller in use, Wake, Bluetooth management; pair a controller; paired devices with Forget | An adapter |
| Settings | Face-button layout, rumble, browser support, Controller Link counters, developer diagnostics | Nothing |

`index.html#kbm` (or any page name: `play kbm amiibo custom adapter settings`)
opens that page.

What a browser cannot do, and the page says so where it matters:

| Companion feature | Portal |
| --- | --- |
| Motion in Controller Link | No. A page cannot read a controller's IMU |
| Touch Gamepad | No |
| Headset and microphone | No |
| Editing saved KB/M profiles (create, rename, staged save) | No. The portal edits the mapping that is running and chooses which stored profile runs now and at power-up |
| Controller Link over USB | No. The data plane is a Bluetooth characteristic |
| Holding the adapter connection in the background | No. Controller Link pauses, and the console gets a neutral report, when the tab is hidden or loses focus |
| Keyboard & Mouse Link | Designed, not built: [`docs/architecture/kbm-link-hld.md`](../docs/architecture/kbm-link-hld.md) |

## Controller Link in the browser

The page reads the Gamepad API's `standard` mapping, encodes the canonical
26-byte bridge report, and writes it to the Controller Link characteristic with
one write in flight and the newest state winning, exactly as the Windows
companion does. Rumble and the player number come back on the feedback
characteristic.

- The browser's layout is **positional**: its bottom face button is button 0
  whatever is printed on it. The default sends position (bottom is B); Settings
  can send Xbox letters instead.
- A controller the browser reports with a non-`standard` mapping is refused,
  because its buttons cannot be trusted.
- The page refuses to start if the adapter reports a different bridge contract.
- **Not yet run against an adapter.** The encoder is held to
  `contracts/bridge_report_goldens.csv`; the link itself (connection interval
  under Web Bluetooth, latency, rumble) is STATUS.md gate 16.

## Checking a change

```powershell
python tests\run_tool_tests.py portal     # encoder goldens, page structure, paging rules, escaping
python tools\portal_preview.py --shots    # every page rendered against the repository's fixtures
```

`tools/portal_preview.py` writes a scratch copy of the page whose transport is
answered from `contracts/management/`, so the connected pages can be looked at
with no adapter. Screenshots land in `build/portal-preview/shots/`. It proves
layout, never adapter behaviour.

Rules the tests hold:

- Text from the adapter never reaches `innerHTML`. New markup is built with
  `createElement` and `textContent` (`el()` in the page).
- The drawn keyboard is the table in the C# `KeyboardLayout.cs`; regenerate the
  portal's copy rather than editing it.
- A paged read that changes its total, repeats a cursor or comes up short is a
  failure, never a shorter list.

## Running it locally

Run the production portal from the repository root:

```powershell
.\tools\run_config_portal.ps1
```

Run the diagnostic portal without hardware:

```powershell
.\tools\run_amiibo_portal_test.ps1
```

## The amiibo library

Both launchers serve `web/` from localhost and stop their local server when Enter is pressed in the
launcher window. The production launcher opens the real USB/Bluetooth portal. Standard firmware
also advertises management in normal controller personalities while the console is awake; a new
phone bond still requires the double-tap pairing window and writes require a bonded 16-byte
encrypted ATT link. The two-second hold remains the route to the CDC Config USB personality. The
diagnostic launcher opens the hardware-free test page, which
can recursively import `.bin` directories, exercise the local library and AmiiboAPI lookup,
simulate the adapter's transactional upload and persistence behavior, apply controlled
console-write bytes, and save the resulting mutable image directly into the browser-local cached
library. The production manager starts with no visible catalog entries and displays only validated
user-imported files; a directory scan fills the carousel progressively. AmiiboAPI enriches and
orders known entries but never gates import. Three compact game-series, amiibo-series, and
product-type chips cycle `All` and the imported library's available values alphabetically. The
diagnostic harness retains its additional sorting controls.

The production page keeps its library manager available without an adapter connection. Cached
entries hold mutable validated dumps, with content-derived keys for distinct v3 combinations; the
board stores exactly one amiibo, so the manager has one loaded pointer. The library
exports/imports as a flat `.zip` (`library.json` manifest plus one `.bin` per amiibo; legacy
`.json` backups still import) so browser-cache loss does not become tag-save loss. Connected
**Load amiibo** uploads the highlighted image immediately; offline **Select amiibo** remembers it.
Conditional **Import amiibo**/**Sync amiibo** validates the current adapter image and updates the
matching browser copy before acknowledging dirty protection. Load/Select and Import/Sync share the
single center action position so the manager never presents competing primary actions. On
connection, the portal follows the adapter's active library entry once, then leaves later carousel
browsing under user control. The merged button is always labeled **Eject amiibo** while its tooltip
and confirmation describe the exact scope.

The library is import-only: users supply their own genuine dumps. A key-based generator was
prototyped and removed in favor of import-only simplicity. The smaller key-based rewrite path is
retained for explicit **Initialize amiibo** on an imported dump: it works with or without an
adapter connection, requests the user's own `key_retail.bin` when needed, clears ordinary and v3
game/save state, re-signs locally, and self-verifies before replacing the browser copy. Keys and
tag bytes never leave the browser. The amiibo identity/crypto research is retained in
[`../docs/switch2/amiibo-identity-and-generation.md`](../docs/switch2/amiibo-identity-and-generation.md).
The AmiiboAPI catalog is enhancement-only (entries display even when it is offline), and carousel
navigation wraps at the ends while the visible neighbor window remains non-wrapping. The
centered amiibo's release date appears above it.

The selected carousel artwork stays centered at 100%. Four non-overlapping neighbors on each side
render at exactly 80/60/40/20%, and navigation is smoothly animated. The focused carousel accepts
keyboard arrows, a mouse wheel/trackpad while hovered, and horizontal touch/pen swipes; the old
visible arrow buttons and `1 of N` counter are intentionally absent. Names are omitted from the
artwork row; the centered name and compact save facts appear below it. The three tap-to-cycle
filters sit together below the compact primary action and collapse to one column on narrow screens.
When user-owned keys are available, write count appears as a badge on the centered artwork.
Clicking that artwork toggles an inline, non-modal context drawer containing Character, Game
series, Amiibo series, Product type, compatible games, Download, Initialize, Eject, and Delete.
Initialize changes the mutable browser copy; the UI still does not expose the adapter journal's
internal original/recovery pair.

Controller appearance is on the Customize page: the shared Pro2 body/Sony lightbar and the
independent Joy-Con Left/Right accents. Amiibo operation status lives inside the manager rather
than occupying the global connection header. Search remains compact in the manager toolbar because
large owned libraries cannot be navigated efficiently by carousel alone. The log is under Settings, in Developer diagnostics.

The production markup reserves a capability-gated **Scan physical amiibo** action, but keeps it
hidden until firmware exposes a safe Config-transport scan API. The existing UART `nfc_probe`
initiator proves the underlying research path, not a production Config-mode contract; the portal
does not ship a dead or misleading button.

Browser storage used by the diagnostic page is local to the localhost origin. The simulated
adapter state uses a separate `PicoSwitch2AmiiboDiagnostic` IndexedDB database and cannot change
firmware or a connected controller. Both launchers default to port 8765, so their browser-local
amiibo library and catalog cache use the same stable origin when run one at a time.

Figure-v3 initialization is allocation-independent. It clears the captured sector-0 Air Riders
window and the complete second 1 KB user-memory sector while preserving sector-0 chip
configuration and the 64-byte machine/SRAM response. This covers Kirby's sector-1 pages
`0x00/0x01`, King Dedede's `0x64/0x65`, and future allocations following the same tag format
without storing a character table.
