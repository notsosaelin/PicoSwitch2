using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The profile picker's state machine — the crash this page shipped with.
/// </summary>
/// <remarks>
/// Windows Error Reporting recorded <c>PicoSwitch.Companion.App.exe</c> faulting
/// in <c>Microsoft.UI.Xaml.dll</c>, exception <c>0xc000027b</c>
/// (STATUS_STOWED_EXCEPTION), in two buckets: <c>0x8000ffff</c> (E_UNEXPECTED)
/// and <c>0x80070490</c> (ERROR_NOT_FOUND).
///
/// The page rebuilt the picker's items on EVERY render and read the current
/// selection back out of the <c>ComboBoxItem</c> objects it had just created.
/// Renders arrive asynchronously — from a dispatcher-enqueued state callback and
/// from every command's completion — so the collection could be torn down while
/// the control still referenced it. Saving a profile fires a state change, and
/// the user's very next act in the reported sequence is to open that picker.
///
/// These tests drive the reported sequences against the extracted state machine.
/// They would have failed on the old design, which had no way to answer "do the
/// items actually need replacing?" at all — it always replaced them.
/// </remarks>
public sealed class KbmProfileSelectionTests
{
    private const int Work = 2;
    private const int Halo = 3;

    // Builtin is derived from the id (only Default is built in), so it is never
    // passed: the row for KbmProfileIds.Default IS the built-in one.
    private static KbmProfileInfo Row(int id, string name, bool builtin = false,
                                      int revision = 1, long fingerprint = 100) =>
        new(builtin ? KbmProfileIds.Default : id, KbmLayout.Keyboard, name, revision,
            Overrides: 0, Fingerprint: fingerprint);

    private static IReadOnlyList<KbmProfileInfo> Library(params KbmProfileInfo[] rows) => rows;

    private static IReadOnlyList<KbmProfileInfo> Standard() => Library(
        Row(KbmProfileIds.Default, "Default", builtin: true),
        Row(Work, "Work"),
        Row(Halo, "Halo"));

    /* --------------------------------------------------- the crash sequences */

    [Fact]
    public void SavingAProfileDoesNotForceThePickerToBeRebuilt()
    {
        // THE CRASH, at its source. Save bumps the revision and the fingerprint,
        // which fires a state change, which enqueues a render. The old page
        // answered that render by clearing and repopulating the ComboBox — while
        // the user was reaching for it to select Default.
        //
        // Nothing the picker DISPLAYS changed, so the collection must be left
        // alone and the control keeps the item its selection already points at.
        var before = Standard();
        var afterSave = Library(
            Row(KbmProfileIds.Default, "Default", builtin: true),
            Row(Work, "Work", revision: 2, fingerprint: 999),
            Row(Halo, "Halo"));

        var plan = KbmProfileSelection.Plan(before, afterSave, Work);

        Assert.False(plan.Rebuild);
        Assert.Equal(Work, plan.SelectedId);
        Assert.Equal(1, plan.SelectedIndex);
    }

    [Fact]
    public void TheFullReportedSequenceKeepsSelectionCoherentThroughout()
    {
        // select custom -> edit -> Save -> select Default.
        var rows = Standard();
        var rendered = (IReadOnlyList<KbmProfileInfo>)[];

        // First render: the picker is empty, so it genuinely must be built.
        var open = KbmProfileSelection.Plan(rendered, rows, KbmProfileIds.Default);
        Assert.True(open.Rebuild);
        rendered = open.Rows;

        // Select the custom profile.
        Assert.Equal(KbmSelectionAction.Open,
                     KbmProfileSelection.Decide(Work, KbmProfileIds.Default,
                                                draftDirty: false, suppressed: false));

        // Edit it: renders happen, nothing displayed changes.
        var editing = KbmProfileSelection.Plan(rendered, rows, Work);
        Assert.False(editing.Rebuild);
        Assert.Equal(Work, editing.SelectedId);

        // Save: revision moves, and the render that follows must not rebuild.
        var saved = Library(rows[0], Row(Work, "Work", revision: 7, fingerprint: 42), rows[2]);
        var afterSave = KbmProfileSelection.Plan(rendered, saved, Work);
        Assert.False(afterSave.Rebuild);
        rendered = afterSave.Rows;

        // Select Default. The draft is clean after a save, so this opens directly.
        Assert.Equal(KbmSelectionAction.Open,
                     KbmProfileSelection.Decide(KbmProfileIds.Default, Work,
                                                draftDirty: false, suppressed: false));

        var back = KbmProfileSelection.Plan(rendered, saved, KbmProfileIds.Default);
        Assert.False(back.Rebuild);
        Assert.Equal(KbmProfileIds.Default, back.SelectedId);
        Assert.Equal(0, back.SelectedIndex);
    }

