# Web portal sources

- `index.html` is the production Web Serial configuration portal. It is served locally and is not
  embedded in firmware.
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
launcher window. The production launcher opens the real Web Serial portal. The diagnostic launcher
opens the hardware-free test page, which can recursively import `.bin` directories, exercise the
local library and AmiiboAPI lookup, simulate the adapter's transactional upload and persistence
behavior, apply controlled console-write bytes, and save the resulting mutable image directly into
the browser-local cached library.

The production page keeps its library manager available without a serial connection. Cached entries
hold separate Unused and Used images, and the versioned JSON backup action exports/imports the full
library so browser-cache loss does not become tag-save loss.

Browser storage used by the diagnostic page is local to the localhost origin. The simulated
adapter state uses a separate `PicoSwitch2AmiiboDiagnostic` IndexedDB database and cannot change
firmware or a connected controller. Both launchers default to port 8765, so their browser-local
amiibo library and catalog cache use the same stable origin when run one at a time.
