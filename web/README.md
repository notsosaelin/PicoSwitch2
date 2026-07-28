# Web portal sources

- `index.html` is the production configuration portal. It supports USB Web Serial and the
  Config-personality-only BLE GATT transport, is served locally, and is not embedded in firmware.
- `diagnostic.html` is a browser-only Virtual Amiibo test harness. It is not embedded in firmware
  and never opens Web Serial.

Run the production portal locally from the repository root:

```powershell
.\tools\run_config_portal.ps1
```

Run the diagnostic portal without hardware:

```powershell
.\tools\run_amiibo_portal_test.ps1
```

Both launchers serve `web/` from localhost and stop their local server when Enter is pressed in the
launcher window. The production launcher opens the real USB/Bluetooth portal. Bluetooth is
advertised and accepted only after a two-second BOOTSEL hold has entered Config; it is not a
normal-controller-mode service. The diagnostic launcher opens the hardware-free test page, which
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

The production page omits the retired current-input/current-output cards. Controller appearance
remains available in one compact full-width panel for the shared Pro2 body/Sony lightbar and the
independent Joy-Con Left/Right accents. Amiibo operation status lives inside the manager rather
than occupying the global connection header. Search remains compact in the manager toolbar because
large owned libraries cannot be navigated efficiently by carousel alone. The log remains nested
under Developer diagnostics.

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
