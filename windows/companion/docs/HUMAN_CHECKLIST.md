# What still needs a human

Everything in Phases 8 and 9 that could be automated is done. What remains needs
a certificate, a second machine, a screen reader, or a pair of hands — and each
one is listed here with enough detail to be done in one sitting.

Nothing below blocks *using* the app. The unpackaged self-contained build runs
today, and an unsigned MSIX installs on a machine with developer mode on. These
are the steps between "works" and "shippable to strangers".

---

## 1. Get a code-signing certificate — everything else in Phase 9 waits on this

**Why it cannot be done for you:** a certificate is an identity. `CLAUDE.md`
forbids committing one, and `WINDOWS_PASS.md` §27.2 requires credentials to live
outside version control. The build is already wired for one and produces an
unsigned package until it finds it.

**What to get.** Any of these works; they differ in who trusts the result.

| Option | Cost | Who trusts it |
|---|---|---|
| A public CA's code-signing certificate (Sectigo, DigiCert, SSL.com…) | ~$200–400/yr, OV; more for EV | everyone, no SmartScreen warning after reputation builds |
| Microsoft Store publishing | $19 one-off individual account | everyone; the Store signs it for you and you never hold a key |
| Self-signed | free | only machines you install the certificate on — fine for you and testers, not for strangers |

**If you only want to test the pipeline,** self-signed is enough and takes a
minute. The subject **must** be `CN=PicoSwitch2`, matching `Package.appxmanifest`
`<Identity Publisher="…">`, or signing fails:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=PicoSwitch2' `
    -KeyUsage DigitalSignature -FriendlyName 'PicoSwitch2 signing' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
$cert.Thumbprint
```

**Wire it up.** Create `windows/companion/signing.properties` — already
gitignored, and never commit it:

```properties
certificateThumbprint = <the thumbprint printed above>
appInstallerUrl = https://<wherever you publish releases>
```

Then:

```powershell
cd windows\companion
.\build.ps1 -Msix -Configuration Release
```

You should see `signing: certificate store thumbprint …`, a `package:` line, and
an `appinstaller:` line. If you see `the package will be UNSIGNED`, the
properties file was not found or the key names are misspelled.

**A password-protected `.pfx` will not work** — the packaging task cannot import
one. `build.ps1` stops with the fix rather than letting MSBuild emit APPX0105.
Import it to the store once and use the thumbprint. Details in `README.md` §4.

**Verify:**

```powershell
Get-AuthenticodeSignature <path to the .msix> | Format-List Status, SignerCertificate
```

`Valid` for a CA certificate. `UnknownError` for a self-signed one is expected —
its chain does not validate until you install it into Trusted Root on the test
machine.

---

## 2. Publish a release, then test the upgrade

**Why it cannot be done for you:** it needs two signed packages, published at a
real URL, installed in sequence.

The automated half is done: `UpgradePersistenceTests` pins the exact on-disk JSON
the shipping build writes and proves the current decoders still read it, so a
codec change that would empty someone's adapter list fails in CI. What that
cannot prove is that MSIX itself carries `%LOCALAPPDATA%` across a version bump.

**Steps:**

1. Build and sign `0.1.0.0`. Install it. Use it enough to create real state:
   pair an adapter, **rename it** (the alias is the field a user typed, so it is
   the one worth checking), let a peer inventory populate, add an Amiibo to the
   library, change a setting, and edit a Touch Gamepad layout.
2. Bump `<Identity Version>` in `Package.appxmanifest` to `0.1.1.0`. Build, sign,
   publish both the `.msix` and the generated `.appinstaller` to
   `appInstallerUrl`.
3. Launch the installed app. With `HoursBetweenUpdateChecks="0"` it checks on
   every launch, so the update should offer itself immediately.
4. After updating, confirm **all** of this survived: the adapter list, the alias
   you typed, which adapter is selected, peer history, the Amiibo library, your
   settings, and your Touch Gamepad layout.

If something is lost, that is a real defect — capture which document and it is
fixable in the codec, which is exactly what the automated test then pins.

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
