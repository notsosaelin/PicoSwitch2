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
library. The production manager starts with no visible catalog entries and only displays
user-imported owned files with an exact AmiiboAPI match; a directory scan fills the carousel
progressively. It preserves AmiiboAPI source order while the game-series, amiibo-series, and
product-type arrows cycle `All` and the imported library's available values alphabetically. The
diagnostic harness retains its additional sorting controls.

The production page keeps its library manager available without an adapter connection. Cached
entries hold one mutable dump per exact catalog identity, and two independent quick-slot pointers
can reference different amiibo. The versioned JSON backup exports/imports the full library and slot
assignments so browser-cache loss does not become tag-save loss. Its staged workflow is **Assign
Highlighted to Slot N** → **Load Slot N to Adapter**. **Sync Amiibo from Adapter** validates the
latest console-written image and overwrites that identity's browser copy before acknowledging
adapter dirty protection. **Eject Adapter Amiibo** changes only presentation.

The selected carousel artwork stays centered at 100%. Four non-overlapping neighbors on each side
render at exactly 80/60/40/20%, and navigation is smoothly animated. Names are omitted from the
carousel. The formatted detail card contains only Name, Character, Game series, Amiibo series, and
Product type. The UI does not expose an original/reset copy; users format or erase through the
console.

The production page omits the retired current-input/current-output cards. Controller appearance
remains available in one compact full-width panel for the shared Pro2 body/Sony lightbar and the
independent Joy-Con Left/Right accents. Live Amiibo status appears in the header, the three lower
manager panels use equal widths and centered typography, and the log is nested under Developer
diagnostics.

Browser storage used by the diagnostic page is local to the localhost origin. The simulated
adapter state uses a separate `PicoSwitch2AmiiboDiagnostic` IndexedDB database and cannot change
firmware or a connected controller. Both launchers default to port 8765, so their browser-local
amiibo library and catalog cache use the same stable origin when run one at a time.
