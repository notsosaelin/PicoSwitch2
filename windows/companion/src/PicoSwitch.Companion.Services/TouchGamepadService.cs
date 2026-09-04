using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Everything the Touch Gamepad surface needs, in one observable value.
///
/// A single snapshot rather than six separate observables: the page draws the resolved
/// layout, the profile picker and the audit banner from ONE state, and three properties
/// that can be read a frame apart are three ways for the canvas to disagree with the
/// validator about the same layout.
/// </summary>
public sealed record TouchGamepadState
{
    /// <summary>
    /// The personality the adapter CONFIRMED, or null when nothing has said.
    ///
    /// Null is a real state and not a default: it means the surface has not been told
    /// which controller it is drawing, which is different from being told Pro Controller
    /// 2. Guessing would show the user a layout for hardware they are not emulating.
    /// </summary>
    public TouchProfileId? Personality { get; init; }

    public TouchProfileLibrary Library { get; init; } =
        TouchProfileLibrary.Empty(TouchProfileId.Pro2);

    /// <summary>The document being edited, which is the selected profile's until it is saved.</summary>
    public TouchLayoutDocument Document { get; init; } =
        TouchLayoutDocument.AuthoredDefault(
            TouchProfileCatalog.Require(TouchProfileId.Pro2));

    /// <summary>Real geometry for the region last measured, audited.</summary>
    public ResolvedTouchLayout Resolved { get; init; } = ResolvedTouchLayout.Empty;

    /// <summary>Unsaved edits exist.</summary>
    public bool Dirty { get; init; }

    public bool CanUndo { get; init; }

    public bool CanRedo { get; init; }

    public IReadOnlySet<string> Selection { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    public TouchAlignmentSettings Alignment { get; init; } = TouchAlignmentSettings.Off;

    /// <summary>
    /// The personality came from the registry rather than from a live adapter.
    ///
    /// Worth a sentence to the user: it is what the adapter reported at the last verified
    /// connection, so it can be out of date. Everything here is local, so a stale answer
    /// costs a wrong-looking layout and never a wrong button on a console.
    /// </summary>
    public bool PersonalityRemembered { get; init; }

    /// <summary>
    /// What to tell the user, when there is something.
    ///
    /// One field rather than several because only one sentence fits, and the priority is
    /// fixed: a storage warning outranks a composition warning outranks an audit finding.
    /// </summary>
    public string? Warning { get; init; }

    /// <summary>
    /// Whether the surface may be edited at all.
    ///
    /// False only when the window is genuinely too small: no EDIT can clear that, so
    /// offering the editor there would be offering a repair that cannot work.
    /// </summary>
    public bool Editable => Personality is not null && !Resolved.RegionTooSmall;
}

/// <summary>
/// The Touch Gamepad, as the app sees it.
///
/// ## What this owns, and what it does not
///
/// It owns the STATE: which personality is confirmed, which profile library is loaded,
/// which document is being edited, the undo history, and the resolved geometry for the
/// region the surface last measured. Every rule about what an edit means lives in
/// <see cref="TouchLayoutEditor"/> and <see cref="TouchProfileLibraryEditor"/>, in the
/// portable core, where it is shared with the Android companion and tested without a UI.
///
/// Gameplay contacts enter the shared <see cref="ControllerInputSession"/> here,
/// after the WinUI surface has reduced pointer events to platform-neutral contacts.
/// The surface still opens and edits with no adapter attached; only activating its
/// gameplay state affects Controller Link input.
///
/// ## Personality
///
/// The confirmed personality comes from the adapter snapshot and is never guessed. When
/// it changes the library is reloaded and the editor is reset, because a layout for one
/// controller has no meaning for another.
/// </summary>
public sealed class TouchGamepadService
{
    private readonly ITouchProfileLibraryStore profiles;
    private readonly ITouchLayoutOverrideStore legacy;
    private readonly DiagnosticLog diagnostics;
    private readonly Func<long> nowEpochMs;
    private readonly ControllerInputSession? controllerInput;
    private readonly StateValue<TouchGamepadState> state = new(new TouchGamepadState());

    private TouchEditorHistory? history;
    private TouchLayoutRegion region;
    private string? storageWarning;

