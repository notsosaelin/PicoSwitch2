# Touch Gamepad Personality-Aware Layout Engine — High-Level Design

Status: Proposed extension to an implemented baseline

Repository baseline: `ns2-testing` at `d767674d856ac002835274c094462a8e736386ff`, plus the current working tree

Last audited: 2026-08-23

## 1. Decision summary

PicoSwitch2 should extend the existing Touch Gamepad engine into a personality-aware layout system.
It should not replace that engine with a second hierarchy of controls that each own rendering, hit
testing, and input behavior.

The design has five central decisions:

1. Keep `:bridge-core` as the platform-neutral owner of touch semantics, contact ownership, layout
   resolution, validation, and release behavior.
2. Add a profile catalog that describes each console-facing personality's visible controls and the
   fixed bridge action that produces each one.
3. Keep shipped templates immutable and persist sparse, versioned user overrides by stable control
   id. The effective layout is `default template + user override`.
4. Keep rendering in the Android app as a pure consumer of resolved geometry and visual roles.
   Android resource ids and bitmap assets must not enter `:bridge-core`.
5. A personality change must neutralize touch input and replace the active profile atomically. It
   must not tear down the surviving Classic HID controller link.

This design does not change the Android HID descriptor, bridge report layout, console-facing USB
reports, or the locked mapping for directly paired physical controllers. Its implemented baseline
does include the narrow Android-bridge face correction described in section 6: only logical
A/B/X/Y bridge usages bypass the positional physical-controller swap.

## 2. Audit conclusions

The original research had useful product goals, but its starting model and several personality
requirements did not match the repository.

| Original claim or proposal | Repository evidence | HLD disposition |
|---|---|---|
| Touch Gamepad is hardcoded Pro Controller 2 UI that first needs a layout engine. | [`TouchLayout.kt`](android/companion/bridge-core/src/main/kotlin/dev/picoswitch/bridge/touch/TouchLayout.kt), [`TouchLayoutV1.kt`](android/companion/bridge-core/src/main/kotlin/dev/picoswitch/bridge/touch/TouchLayoutV1.kt), and [`TouchLayoutAudit.kt`](android/companion/bridge-core/src/main/kotlin/dev/picoswitch/bridge/touch/TouchLayoutAudit.kt) already provide a versioned declarative layout, safe-area resolution, stable ids, and mechanical validation. | Treat the current engine as the baseline and evolve it. |
| Each `TouchElement` should own rendering, bounds, and input behavior. | The current design deliberately separates semantic action, geometry, input state, and the Android renderer. [`TouchControlRenderer.kt`](android/companion/app/src/main/java/dev/picoswitch/companion/ui/touch/TouchControlRenderer.kt) draws the resolved list in one pass. | Reject combined ownership. It would weaken portability, validation, and rendering performance. |
| D-pad should be a special unified control. | The current `Directions` action and D-pad resolver already provide one claimed region, eight sectors, and hysteresis. | Retain for Pro2 and GameCube. A Joy-Con direction cluster is different: four independent visual buttons with four fixed bridge actions. |
| Shipped layouts should be JSON files. | Current typed Kotlin templates are compile-time checked and directly covered by JVM tests. No runtime content pipeline needs JSON defaults. | Keep shipped defaults typed and code-owned. Use a versioned persisted document only for user overrides. |
| A layout element should persist `inputMapping`. | Controller remapping is a separate firmware capability gap, and the touch path must not create a second mapping system. | The profile owns a fixed output-to-bridge binding. Users may change presentation and geometry, never the action. |
| Pro Controller 2 requires a greenfield refactor. | The current V1 template already represents the full Pro2 control set and is resolved through the existing bridge. | First acceptance gate is parity: the new profile-backed Pro2 layout must preserve behavior and appearance. |
| GameCube needs A/B/X/Y, Z, L/R, Start, D-pad, and two sticks. | The implemented NSO GameCube personality also exposes Plus, Minus, ZL, Home, Capture, C, continuous analog L/R, and separate L/R detents; L3/R3 are forbidden. See [`docs/switch2-gc/mapping.md`](docs/switch2-gc/mapping.md). | Replace the incomplete classic-controller inventory with the repository's NSO GameCube output contract. |
| Joy-Con Left and Right should use upright half-controller inventories; the Right template's touch stick should write the generic right-stick channel. | Both personalities are hardware-confirmed horizontal single-controller mappings. For a non-native source such as the Android bridge, the generic left-stick/L3 channels are the established **primary input source** for either half. The side-aware firmware then rotates that input and writes the emulated half's one native stick field. See [`docs/switch2-joycon2/mapping.md`](docs/switch2-joycon2/mapping.md). | Ship sideways, gameplay-oriented templates that match the existing firmware policy. Treat `Stick(Left)` as an internal bridge-source name, not as the console-facing identity of the Right Joy-Con's stick. Do not invent an upright or paired Joy-Con mode. |
| A personality change should close the controller link and require a new session. | Controller-personality re-enumeration resets USB personality state but deliberately preserves the Bluetooth/seam/report layers. | Neutralize and switch profiles without disconnecting Classic HID. Config or unknown personality disables gameplay until a valid profile is confirmed. |
| melonDS Android and WatermelonDS are two independent examples. | WatermelonDS is a fork of melonDS Android and retains the same layout-domain lineage. | Count this as one design family, not two independent confirmations. |
| `refs/526685-middle.png` can become GameCube artwork after cleanup. | The file is a maintainer-supplied scan of physical GameCube controller buttons. It is used only as a private geometry reference for tracing and isolating the button silhouettes; the raster itself is not an application asset and will not be redistributed. It contains only the face cluster, not the whole controller. | Build the shipped controls as app-owned vector/code-native geometry. Do not package, render, or otherwise depend on the scan at runtime. |

