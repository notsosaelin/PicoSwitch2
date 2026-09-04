# What still needs a human

Both release artifacts build, install and uninstall. What remains needs a second
machine, a screen reader, or a pair of hands.

**Nothing here blocks releasing.** Neither code signing nor MSIX is required for
an ordinary GitHub release; both are optional polish and are listed as such at
the end.

---

## Release checklist

```powershell
cd windows\companion
./build.ps1 -Configuration Release                       # build + run every test
./build.ps1 -ReleaseArtifacts -Configuration Release     # both artifacts + hashes
```

- [ ] tests green (four suites, plus the descriptor parity check)
- [ ] `artifacts/` contains exactly the Setup `.exe`, the portable `.zip` and
      `SHA256SUMS.txt` — no PDBs, no MSIX, no `AppPackages`
- [ ] hashes in `SHA256SUMS.txt` match the two files
- [ ] install the Setup `.exe` from a folder outside the repository; the app
      launches, the Start Menu shortcut works, and it appears in Installed Apps
- [ ] uninstall it; shortcuts and files go, `%LOCALAPPDATA%\PicoSwitch2` stays
- [ ] extract the portable `.zip` somewhere clean and run it
- [ ] create the GitHub Release and upload **both** artifacts and
      `SHA256SUMS.txt`

### What to write in the release notes

> **Installer (recommended)** — `PicoSwitch2-Companion-Setup-<version>-x64.exe`
> Installs for you only by default, no administrator rights needed. Choose the
> location and whether you want Start Menu and Desktop shortcuts. Uninstalls
> from Settings → Apps → Installed apps. Fetches the .NET Desktop Runtime for
> you if you do not already have it.
>
> **Portable** — `PicoSwitch2-Companion-<version>-x64-portable.zip`
> Extract and run. Nothing is installed and no runtime is required; the download
> is larger because both runtimes are inside it.
>
> Windows SmartScreen may warn on first launch, because this unsigned GitHub
> release has not yet accumulated reputation. Choose **More info** → **Run
> anyway** if you downloaded it from the official PicoSwitch2 releases page.

Both builds store settings in `%LOCALAPPDATA%\PicoSwitch2`. "Portable" means no
installer is required, not that the app keeps its configuration beside the
executable — an uninstall and a re-extract both find your existing adapters.

---

## 1. Signing — optional, and not needed to release

Neither artifact is signed, and neither needs to be. SmartScreen is driven by
reputation and EV certificates, not by the presence of any signature, so a
self-signed certificate would produce the *same* first-run prompt while also
asking every user to install a root certificate. Do not do that.

If you ever want real signing, `build.ps1` already resolves credentials from
`signing.properties` or the environment, and produces unsigned output when it
finds none — see `README.md` §4. A certificate from a CA (~$200–400/yr) or a
Microsoft Store account ($19 one-off, where the Store signs on your behalf) are
the two routes.

### The MSIX path, if you ever want it

MSIX **cannot be installed unsigned** — a platform rule, not a setting — which
is why it is not the primary artifact for a free download. It is wired and
proven, so nothing has to be built later, only configured.

A self-signed certificate was created on this machine on 2026-09-04
(`CN=PicoSwitch2`, thumbprint `83FDEB8E…`, valid to 2031-09-04) and a Release
MSIX was signed with it and verified to chain to its own public `.cer`. The
thumbprint is in `windows/companion/signing.properties`, which is gitignored.

Nothing depends on it. To remove it:

```powershell
Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=PicoSwitch2' } |
    ForEach-Object { Remove-Item "Cert:\CurrentUser\My\$($_.Thumbprint)" -Force }
Remove-Item windows\companion\signing.properties
```

### The MSIX path, if you ever want it

It is wired and proven, so nothing has to be built later — only configured.

A self-signed certificate was created on this machine on 2026-09-04
(`CN=PicoSwitch2`, thumbprint `83FDEB8E…`, valid to 2031-09-04) and a Release
MSIX was signed with it and verified to chain to its own public `.cer`. The
thumbprint is in `windows/companion/signing.properties`, which is gitignored.

Nothing depends on it. To remove it:

```powershell
Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=PicoSwitch2' } |
    ForEach-Object { Remove-Item "Cert:\CurrentUser\My\$($_.Thumbprint)" -Force }
Remove-Item windows\companion\signing.properties
```

To use it for real distribution instead, you would want a certificate from a CA
(~$200–400/yr) or a Microsoft Store account ($19 one-off, and the Store signs on
your behalf so you never hold a key). Then replace the thumbprint in
`signing.properties` and run `build.ps1 -Msix -Configuration Release`. Details,
including why a password-protected `.pfx` will not work, are in `README.md` §4.

**If you keep the self-signed certificate, back up its private key.** It lives
only in `Cert:\CurrentUser\My` on this machine. An MSIX upgrade requires the
same publisher, so losing it means never being able to sign a compatible update.
Export it from `certmgr.msc` → Personal → Certificates → right-click → Export,
with the private key and a password of your choosing, and keep that `.pfx`
somewhere outside the repository.