    [Fact]
    public void CustomSaveDefaultCustomRepeatedStaysStable()
    {
        // The same loop several times, which is how the user reported hitting it
        // ("sometimes"). No iteration may require a rebuild once the rows exist.
        var rows = Standard();
        var rendered = KbmProfileSelection.Plan([], rows, KbmProfileIds.Default).Rows;

        for (var i = 0; i < 5; i++)
        {
            var revised = Library(rows[0], Row(Work, "Work", revision: i + 2), rows[2]);

            var custom = KbmProfileSelection.Plan(rendered, revised, Work);
            Assert.False(custom.Rebuild);
            Assert.Equal(Work, custom.SelectedId);

            var def = KbmProfileSelection.Plan(rendered, revised, KbmProfileIds.Default);
            Assert.False(def.Rebuild);
            Assert.Equal(KbmProfileIds.Default, def.SelectedId);
        }
    }

    [Fact]
    public void ARenameIsTheOneEditThatDoesRequireARebuild()
    {
        // The picker shows the name, so a rename must replace the items. This is
        // the case the reconciliation must NOT optimize away, or the list would
        // show a stale name forever.
        var rows = Standard();
        var renamed = Library(rows[0], Row(Work, "Work Mk II"), rows[2]);

        var plan = KbmProfileSelection.Plan(rows, renamed, Work);

        Assert.True(plan.Rebuild);
        Assert.Equal(Work, plan.SelectedId);
        Assert.Equal(1, plan.SelectedIndex);
    }

    [Fact]
    public void DeletingTheOpenProfileFallsBackRatherThanSelectingNothing()
    {
        // The selection's target no longer exists. A -1 selection is what left the
        // picker blank and the profile workflow looking absent; falling back to
        // the first row (always the built-in Default) keeps the page coherent.
        var rows = Standard();
        var afterDelete = Library(rows[0], rows[2]);

        var plan = KbmProfileSelection.Plan(rows, afterDelete, Work);

        Assert.True(plan.Rebuild);
        Assert.Equal(KbmProfileIds.Default, plan.SelectedId);
        Assert.Equal(0, plan.SelectedIndex);
    }

    [Fact]
    public void AProfileAddedByCreateOrDuplicateRebuildsAndCanBeSelected()
    {
        var rows = Standard();
        var added = Library(rows[0], rows[1], rows[2], Row(9, "Zelda"));

        var plan = KbmProfileSelection.Plan(rows, added, 9);

        Assert.True(plan.Rebuild);
        Assert.Equal(9, plan.SelectedId);
        Assert.Equal(3, plan.SelectedIndex);
    }

    [Fact]
    public void AnExternalRefreshWhileAProfileIsOpenKeepsThatProfileOpen()
    {
        // A reload lands with the same library. Selection must survive it: a
        // render that re-asserted a different profile would silently discard the
        // user's draft.
        var rows = Standard();
        var reloaded = Standard();  // equal content, different instances

        var plan = KbmProfileSelection.Plan(rows, reloaded, Work);

        Assert.False(plan.Rebuild);
        Assert.Equal(Work, plan.SelectedId);
    }

    [Fact]
    public void ADisconnectEmptiesThePickerWithoutClaimingAnySelection()
    {
        var plan = KbmProfileSelection.Plan(Standard(), [], Work);

        Assert.True(plan.Rebuild);
        Assert.Equal(KbmProfileIds.None, plan.SelectedId);
        Assert.Equal(-1, plan.SelectedIndex);
    }

    /* ------------------------------------------------- the selection decision */

    [Fact]
    public void ReSelectingTheOpenProfileIsNotATransition()
    {
        // A render re-asserts SelectedIndex, which raises SelectionChanged. If
        // that reopened the profile it would reload from the adapter and throw
        // away the user's unsaved edits on every repaint.
        Assert.Equal(KbmSelectionAction.Ignore,
                     KbmProfileSelection.Decide(Work, Work, draftDirty: true,
                                                suppressed: false));
    }

    [Fact]
    public void AProgrammaticRepopulationIsSuppressed()
    {
        Assert.Equal(KbmSelectionAction.Ignore,
                     KbmProfileSelection.Decide(Halo, Work, draftDirty: false,
                                                suppressed: true));
    }

    [Fact]
    public void AnEmptySelectionIsNotATransition()
    {
        // Clearing the items drives SelectedIndex to -1 and raises the event.
        Assert.Equal(KbmSelectionAction.Ignore,
                     KbmProfileSelection.Decide(KbmProfileIds.None, Work,
                                                draftDirty: false, suppressed: false));
    }

    [Fact]
    public void LeavingADirtyDraftAsksFirst()
    {
        Assert.Equal(KbmSelectionAction.ConfirmDiscard,
                     KbmProfileSelection.Decide(Halo, Work, draftDirty: true,
                                                suppressed: false));

        Assert.Equal(KbmSelectionAction.Open,
                     KbmProfileSelection.Decide(Halo, Work, draftDirty: false,
                                                suppressed: false));
    }

    [Fact]
    public void TheDecisionIsMadeFromArgumentsAloneSoItCannotRaceAnAwait()
    {
        // The old handler read the draft, awaited a confirmation dialog, and then
        // acted on state that could have been replaced while it was suspended.
        // Decide() is pure: the same inputs always produce the same action, so the
        // transition is atomic with respect to whatever the caller awaits.
        var first = KbmProfileSelection.Decide(Halo, Work, true, false);
        var second = KbmProfileSelection.Decide(Halo, Work, true, false);
        Assert.Equal(first, second);
        Assert.Equal(KbmSelectionAction.ConfirmDiscard, first);
    }
}