### External research retained

Dolphin demonstrates separate button, D-pad, joystick, and pointer collections and a distinct edit
mode in
[`InputOverlay.kt`](https://github.com/dolphin-emu/dolphin/blob/123d32248e455ec242866e3a75fea70dcecfd567/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/overlay/InputOverlay.kt).
Those are useful interaction precedents, but Dolphin's drawable objects also carry responsibilities
that PicoSwitch2 already separates more cleanly.

melonDS Android demonstrates default versus custom layout identity, a layout repository, reset,
editing, and persistence in
[`LayoutConfiguration.kt`](https://github.com/rafaelvcaetano/melonDS-android/blob/ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6/app/src/main/java/me/magnum/melonds/domain/model/layout/LayoutConfiguration.kt) and
[`InternalLayoutsRepository.kt`](https://github.com/rafaelvcaetano/melonDS-android/blob/ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6/app/src/main/java/me/magnum/melonds/impl/InternalLayoutsRepository.kt).
The reusable lesson is immutable defaults plus recoverable user state, not its emulator-specific
screen-layout model.

[WatermelonDS](https://github.com/SapphireRhodonite/WatermelonDS/tree/1e0a463d785448ab47f78a12e2a88241df288cf9)
is a melonDS Android fork and therefore corroborates that lineage rather than providing an
independent architecture sample.

## 3. Scope

### Goals

- Select a truthful default layout for Pro Controller 2, NSO GameCube, Joy-Con 2 Left, and Joy-Con
  2 Right.
- Preserve the current low-latency input path and stable pointer ownership.
- Keep control semantics, geometry, rendering, and persistence separate.
- Allow users to move, resize, group-edit, hide, show, and restore controls.
- Preserve immutable defaults and migrate user overrides across compatible template revisions.
- Keep labels and shapes consistent with the console-facing output produced by the existing bridge
  and firmware mapping.
- Fail neutral when the personality, persisted layout, or resolved geometry is invalid.
- Make future personalities additive: one profile, one or more templates, bindings, renderer roles,
  and tests.

### Non-goals

- No arbitrary button remapping.
- No Android HID descriptor or bridge contract change.
- No broader firmware mapping redesign beyond the implemented, face-only Android-bridge correction.
- No paired Joy-Con L+R identity; one Pico still presents one USB controller identity.
- No synthesized upright Joy-Con mode that conflicts with the accepted sideways policy.
- No Android types, drawable ids, file paths, or content URIs in `:bridge-core`.
- No runtime download of layouts or assets.
- No partial-travel touch gesture for analog triggers in the first release. The existing touch
  trigger is a full-pull press.
- No arbitrary user rotation in the first editor release. Rotation requires rotated hit testing and
  overlap auditing, not just rotated artwork.
- No claim of hardware validation until the physical matrix in section 13 passes.

## 4. Existing baseline to preserve

The current path is already the correct foundation:

```text
Android pointer events
  -> Modifier.touchGamepadContacts                  app / Android
    -> TouchContactTracker                         bridge-core
      -> TouchControlEngine                        bridge-core
        -> TouchContribution                       bridge-core
          -> ControllerInputState                  bridge-core
            -> BridgeSession / ControllerReportEncoder
              -> AndroidHidTransport
                -> PicoSwitch2 -> active USB personality

TouchLayout template + interaction-safe region
  -> TouchLayoutResolver
    -> TouchLayoutAudit
      -> ResolvedTouchLayout
        -> TouchControlEngine + Android canvas renderer
```

The redesign must preserve these established invariants:

- contacts are keyed by stable pointer id, never pointer index;
- a contact claims at most one control on Down and cannot wander into another;
- a control has at most one owning contact;
- physical and touch gameplay input never merge;
- every invalidating boundary uses the same idempotent release path;
- a geometry or profile replacement releases before installing new geometry;
- every face label agrees with the final console output after the Android HID usage, the
  bridge-aware firmware seam, and the selected personality encoder; an intermediate
  `ControllerState` match alone is not sufficient evidence;
- gameplay state is never persisted;
- an invalid layout sends neutral input and does not guess.

## 5. Target architecture

```text
confirmed adapter Personality
  -> TouchProfileSelector                         app orchestration
    -> TouchProfileCatalog                        bridge-core
      -> immutable TouchLayoutTemplate
      +  sparse TouchLayoutOverride               platform store
        -> TouchLayoutComposer
          -> profile-aware TouchLayoutAudit
            -> TouchLayoutResolver
              -> ResolvedTouchLayout
                + TouchControlEngine              semantic input
                + TouchControlRenderer            Android presentation
```

### 5.1 Responsibility boundaries

| Component | Responsibility | Must not own |
|---|---|---|
| `TouchProfileCatalog` | Supported personality profiles, output capabilities, fixed bridge bindings, default template ids | Android resources, live contacts, persistence |
| `TouchLayoutTemplate` | Immutable shipped control geometry, visual roles, edit groups | User state, platform resources |
| `TouchLayoutOverrideStore` | Versioned per-profile user overrides | Gameplay state, default mutation, input semantics |
| `TouchLayoutComposer` | Merge template and sparse override by stable id | Rendering, platform storage APIs |
| `TouchLayoutAudit` | Profile contract, finite values, unique ids, bounds, target size, overlap, required safety controls | Silent repair of ambiguous geometry |
| `TouchLayoutResolver` | Convert authored logical units into real safe-area geometry | Personality discovery, persistence |
| `TouchControlEngine` | Contact ownership and semantic contribution | Visual assets, management state |
| Android renderer/editor | Draw visual roles and collect edit gestures | Bridge mappings, contact-to-controller semantics |
| `TouchProfileSelector` | Map confirmed management personality to a profile and coordinate safe replacement | Firmware mutation, Bluetooth teardown |

## 6. Domain model

Names below are conceptual. Implementation may reuse and extend the existing types rather than
introducing all-new classes.

```kotlin
data class TouchControllerProfile(
    val id: TouchProfileId,
    val displayName: String,
    val defaultTemplateId: String,
    val outputs: Set<TouchOutputControl>,
    val bindings: Map<TouchOutputControl, TouchControlAction>,
)

data class TouchLayoutTemplate(
    val id: String,
    val profileId: TouchProfileId,
    val schemaVersion: Int,
    val templateRevision: Int,
    val controls: List<TouchControlSpec>,
)

data class TouchControlSpec(
    val id: String,
    val output: TouchOutputControl,
    val interaction: TouchControlKind,
    val geometry: TouchControlGeometry,
    val visual: TouchVisualSpec,
    val editGroupId: String? = null,
)

data class TouchLayoutOverride(
    val schemaVersion: Int,
    val profileId: TouchProfileId,
    val templateId: String,
    val basedOnRevision: Int,
    val controls: Map<String, TouchControlOverride>,
)

data class TouchControlOverride(
    val anchorX: Float? = null,
    val anchorY: Float? = null,
    val scale: Float? = null,
    val visible: Boolean? = null,
)
```

`TouchOutputControl` describes what the user sees on the emulated controller. The profile's binding
describes the fixed generic bridge action that makes the existing firmware produce that output.
This distinction is required for Joy-Con sideways mappings and GameCube Z/ZL/trigger behavior.

For buttons, a binding names an Android bridge source usage, not an instruction to reuse the
locked direct-controller map blindly. The companion has already normalized its first four usages
to logical A/B/X/Y. The firmware parser exposes those usages as `JP_BUTTON_B1..B4`, and the
source-aware seam maps them directly to canonical A/B/X/Y destinations when
`from_android_bridge` is true. Directly paired physical controllers continue through the locked
`NS2_BASE_BUTTON_MAP` (B/A/Y/X for those four slots). The raw JP bitmap is not rewritten because
personality encoders such as sideways Joy-Con 2 consume it deliberately.

The override cannot contain an action, output binding, kind, asset path, or arbitrary label. That is
the boundary that keeps customization from becoming an unsupported remapping system.

### 6.1 Flat runtime controls, grouped editor behavior

Compound visual groups must not become compound input owners. A face cluster still consists of four
independent controls so four contacts can press it. `editGroupId` lets the editor move or scale those
controls together while the runtime engine continues to see a flat list.

The same rule applies to Joy-Con direction buttons. Pro2 and GameCube use one vector D-pad control;
Joy-Con Left uses four independent buttons because its directional cluster is physically and
semantically four buttons.

## 7. Personality contracts and fixed bindings

These bindings describe the current Android generic-controller input that the firmware already
translates. They are not user remaps.

### 7.1 Pro Controller 2

Retain the existing V1 behavior:

- face diamond through `Face(position)` and the shared face-layout resolver;
- unified D-pad through `Directions`;
- left and right sticks;
- L/R, ZL/ZR, L3/R3, Minus/Plus, Home, Capture, and C.

The selectable face presentations are an explicit label-to-logical-bridge contract:

| Position | Nintendo label / bridge action | Xbox label / bridge action |
|---|---|---|
| South | B / `Logical(B)` | A / `Logical(A)` |
| East | A / `Logical(A)` | B / `Logical(B)` |
| West | Y / `Logical(Y)` | X / `Logical(X)` |
| North | X / `Logical(X)` | Y / `Logical(Y)` |

Those logical usages reach the same-named canonical destination only because Android-bridge
provenance selects the face-only seam rule described in section 6. Applying the direct-controller
base map again would reverse both layouts. No shoulder, trigger, system button, or directly paired
controller mapping changes with this rule.

Menu entry is app-shell navigation, not a controller-layout element. On Android, a committed Back
gesture from either screen edge opens the menu during play and closes it while it is visible. Only
the explicit Exit action inside that menu leaves Touch Gamepad mode.

The first profile-backed template must match the current resolved geometry and renderer output at all
existing layout probes before other personalities are added.

### 7.2 NSO GameCube

The template represents the implemented NSO GameCube identity, not a simplified original GameCube
controller:

| Visible output | Fixed bridge action |
|---|---|
| A, B, X, Y | matching logical `ControllerButton` |
| D-pad | `Directions` |
| Main Stick | `Stick(Left)` |
| C Stick | `Stick(Right)` |
| ZL | `Logical(L1)` |
| Z | `Logical(R1)` |
| L full pull plus detent | `Trigger(Left)` |
| R full pull plus detent | `Trigger(Right)` |
| Minus / Plus | `Logical(Select)` / `Logical(Start)` |
| Home / Capture / C | matching logical button |

L3 and R3 must not appear. The GameCube renderer may use distinct A/B/X/Y shapes, but their visual
silhouettes do not change their independent hit regions or actions.

The current touch-trigger interaction is binary: it publishes the analog endpoint and digital press
together. On GameCube that produces a full native trigger pull plus its detent; it does not claim to
provide partial analog travel.

### 7.3 Joy-Con 2 Left, sideways single-controller mode

| Visible output | Current normalized bridge source |
|---|---|
| Native Joy-Con 2 (L) stick | Primary analog source (`Stick(Left)` in the current bridge contract) |
| Stick click | `Logical(LeftStick)` |
| Up / Left / Right / Down buttons | `Logical(X)` / `Logical(A)` / `Logical(Y)` / `Logical(B)` |
| SL / SR | `Logical(L1)` / `Logical(R1)` |
| L / ZL | `Trigger(Left)` / `Trigger(Right)` |
| Minus / Capture | `Logical(Select)` / `Logical(Capture)` |

The four direction controls are independent buttons. They must not use `Directions`, because the
firmware deliberately uses the generic source D-pad as a second source for the one Joy-Con stick.

### 7.4 Joy-Con 2 Right, sideways single-controller mode

| Visible output | Current normalized bridge source |
|---|---|
| Native Joy-Con 2 (R) stick | Primary analog source (`Stick(Left)` in the current bridge contract) |
| Stick click | `Logical(LeftStick)` |
| A / B / X / Y | `Logical(A)` / `Logical(X)` / `Logical(B)` / `Logical(Y)` |
| SL / SR | `Logical(L1)` / `Logical(R1)` |
| R / ZR | `Trigger(Left)` / `Trigger(Right)` |
| Plus / Home / C | `Logical(Start)` / `Logical(Home)` / `Logical(C)` |

The word `Left` above describes only the intermediate, full-gamepad-shaped bridge channel. It does
not describe what PicoSwitch2 advertises to the console. For the Right personality, the complete
path is:

```text
touch primary stick
  -> generic bridge left-stick channel
  -> side-aware Right Joy-Con rotation/encoding
  -> Joy-Con 2 (R) report 0x08, one local stick field
  -> console sees the native stick of a Right Joy-Con
```

Thus the emulated Right Joy-Con is not advertised as having a physical left stick. When registered
sideways, its sole native stick becomes the controller's primary gameplay stick through the same
console-side interpretation used for a genuine Right Joy-Con. The app must not substitute the
generic right-stick channel merely because the emulated hardware is the right half; the current
generic sideways encoder does not consume that channel. A future bridge-model rename such as
`PrimaryStick` could remove the naming ambiguity, but it would be a terminology/API cleanup rather
than a change to console-facing behavior and is outside this design's first implementation phase.

### 7.5 Face presentation

The existing Nintendo/Xbox face-layout preference applies to the Pro2 face diamond only. GameCube
and Joy-Con templates have personality-fixed labels and bindings. Letting the generic face-layout
preference rewrite them would make the drawing disagree with the target personality.

The durable invariant is: touching a control labeled `L` must produce personality output `L` after
the complete Android-report, firmware-seam, and personality-encoder path. Every gameplay
personality must declare each visible face label and bridge binding explicitly. A new personality
cannot inherit a default face map. Repository acceptance must fail if the set of registered visible
face controls differs from the set covered by cross-layer goldens. This applies to both Pro2
presentation choices and to the fixed GameCube and Joy-Con layouts.

## 8. Template and renderer design

Shipped templates remain Kotlin values in `:bridge-core`. This keeps actions and geometry typed,
lets tests instantiate them without Android, and avoids adding a runtime parser for first-party
defaults.

`TouchVisualSpec` contains platform-neutral roles such as:

- round labeled button;
- rectangular shoulder or trigger;
- unified D-pad;
- analog stick;
- GameCube large A, small B, bean X, and bean Y;
- Joy-Con direction or face button;
- rounded-square utility controls with a C legend, softly filled inset capture disc, or
  home-in-circle glyph.

The Android renderer maps those roles to Canvas paths, colors, typography, or app-owned vector
resources. It should continue drawing in one Canvas pass. A resource name may be resolved by the app,
but Android resource ids and local paths are never serialized into a portable layout.

All labeled roles share one typography target rather than carrying per-template font sizes. The
current renderer uses a 24sp semibold target, then shrinks only when required to keep the measured
legend inside 78% of the control width and 68% of its height. Short legends such as A, B, X, and Y
therefore retain the larger balanced size; longer shoulder or system legends remain centred,
single-line, and visibly clear of the outline. Capture and Home use code-native Canvas geometry
rather than text or redistributable image assets.

Visual bounds and hit bounds remain separate. Decorative art may be irregular while the hit target
stays simple, predictable, and auditable. A non-rectangular image must not introduce pixel-alpha hit
testing.

## 9. User customization and persistence

### 9.1 Effective layout

```text
immutable template at current revision
  + valid sparse overrides whose control ids still exist
  = effective authored layout
  -> resolve into the current interaction-safe region
  -> audit resolved geometry
```

Unknown override ids are retained in storage but ignored at runtime. New template controls use their
default geometry. This lets an application update add a control without discarding the user's other
placements.

### 9.2 Storage

Persist one override document per profile in app-private storage. Keep global Touch Gamepad settings
such as opacity, background dimming, haptics, and deadzone separate unless a later product decision
explicitly makes them profile-specific.

Storage rules:

- defaults are never written or mutated;
- current presses, contacts, stick vectors, D-pad state, and authority are never persisted;
- decoding rejects non-finite numbers, invalid scales, unknown schema versions, and profile/template
  mismatches;
- an unreadable or future-version override falls back to the shipped default without deleting the
  raw document;
- migrations are explicit and sequential;
- Restore Defaults deletes only the selected profile's override;
- replacing a template revision applies compatible overrides by stable control id, then audits the
  result.

### 9.3 Editor state

Editing is a distinct state from playing:

```text
Playing -> edge Back -> release all -> host Menu -> Editing draft
Editing draft -> validate -> Save -> install effective layout -> Playing
Editing draft -> Cancel -> discard draft -> previous effective layout
```

The touch gameplay router is disabled while editing. The editor operates on a draft, uses explicit
Save and Cancel, and keeps the host menu's exit affordance visible. The host menu and edge-Back
navigation belong to the app shell, so layout overrides cannot move, hide, or shadow them.

First-release operations:

- select a control or edit group;
- drag within the interaction-safe region;
- resize with bounded uniform scale;
- hide or show gameplay controls from a control list;
- reset one control, one group, or the entire current profile;
- preview hit bounds and blocking audit findings.

Shipped templates must contain the complete profile contract. A user may deliberately hide a
gameplay control; that produces an editor warning, not a runtime mapping change. Ambiguous hit
regions, undersized targets, invalid values, or out-of-bounds geometry remain blocking. Menu
reachability is validated at the Android navigation layer rather than by `TouchLayoutAudit`.

## 10. Personality selection and transitions

The app selects a profile from the latest confirmed management `Personality`. It must not infer the
USB personality from the Classic controller link or from a visual theme.

Entry behavior:

1. Read the current confirmed personality.
2. Reject `Config` and `Unknown` as gameplay profiles.
3. Load that profile's template and override.
4. Compose, resolve, and audit before enabling contacts.
5. Enter touch authority using the existing `AndroidBridge` path.

Transition behavior while Touch Gamepad is open:

1. Mark input temporarily unavailable.
2. Call the one release-all path with a personality-change reason while the transport is live.
3. Quarantine contacts already down; they cannot claim the new layout until lifted and pressed
   again.
4. Compose and audit the new profile.
5. Atomically install the new resolved layout and resume, or remain neutral with a clear error.

No user override is migrated between profiles. Switching profiles selects that profile's own stored
override. The Classic HID link remains connected; only the console-facing USB personality changes.

## 11. Validation and failure behavior

`TouchLayoutAudit` becomes profile-aware and distinguishes shipped-template validation from user
draft validation.

Always blocking:

- duplicate or unstable control ids;
- an output absent from the selected profile;
- a missing fixed binding;
- a face control whose selected personality lacks an explicit binding;
- non-finite or out-of-range values;
- overlapping hit regions;
- a target below the minimum touch size;
- visual or hit bounds outside the interaction-safe region;
- a resolved region too small to operate safely.

Blocking for shipped templates or repository acceptance, as applicable:

- any required profile output missing;
- any forbidden output present, including GameCube L3/R3;
- inconsistent profile/template ids or schema metadata;
- a gameplay `Personality` with no exactly matching `TouchControllerProfile`, or a profile selected
  through a default/fallback branch rather than exhaustive registration;
- any registered visible face-control tuple with no exactly matching cross-layer golden case, or a
  stale golden case that no longer corresponds to a registered visible control.

Warnings for user drafts:

- an intentionally hidden gameplay output;
- an override from an older template revision whose removed ids were ignored.

Runtime fallback order:

1. valid effective user layout;
2. immutable default template for the confirmed profile;
3. neutral disabled surface with the exact audit problem.

The engine never silently falls back to another personality.

## 12. Implementation sequence

### Phase 1 — Profile model with Pro2 parity

- Introduce profile ids, output contracts, fixed bindings, and a profile-aware audit.
- Express the current V1 layout as the Pro2 default template without changing its resolved geometry.
- Add parity tests that compare every V1 control id, action, position, size, and visual role.

### Phase 2 — Personality templates

- Add NSO GameCube, Joy-Con 2 Left, and Joy-Con 2 Right templates.
- Add renderer roles for GameCube shapes and Joy-Con clusters.
- Add end-to-end binding golden tests against the existing firmware mapping policy.
- Extend the debug layout lab to select every profile at arbitrary window sizes.

### Phase 3 — Overrides and editor

- Add the portable override model, composer, codec, and store interface.
- Add the Android app-private store and explicit editor state machine.
- Implement move, uniform resize, group edit, hide/show, Save/Cancel, and Restore Defaults.
- Add schema and template-revision migration tests.

### Phase 4 — Hardware acceptance

- Run the physical matrix in section 13.
- Update current-state documentation only after evidence exists.
- Keep any failed or revised interaction hypothesis in a dated experiment record rather than in
  `STATUS.md`.

## 13. Acceptance and test strategy

### 13.1 Portable JVM tests

- every shipped profile has one valid default template;
- the gameplay personalities are exhaustively equal to the profile catalog keys, excluding only
  `Config` and `Unknown`, so adding a personality without a touch profile fails immediately;
- every supported output has exactly one fixed binding and forbidden outputs are absent;
- all templates resolve and audit at the existing window/density probes;
- Pro2 resolved geometry remains unchanged in Phase 1;
- Joy-Con direction/face buttons are independent and support multi-contact chords;
- both Joy-Con profiles use the generic left-stick and L3 source policy;
- GameCube exposes Z, ZL, full-pull L/R and detents while never exposing L3/R3;
- override merge, reset, corrupt-data fallback, unknown ids, schema migration, and template revision
  changes;
- profile replacement releases all held state and a held contact cannot resume across it;
- hidden controls contribute no hit region and no input.

### 13.2 Cross-layer mapping tests

For every visible face control in every gameplay personality, including both Pro2 Nintendo/Xbox
presentations and the personality-fixed GameCube/Joy-Con layouts:

1. assert the rendered label against the profile's expected face record;
2. produce the corresponding Kotlin `ControllerState` and exact Android HID usage;
3. feed the report through the real firmware parser and the bridge-aware seam while separately
   proving the locked direct-controller base map is unchanged;
4. feed both the corrected canonical state and unchanged raw JP source bitmap into the selected
   Pro2, GameCube, Joy-Con Left, or Joy-Con Right encoder;
5. assert the exact final personality output bit and assert every unrelated face/output field is
   neutral.

These goldens are the guard against a correct-looking layout whose labels drive the wrong output.
An assertion that stops at Kotlin state or HID usage is explicitly incomplete. The same exhaustive
matrix is a required acceptance gate whenever a future personality is registered. CI compares the
set of golden keys `(personality, template, presentation, control id)` for exact equality with the
catalog's visible face-control keys, so adding or removing a personality/control cannot silently
fall through a default or escape final-encoder coverage.

The implemented Pro2 baseline currently has paired app-side and production-parser/seam-resolver
coverage, not one executable that reaches a final personality report. Phase 2 must add the full
goldens above before the profile catalog accepts GameCube, either Joy-Con, or any later personality;
the paired baseline tests are not a waiver for that gate.

### 13.3 Android tests

- profile selection from management state, including `Unknown` and `Config` refusal;
- separate per-profile persistence and Restore Defaults;
- editor Play/Edit isolation, Save/Cancel, and lifecycle release;
- renderer coverage for each visual role;
- layout-lab rendering at representative safe areas, densities, font scales, and gesture insets;
- instrumented multi-pointer routing after profile and geometry changes;
- debug and release APK assembly plus Android lint.

Use [`tools/run_android_tests.ps1`](tools/run_android_tests.ps1) as the entry point for the existing
JVM suites and run the relevant firmware host-test groups through
[`tools/run_host_tests.ps1`](tools/run_host_tests.ps1). Report software validation separately from
hardware validation.

### 13.4 Physical hardware matrix

On an Android handheld, PicoSwitch2, and a real Switch 2:

- enter each of the four controller personalities and verify the matching template appears;
- verify every visible control in ordinary gameplay, including simultaneous stick/button/trigger
  chords;
- verify Joy-Con sideways directions, stick rotation, SL/SR, and side-specific system buttons;
- verify GameCube Z/ZL, full-scale L/R values and detents, and the absence of L3/R3;
- switch personality while contacts are held and confirm one neutral transition with no stuck state
  and no Classic HID disconnect;
- background, foreground, rotate, resize, open editor, cancel editor, save editor, drop the link, and
  reconnect while holding inputs;
- verify local touch haptics and console rumble remain separate;
- perform a stuck-input torture pass and real-game correctness pass.

Until this matrix passes, new profiles are source-tested, not hardware-confirmed.

## 14. Documentation impact

When implemented:

- update [`android/companion/README.md`](android/companion/README.md) with the profile and editor
  behavior;
- update [`android/companion/FEATURE_PARITY.md`](android/companion/FEATURE_PARITY.md) with software and
  physical validation levels;
- replace the completed Touch Gamepad roadmap state in [`PLAN.md`](PLAN.md) rather than appending an
  implementation diary;
- update [`STATUS.md`](STATUS.md) only with current validated behavior;
- add a dated experiment record only if hardware testing resolves an unknown or rejects a layout or
  mapping hypothesis.

## 15. Remaining decisions before implementation

1. Decide whether user overrides are local to one physical display class or shared across all
   Android devices. The initial store should assume local-only unless synchronization becomes a real
   product requirement.
2. Decide whether hiding a gameplay control should show a persistent in-game indicator. The safety
   requirement is only that the app-shell edge gesture keeps Menu and Restore Defaults reachable.

None of these decisions blocks Phase 1 profile modeling or Pro2 parity work.