---

## 2. Test an upgrade — only if you go the MSIX route

**Not applicable to the zip.** A zip has no upgrade mechanism: users download the
new one and replace the folder. Their configuration lives in `%LOCALAPPDATA%`
and is untouched by that, which is what
`AdapterRegistryStoreTests.TheDefaultLocationIsUnderLocalAppDataSoBothPackageFlavoursAgree`
pins.

The automated half is done regardless: `UpgradePersistenceTests` pins the exact
on-disk JSON the shipping build writes and proves the current decoders still
read it, so a codec change that would empty someone's adapter list fails in CI.

If you later ship MSIX and want the full check, it needs two signed packages
installed in sequence:

1. Build and sign `0.1.0.0`, install it, and create real state: pair an adapter,
   **rename it** (the alias is the field a user typed, so it is the one worth
   checking), let a peer inventory populate, add an Amiibo, change a setting,
   edit a Touch Gamepad layout.
2. Bump `<Identity Version>` in `Package.appxmanifest` to `0.1.1.0`, build, sign,
   and publish both the `.msix` and the generated `.appinstaller`.
3. Launch the installed app. With `HoursBetweenUpdateChecks="0"` it checks on
   every launch, so the update should offer itself immediately.
4. Confirm all of it survived: adapter list, alias, active selection, peer
   history, Amiibo library, settings, Touch Gamepad layout.

---

## 3. The accessibility pass — Phase 8's exit criterion

**Why it cannot be done for you:** the criterion is a person completing real
tasks with a screen reader. The automated part (every glyph-only button carries
an accessible name) is already guarded by
`LayeringGuardTests.AGlyphOnlyButtonAlwaysCarriesAnAccessibleName`.

Turn Narrator on with **Ctrl + Win + Enter**. Then, without looking at the
screen, complete each of:

- [ ] first pairing of an adapter, start to finish;
- [ ] switching the controller personality;
- [ ] uploading an Amiibo.

Each should be possible from announcements alone. What you are listening for:
every control announces what it *is* and what it *does*; a busy state says it is
busy rather than going silent; an error says what went wrong and what to do.

Also, by keyboard only, with no mouse:

- [ ] reach every control on every page with Tab / Shift+Tab, in an order that
      matches the visual one;
- [ ] drive the Touch Gamepad layout editor end to end — select, move, resize,
      undo, save.

---

## 4. The rest of §26.5 — the manual UX pass

Each of these has found real bugs in comparable apps, and none can be faked in a
unit test.

- [ ] **200% text scale** (Settings → Accessibility → Text size). Nothing
      clipped, nothing overlapping.
- [ ] **High contrast** themes. Every control still distinguishable; the Touch
      Gamepad's controls still visible against both a light and a dark ground.
- [ ] **Light / dark / system theme switching *while connected*.** The switch
      must not drop the management session.
- [ ] **Minimum window size.** Nothing unreachable; no horizontal scrollbar.
- [ ] **Per-monitor DPI change** by dragging the window between your 4K and the
      1920×1200. Layout and the Touch Gamepad both re-resolve cleanly. *(This is
      where the white title-bar bar lived, so it is worth a second look.)*
- [ ] **Disconnect the adapter during every major workflow** — mid-Amiibo
      upload, mid-personality switch, mid-KB/M bind. Each page must degrade to a
      stated reason, never to a spinner that never ends. This is the one that
      finds real bugs.

Touch-specific, on a real touchscreen rather than an emulator:

- [ ] the gameplay surface at several window sizes and both orientations;
- [ ] the layout editor driven entirely by **keyboard**, then **mouse**, then
      **pen**, then **touch**;
- [ ] layout audit findings announced and legible;
- [ ] control opacity at its default against a light and a dark background under
      high contrast.

---

## 5. The two-machine release regression — §26.6

**Why it cannot be done for you:** it requires a second machine with a
*different* Bluetooth radio. Radio behaviour is the single biggest source of
variation in this product, and this repository has already spent two passes on
defects that were one adapter's firmware rather than the code.

Before any release:

- [ ] §26.1–§26.3 green (`build.ps1` does this);
- [ ] cross-language guards green, including
      `tools/check_android_descriptor_parity.py`;
- [ ] hardware cases **H1–H16 and H20–H22** run on **two machines with different
      radios**;
- [ ] §26.5 above run once;
- [ ] the support bundle inspected by eye and confirmed free of addresses, keys,
      raw Amiibo bytes and management JSON — there is a redaction test, but a
      human reading it once is the point;
- [ ] the Android suites re-run, to prove the Windows pass changed nothing there.

---

## 6. One thing worth a review rather than a test

**Every destructive confirmation should name its consequence** — "Forget this
adapter? You will need to pair it again" rather than "Are you sure?". The
dialogs live in `AdapterPage`, `AmiiboPage`, `KeyboardMousePage`, `SettingsPage`,
`DiagnosticsPage` and `TouchGamepadView`.

No automated guard was written for this, deliberately: any check would be
pattern-matching prose and would pass on text that reads badly. It wants one
read-through, not a test.