    public TouchGamepadService(
        ITouchProfileLibraryStore profiles,
        ITouchLayoutOverrideStore legacy,
        DiagnosticLog diagnostics,
        Func<long>? nowEpochMs = null,
        ControllerInputSession? controllerInput = null)
    {
        this.profiles = profiles;
        this.legacy = legacy;
        this.diagnostics = diagnostics;
        this.controllerInput = controllerInput;
        this.nowEpochMs = nowEpochMs ??
            (() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
    }

    public IReadOnlyStateValue<TouchGamepadState> State => state;

    public void ActivateGameplay()
    {
        controllerInput?.SetTouchLayout(state.Value.Resolved);
        controllerInput?.ActivateTouch();
        diagnostics.Info("touch", "gameplay input activated");
    }

    public void DeactivateGameplay()
    {
        controllerInput?.DeactivateTouch();
        diagnostics.Info("touch", "gameplay input deactivated");
    }

    public void DispatchGameplayContacts(IReadOnlyList<TouchContact> contacts) =>
        controllerInput?.DispatchTouchContacts(contacts);

    public void TickGameplay(long nowNanos) => controllerInput?.TickTouch(nowNanos);

    public void ReleaseGameplay(TouchReleaseReason reason) => controllerInput?.ReleaseTouch(reason);

    /// <summary>
    /// What the touch engine is doing right now.
    /// </summary>
    /// <remarks>
    /// The surface draws presses, latches, trigger travel and stick deflection
    /// from this rather than from the pointer events it dispatched, so the picture
    /// and the input the console receives cannot disagree.
    ///
    /// A service with no controller input (the layout lab, and every test that
    /// builds one) reports an empty snapshot rather than refusing: an editor with
    /// nothing held is exactly right.
    /// </remarks>
    public TouchDiagnosticsSnapshot Diagnostics() =>
        controllerInput?.TouchDiagnostics() ?? new TouchDiagnosticsSnapshot();

    /// <summary>
    /// The personality the adapter confirmed. Idempotent.
    ///
    /// Reloads the library and discards the editor's history, because an undo step from
    /// one controller's layout cannot be applied to another's.
    /// </summary>
    public void SetPersonality(Personality personality, string? lastConfirmedWireName = null)
    {
        var mapped = TouchProfileSelector.SelectOrRemembered(personality, lastConfirmedWireName);
        var remembered = mapped is not null && TouchProfileSelector.Select(personality) is null;

        if (mapped == state.Value.Personality)
        {
            // Same controller: only how we KNOW that has changed — an adapter connected
            // while the user was editing, say. Reloading here would discard their unsaved
            // work as a side effect of plugging something in.
            if (remembered != state.Value.PersonalityRemembered)
            {
                state.Set(state.Value with { PersonalityRemembered = remembered });
            }

            return;
        }

        if (mapped is null)
        {
            // A personality with no touch layout — or none confirmed yet. Say nothing
            // rather than drawing the last controller's arrangement.
            history = null;
            controllerInput?.ReleaseTouch(TouchReleaseReason.PersonalityChanged);
            state.Set(new TouchGamepadState());
            return;
        }

        Load(mapped.Value, remembered);
    }

    /// <summary>
    /// The interaction-safe rectangle changed: a resize, a DPI change, an inset.
    ///
    /// Re-resolves rather than scaling what was already resolved. Every control's
    /// position is a function of the rectangle, and the audit's answer is a function of
    /// the resulting geometry, so a cached resolve is a layout that says it fits a window
    /// it was never measured against.
    /// </summary>
    public void SetRegion(TouchLayoutRegion next)
    {
        region = next;
        Publish(state.Value.Document);
    }

    // ------------------------------------------------------------------- profiles

    public void SelectProfile(string profileId) =>
        ApplyLibrary(TouchProfileLibraryEditor.Select(state.Value.Library, profileId), adopt: true);

    public void CreateProfile(string name) =>
        ApplyLibrary(
            TouchProfileLibraryEditor.Create(state.Value.Library, name, nowEpochMs()),
            adopt: true);

    public void DuplicateProfile(string profileId, string? name = null) =>
        ApplyLibrary(
            TouchProfileLibraryEditor.Duplicate(state.Value.Library, profileId, nowEpochMs(), name),
            adopt: true);

    public void RenameProfile(string profileId, string name) =>
        ApplyLibrary(
            TouchProfileLibraryEditor.Rename(state.Value.Library, profileId, name), adopt: false);

    public void DeleteProfile(string profileId) =>
        ApplyLibrary(TouchProfileLibraryEditor.Delete(state.Value.Library, profileId), adopt: true);

    /// <summary>
    /// Store the edited document into the selected profile.
    ///
    /// Saving onto the factory profile creates a new one instead — the core decides that,
    /// not this service, because it is a rule about the user's data rather than about
    /// this platform's UI.
    /// </summary>
    public void Save()
    {
        var current = state.Value;
        ApplyLibrary(
            TouchProfileLibraryEditor.Save(
                current.Library, current.Library.SelectedProfileId, current.Document, nowEpochMs()),
            adopt: true);
    }

    /// <summary>Throw the unsaved edits away and go back to what is stored.</summary>
    public void Discard() => Adopt(state.Value.Library.Selected.Document);

    public void ResetToDefault() =>
        ApplyLibrary(
            TouchProfileLibraryEditor.ResetToDefault(
                state.Value.Library, state.Value.Library.SelectedProfileId, nowEpochMs()),
            adopt: true);

    public string? Export(string profileId) =>
        state.Value.Library.Profile(profileId) is { } profile
            ? TouchProfileLibraryJsonCodec.EncodeExport(profile)
            : null;

    /// <summary>Adopt a shared layout file. Returns the refusal, or null on success.</summary>
    public string? Import(string raw)
    {
        switch (TouchProfileLibraryJsonCodec.DecodeExport(raw))
        {
            case TouchProfileDecodeResult.Invalid invalid:
                diagnostics.Warn("touch", $"layout import refused: {invalid.Problem}");
                return invalid.Problem;

            case TouchProfileDecodeResult.Valid valid:
            {
                var edit = TouchProfileLibraryEditor.Import(
                    state.Value.Library, valid.Value, nowEpochMs());
                ApplyLibrary(edit, adopt: true);
                return edit is TouchProfileEdit.Rejected rejected ? rejected.Reason : null;
            }

            default:
                return "That layout file could not be read";
        }
    }

    // --------------------------------------------------------------------- editing

    public void SetSelection(IReadOnlySet<string> selection) =>
        state.Set(state.Value with { Selection = selection });

    public void SetAlignment(TouchAlignmentSettings alignment) =>
        state.Set(state.Value with { Alignment = alignment });

    /// <summary>
    /// Apply one editor operation and record it as a single undo step.
    ///
    /// The label is what the undo menu says. Push happens once per COMPLETED gesture, so
    /// a drag that produced fifty intermediate documents is one step; the caller decides
    /// when a gesture ended by choosing when to call this.
    /// </summary>
    public void Edit(string label, Func<TouchLayoutDocument, TouchLayoutDocument> operation)
    {
        if (history is null)
        {
            return;
        }

        var next = operation(state.Value.Document);
        history.Push(next, label);
        Publish(history.Current);
    }

    /// <summary>
    /// The same, for an operation that can refuse.
    ///
    /// A refusal is surfaced and changes nothing — including the undo stack, because an
    /// undo step for an operation that did not happen is a step that appears to do
    /// nothing.
    /// </summary>
    public string? Edit(string label, Func<TouchLayoutDocument, TouchEditResult> operation)
    {
        if (history is null)
        {
            return null;
        }

        var result = operation(state.Value.Document);
        if (!result.Changed)
        {
            state.Set(state.Value with { Warning = result.Refusal });
            return result.Refusal;
        }

        history.Push(result.Document, label);
        Publish(history.Current, selection: result.Created.Count > 0
            ? result.Created.ToHashSet(StringComparer.Ordinal)
            : null);
        return null;
    }

    /// <summary>
    /// A live gesture's intermediate document: published, but never pushed.
    ///
    /// The working document is authoritative during a drag and only the endpoints are
    /// worth remembering, so this keeps the canvas following the finger without filling
    /// the undo stack with one step per pointer frame.
    /// </summary>
    public void Preview(TouchLayoutDocument document) => Publish(document);

    /// <summary>
    /// Turn everything <see cref="Preview"/> has shown into ONE undo step.
    ///
    /// The counterpart of the preview rule above: a gesture ends when the caller says it
    /// ends, and what it produced is remembered as the single thing the user did. A
    /// commit that would record nothing — a press with no movement, a drag that came back
    /// to where it started — is dropped, because an undo step that appears to do nothing
    /// is worse than no step at all.
    /// </summary>
    public void Commit(string label)
    {
        var document = state.Value.Document;
        if (history is null || document.Equals(history.Current))
        {
            return;
        }

        history.Push(document, label);
        Publish(document);
    }

    public void Undo()
    {
        if (history?.Undo() is { } document)
        {
            Publish(document);
        }
    }

    public void Redo()
    {
        if (history?.Redo() is { } document)
        {
            Publish(document);
        }
    }

    // -------------------------------------------------------------------- internals

    private void Load(TouchProfileId personality, bool remembered)
    {
        var loaded = profiles.Load(personality);
        var library = loaded.Library;
        storageWarning = loaded.Warning;

        // ONE-TIME legacy adoption, and only when nothing has been stored yet. A library
        // document that already exists means the upgrade has happened, and re-adopting
        // would add a second copy of the same layout on every launch.
        if (library.UserProfiles.Count == 0 && loaded.Warning is null &&
            legacy.Load(personality) is TouchOverrideDecodeResult.Valid stored)
        {
            var adopted = TouchProfileLibraryEditor.AdoptLegacyOverride(
                personality, stored.Value, nowEpochMs());

            if (adopted.UserProfiles.Count > 0)
            {
                library = adopted;
                profiles.Save(library);
                diagnostics.Info(
                    "touch",
                    $"adopted a pre-2.0 {personality.Key()} layout as '{library.UserProfiles[0].Name}'");
            }
        }

        history = new TouchEditorHistory(library.Selected.Document);

        state.Set(new TouchGamepadState
        {
            Personality = personality,
            PersonalityRemembered = remembered,
            Library = library,
            Document = library.Selected.Document,
            Alignment = state.Value.Alignment,
        });

        Publish(library.Selected.Document);
    }

    private void ApplyLibrary(TouchProfileEdit edit, bool adopt)
    {
        switch (edit)
        {
            case TouchProfileEdit.Rejected rejected:
                diagnostics.Warn("touch", $"layout profile edit refused: {rejected.Reason}");
                state.Set(state.Value with { Warning = rejected.Reason });
                return;

            case TouchProfileEdit.Applied applied:
                profiles.Save(applied.Library);
                state.Set(state.Value with { Library = applied.Library });
                if (adopt)
                {
                    Adopt(applied.Library.Selected.Document);
                }
                else
                {
                    Publish(state.Value.Document);
                }

                return;
        }
    }

    /// <summary>Take a stored document as the new editing baseline, discarding the history.</summary>
    private void Adopt(TouchLayoutDocument document)
    {
        history = new TouchEditorHistory(document);
        Publish(document, selection: new HashSet<string>(StringComparer.Ordinal));
    }

    /// <summary>
    /// Compose, resolve, audit and publish, in that order.
    ///
    /// One place, so the canvas, the audit banner and the "can this be played" verdict
    /// are always three views of the same computation. Recomputing separately is what
    /// let a canvas say a control fits while the validator refused it.
    /// </summary>
    private void Publish(TouchLayoutDocument document, IReadOnlySet<string>? selection = null)
    {
        var current = state.Value;
        if (current.Personality is not { } personality)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        var composed = TouchLayoutComposer.Compose(profile, document);

        // UserDraft: hiding an output is the user's business while they edit, but unsafe
        // geometry still has to block. The shipped-template mode would refuse a layout for
        // a control the user deliberately deleted.
        var resolved = TouchLayoutResolver.Resolve(
            composed.Layout, region, TouchLayoutAuditMode.UserDraft);

        state.Set(current with
        {
            Document = document,
            Resolved = resolved,
            Dirty = !document.Equals(current.Library.Selected.Document),
            CanUndo = history?.CanUndo ?? false,
            CanRedo = history?.CanRedo ?? false,
            Selection = selection ?? current.Selection,

            // Fixed priority: what storage said, then what composition said, then the
            // first blocking audit finding. Only one sentence fits.
            Warning = storageWarning ?? composed.Warning ?? resolved.Problem,
        });
        controllerInput?.SetTouchLayout(resolved);
    }
}
