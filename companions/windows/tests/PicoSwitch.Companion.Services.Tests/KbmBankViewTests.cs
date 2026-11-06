using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The bank projection and the three-way divergence model.
/// </summary>
/// <remarks>
/// The state a single <c>matchesSaved</c> boolean could not express: "I edited
/// this locally, the adapter still holds the old copy, and it is running that old
/// copy." That is the ORDINARY state after a local Save, and telling the user
/// otherwise is what made Save feel like it had silently failed.
/// </remarks>
public sealed class KbmBankViewTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-bank-" + Guid.NewGuid().ToString("N"));

    private KbmLibraryRepository Library() =>
        new(new KbmProfileLibraryStore(new WindowsDocumentStore(root)));

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
        catch (IOException)
        {
        }
    }

    private static KbmBinding Bind(int usage, KbmDestination destination) =>
        new(new KbmSource(KbmSourceKind.Key, usage), destination, Custom: true);

    private static KbmProfileInfo Resident(int id, int position, string name,
                                           long fingerprint,
                                           KbmLayout layout = KbmLayout.Keyboard) =>
        new(id, layout, name, Revision: 1, Overrides: 1, Fingerprint: fingerprint,
            Position: position);

    private static KeyboardMouseState State(
        IEnumerable<KbmProfileInfo>? residents = null,
        int runtimePosition = KbmPositions.Default,
        int bootPosition = KbmPositions.Default,
        bool matchesSaved = true,
        IEnumerable<KbmSwitchBinding>? switches = null) => new()
    {
        Readiness = KeyboardMouseReadiness.Ready,
        Capability = CapabilityState.Available,
        Profiles = new KbmProfiles(
            new ValueList<KbmProfileInfo>(residents ?? []),
            new ValueList<KbmActiveMapping>(
            [
                new KbmActiveMapping(KbmLayout.Keyboard, SourceId: 2, Revision: 1,
                                     Fingerprint: 0, MatchesSaved: matchesSaved,
                                     BootPosition: bootPosition,
                                     RuntimePosition: runtimePosition),
                new KbmActiveMapping(KbmLayout.KeyboardMouse, KbmProfileIds.Default,
                                     0, 0, true),
            ]),
            Max: KbmLimits.MaxProfiles),
        Switches = new ValueList<KbmSwitchBinding>(switches ?? []),
    };

    /* ---------------------------------------------------------------- bank */

    [Fact]
    public void TheBankShowsEveryPositionIncludingEmptyOnes()
    {
        // "Profile 3 — Empty" is what tells a user they have somewhere to assign
        // to. A list of only occupied positions would hide the capacity.
        var bank = KbmBankView.Bank(
            State([Resident(2, 1, "Halo", 111)]), KbmLayout.Keyboard);

        Assert.Equal(KbmLimits.PositionsPerLayout + 1, bank.Count);
        Assert.Equal("Default", bank[0].PositionLabel);
        Assert.Equal("Halo", bank[1].ResidentLabel);
        Assert.True(bank[2].Empty);
        Assert.Equal("Empty", bank[2].ResidentLabel);
        Assert.Equal("Profile 3", bank[3].PositionLabel);
    }

    [Fact]
    public void TheBankMarksRuntimeAndBootSeparately()
    {
        // A switch key moves runtime and not boot, so after one press the two
        // differ for the rest of the session. Showing only one would report the
        // wrong profile as active.
        var bank = KbmBankView.Bank(
            State([Resident(2, 1, "Halo", 111), Resident(3, 2, "Zelda", 222)],
                  runtimePosition: 2, bootPosition: 1),
            KbmLayout.Keyboard);

        Assert.True(bank[1].IsBoot);
        Assert.False(bank[1].IsRuntime);
        Assert.True(bank[2].IsRuntime);
        Assert.False(bank[2].IsBoot);
    }

    [Fact]
    public void ASwitchKeyIsShownAgainstThePositionItSelectsInEitherLayout()
    {
        // One table serves both banks, so the same key appears against Profile 1
        // of the Keyboard bank and of the Keyboard+Mouse bank. That is the point:
        // it selects the layout-appropriate profile.
        var key = new KbmSource(KbmSourceKind.Key, 0x3B);
        var state = State([Resident(2, 1, "Halo", 111)],
                          switches: [new KbmSwitchBinding(key, 1)]);

        Assert.Equal(key, KbmBankView.Bank(state, KbmLayout.Keyboard)[1].SwitchKey);
        Assert.Equal(key,
                     KbmBankView.Bank(state, KbmLayout.KeyboardMouse)[1].SwitchKey);
    }

    [Fact]
    public void EveryswitchActionIsOfferedEvenWhenUnbound()
    {
        // Built from the ACTIONS, not from the bindings: a list of bindings alone
        // would hide the actions the user has not set up yet.
        var actions = KbmBankView.SwitchActions(State());

        Assert.Equal(KbmLimits.PositionsPerLayout + 1, actions.Count);
        Assert.All(actions, action => Assert.Null(action.Key));
        Assert.Equal(KbmPositions.Default, actions[0].Position);
    }

    /* ----------------------------------------------------------- divergence */

    [Fact]
    public void AProfileNeverSentToTheAdapterIsLocalOnly()
    {
        var library = Library();
        library.Create(KbmLayout.Keyboard, "Coding", [Bind(0x04, KbmDestination.A)]);

        var rows = KbmBankView.Library(library.Value, State(), KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.LocalOnly, Assert.Single(rows).State);
        Assert.Null(rows[0].AssignedPosition);
        Assert.False(rows[0].CanUpdateAdapterCopy);
    }

    [Fact]
    public void LocalSaveMakesTheAdapterCopyOutOfDateAndOffersAnUpdate()
    {
        // THE CENTRAL CASE. Save is local; the adapter keeps the older copy and
        // goes on running it. The row has to say so, and offer the explicit
        // action that resolves it.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo",
                                     [Bind(0x04, KbmDestination.Zr)]);
        var state = State([Resident(2, 1, "Halo", created.Fingerprint)],
                          runtimePosition: 1);

        // Agreement first.
        var before = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);
        Assert.Equal(KbmLocalState.Active, before[0].State);
        Assert.False(before[0].CanUpdateAdapterCopy);

        // A local edit, saved locally. The adapter is untouched.
        library.Save(created.Id, created.Name, [Bind(0x04, KbmDestination.A)],
                     created.Mouse);

        var after = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);
        Assert.Equal(KbmLocalState.AdapterCopyOutOfDate, after[0].State);
        Assert.Equal(1, after[0].AssignedPosition);
        Assert.True(after[0].CanUpdateAdapterCopy);
        Assert.Contains("out of date", after[0].StateLabel);
    }

    [Fact]
    public void AssignedButNotRunningReadsAsOnAdapterRatherThanActive()
    {
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Zelda",
                                     [Bind(0x05, KbmDestination.B)]);
        var state = State([Resident(3, 2, "Zelda", created.Fingerprint)],
                          runtimePosition: 1);

        var rows = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.OnAdapter, rows[0].State);
        Assert.Equal(2, rows[0].AssignedPosition);
    }

    [Fact]
    public void AResidentUpdateThatHasNotBeenActivatedSaysSo()
    {
        // Assigning into a position that is currently active must not mutate
        // gameplay mid-session, so the realized snapshot is preserved. The
        // adapter reports matchesSaved=false, and this is the row that explains
        // the resulting state instead of leaving it as an unexplained warning.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo",
                                     [Bind(0x04, KbmDestination.Zr)]);
        var state = State([Resident(2, 1, "Halo", created.Fingerprint)],
                          runtimePosition: 1, matchesSaved: false);

        var rows = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.ResidentUpdatedNotActivated, rows[0].State);
        Assert.Contains("activate", rows[0].StateLabel, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ContentMatchesAResidentWhenNoNameDoes()
    {
        // Windows and Android local ids are NOT shared, so content is the only
        // thing they can compare. A renamed local copy of the same mapping is
        // still the same mapping.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "My Halo",
                                     [Bind(0x04, KbmDestination.Zr)]);
        var state = State([Resident(2, 1, "Halo", created.Fingerprint)]);

        var rows = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.OnAdapter, rows[0].State);
        Assert.Equal(1, rows[0].AssignedPosition);
    }

    [Fact]
    public void AnEditedProfileKeepsItsOwnResidentRatherThanOneItNowEquals()
    {
        // THE ORDERING THIS PINS: name outranks content alone.
        //
        // Editing locally is the common case and changes content, so right after
        // a Save a profile no longer matches its own resident copy -- and may
        // coincidentally equal a DIFFERENT one. Matching on content first made an
        // edited "Halo" claim the unrelated Profile 2 and report itself safely on
        // the adapter, hiding the one fact the user needed: Halo's own copy is
        // stale. Caught by the Android cross-platform scenario, fixed in both.
        var library = Library();
        var halo = library.Create(KbmLayout.Keyboard, "Halo",
                                  [Bind(0x09, KbmDestination.Gr)]);
        var stale = library.Create(KbmLayout.Keyboard, "Scratch",
                                   [Bind(0x04, KbmDestination.Zr)]);
        library.Delete(stale.Id);

        var state = State(
        [
            // What Halo was before the edit.
            Resident(2, 1, "Halo", stale.Fingerprint),
            // Same content as the edited Halo, but a different profile entirely.
            Resident(3, 2, "Zelda", halo.Fingerprint),
        ]);

        var rows = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.AdapterCopyOutOfDate, rows[0].State);
        Assert.Equal(1, rows[0].AssignedPosition);
        Assert.True(rows[0].CanUpdateAdapterCopy);
    }

    [Fact]
    public void OneResidentCopyIsClaimedByAtMostOneLibraryRow()
    {
        // Two local profiles can hold IDENTICAL content -- two untouched copies
        // of Default do -- and a single resident copy must not make both of them
        // read as "on adapter". Claiming consumes, and the strongest evidence
        // wins: name and content together beat content alone.
        var library = Library();
        var halo = library.Create(KbmLayout.Keyboard, "Halo");
        var zelda = library.Create(KbmLayout.Keyboard, "Zelda");
        Assert.Equal(halo.Fingerprint, zelda.Fingerprint);  // same content

        var state = State([Resident(2, 1, "Zelda", zelda.Fingerprint)]);
        var rows = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard);

        Assert.Equal(KbmLocalState.OnAdapter,
                     rows.Single(row => row.Profile.Id == zelda.Id).State);
        Assert.Equal(KbmLocalState.LocalOnly,
                     rows.Single(row => row.Profile.Id == halo.Id).State);
    }

    [Fact]
    public void TheLibraryIsFilteredToTheLayoutBeingEdited()
    {
        // A profile belongs to one layout: its canonical defaults and its source
        // domain are layout-specific, so offering the other layout's profiles
        // would let the user assign a mapping the adapter must refuse.
        var library = Library();
        library.Create(KbmLayout.Keyboard, "Halo");
        library.Create(KbmLayout.KeyboardMouse, "Metroid");

        Assert.Single(KbmBankView.Library(library.Value, State(), KbmLayout.Keyboard));
        Assert.Single(KbmBankView.Library(library.Value, State(),
                                          KbmLayout.KeyboardMouse));
    }

    [Fact]
    public void TwentyLocalProfilesAgainstThreeBankPositionsIsAnOrdinaryState()
    {
        // The whole correction in one assertion: the adapter's capacity does not
        // bound the library, and the extra rows are simply "local only".
        var library = Library();
        for (var i = 0; i < 20; i++)
        {
            library.Create(KbmLayout.Keyboard, $"Profile {i}");
        }

        var rows = KbmBankView.Library(library.Value, State(), KbmLayout.Keyboard);

        Assert.Equal(20, rows.Count);
        Assert.All(rows, row => Assert.Equal(KbmLocalState.LocalOnly, row.State));
        Assert.Equal(KbmLimits.PositionsPerLayout + 1,
                     KbmBankView.Bank(State(), KbmLayout.Keyboard).Count);
    }
}
