using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The local library works with NO adapter, and never writes to one.
/// </summary>
/// <remarks>
/// THE DEFECT THESE PIN. The page's New called
/// <c>SaveKeyboardMouseProfileAsync</c> — the staged ADAPTER transaction — and
/// its Duplicate called <c>kbm profile dup</c>. Creating a local profile
/// therefore required a connection and wrote to flash, because the page's only
/// notion of "profile" was <c>KeyboardMouseDraft.ProfileId</c>, an adapter
/// resident id. "The profile I have open" and "the profile resident on the
/// adapter" were the same object.
///
/// The fix is the domain split, not a UI guard: the editing path now runs on
/// <see cref="KbmLocalDraft"/> and <see cref="KbmLibraryRepository"/>, neither of
/// which can reach a <c>ManagementClient</c>. These tests construct the library
/// with NO transport at all — if any local operation tried to talk to an
/// adapter, there would be nothing there to talk to.
/// </remarks>
public sealed class KbmOfflineLibraryTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-offline-" + Guid.NewGuid().ToString("N"));

    // No transport, no client, no connection. Deliberately.
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

    private static KbmSource Key(int usage) => new(KbmSourceKind.Key, usage);

    /* ------------------------------------------------------------- offline */

    [Fact]
    public void ANewProfileCanBeCreatedWithNoAdapter()
    {
        var library = Library();

        var draft = KbmLocalDraft.FromDefault(KbmLayout.Keyboard)
            .WithName("Halo");
        var created = library.Create(KbmLayout.Keyboard, draft.Name,
                                     draft.Overrides, draft.Mouse);

        Assert.NotEmpty(created.Id);
        Assert.Equal("Halo", created.Name);
        Assert.Single(library.Value.Profiles);
    }

    [Fact]
    public void AProfileCanBeEditedAndSavedWithNoAdapter()
    {
        // The full loop: compose from the embedded canonical defaults, rebind,
        // save. Nothing here has a transport.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo");

        var draft = KbmLocalDraft.From(created);
        Assert.False(draft.Dirty);
        Assert.NotEmpty(draft.Effective);

        draft = draft.With(Key(0x04), KbmDestination.Capture);
        Assert.True(draft.Dirty);

        var saved = library.Save(draft.ProfileId, draft.Name, draft.Overrides,
                                 draft.Mouse);

        Assert.NotEqual(created.Fingerprint, saved.Fingerprint);
        Assert.Equal(saved.Fingerprint, Library().Value.Find(created.Id)!.Fingerprint);
    }

    [Fact]
    public void TheFullMappingIsDrawableOfflineFromTheEmbeddedDefaults()
    {
        // The page used to fetch the canonical table from the adapter, which is
        // what made editing require a connection at all.
        var draft = KbmLocalDraft.FromDefault(KbmLayout.KeyboardMouse);

        Assert.NotEmpty(draft.Effective);
        Assert.Contains(draft.Effective,
                        binding => binding.Source.Kind == KbmSourceKind.MouseButton);
        Assert.All(draft.Effective, binding => Assert.False(binding.Custom));
    }

    [Fact]
    public void EveryLibraryOperationWorksWithNoAdapter()
    {
        var library = Library();

        var halo = library.Create(KbmLayout.Keyboard, "Halo");
        var copy = library.Duplicate(halo.Id, "Halo Copy");
        var renamed = library.Rename(copy.Id, "Halo II");
        var deleted = library.Delete(halo.Id);

        Assert.NotNull(renamed);
        Assert.True(deleted);
        Assert.Single(library.Value.Profiles);
        Assert.Equal("Halo II", library.Value.Profiles[0].Name);
    }

    /* --------------------------------------------------- draft is local-only */

    [Fact]
    public void ADraftCarriesNoAdapterIdentityAtAll()
    {
        // The structural half of the fix. KbmLocalDraft is keyed on a GUID no
        // adapter has ever seen, so there is no id to accidentally send.
        var created = Library().Create(KbmLayout.Keyboard, "Halo");
        var draft = KbmLocalDraft.From(created);

        Assert.Equal(created.Id, draft.ProfileId);
        Assert.True(Guid.TryParseExact(draft.ProfileId, "N", out _));
    }

    [Fact]
    public void EditingTheBuiltinTemplateProducesANewLocalProfileRatherThanWritingToIt()
    {
        // Default is the always-available starting point; making it writable
        // would remove the one mapping a user can always fall back to.
        var library = Library();
        var draft = KbmLocalDraft.FromDefault(KbmLayout.Keyboard)
            .With(Key(0x04), KbmDestination.Zr);

        Assert.True(draft.IsBuiltin);
        Assert.True(draft.Dirty);

        var created = library.Create(draft.Layout, "From Default", draft.Overrides,
                                     draft.Mouse);

        Assert.NotEmpty(created.Id);
        Assert.Single(created.Bindings);
    }

    [Fact]
    public void PuttingAnEditBackReturnsTheDraftToClean()
    {
        // Dirty is computed from content, so an override equal to the canonical
        // default is dropped rather than stored — otherwise Save would stay lit
        // for a change with no effect.
        var canonical = KbmDefaults.For(KbmLayout.Keyboard).Bindings[0];
        var draft = KbmLocalDraft.FromDefault(KbmLayout.Keyboard);

        var edited = draft.With(canonical.Source, KbmDestination.Capture);
        Assert.True(edited.Dirty);
        Assert.Single(edited.Overrides);

        var restored = edited.With(canonical.Source, canonical.Destination);
        Assert.False(restored.Dirty);
        Assert.Empty(restored.Overrides);
    }

    [Fact]
    public void DiscardRestoresTheSavedContentWithoutTouchingAnything()
    {
        var created = Library().Create(KbmLayout.Keyboard, "Halo");
        var draft = KbmLocalDraft.From(created)
            .With(Key(0x04), KbmDestination.Capture)
            .WithName("Renamed");

        Assert.True(draft.Dirty);
        Assert.False(draft.Discard().Dirty);
        Assert.Equal("Halo", draft.Discard().Name);
    }

    /* ----------------------------------------------- library vs adapter copy */

    [Fact]
    public void ALocalSaveLeavesTheResidentCopyAloneAndSaysSo()
    {
        // The ordinary state after a local Save: the adapter still holds the
        // older content and is unaware of the edit, and the row must say that
        // rather than claim agreement.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo");

        var resident = new KbmProfileInfo(2, KbmLayout.Keyboard, "Halo", Revision: 1,
                                          Overrides: 0,
                                          Fingerprint: created.Fingerprint,
                                          Position: 1);
        var state = ConnectedState([resident]);

        Assert.Equal(KbmLocalState.OnAdapter,
                     KbmBankView.Library(library.Value, state, KbmLayout.Keyboard)[0].State);

        // Local edit + local save. The resident copy is untouched by construction.
        var draft = KbmLocalDraft.From(created).With(Key(0x04), KbmDestination.Capture);
        library.Save(draft.ProfileId, draft.Name, draft.Overrides, draft.Mouse);

        var row = KbmBankView.Library(library.Value, state, KbmLayout.Keyboard)[0];
        Assert.Equal(KbmLocalState.AdapterCopyOutOfDate, row.State);
        Assert.True(row.CanUpdateAdapterCopy);
    }

    [Fact]
    public void DeletingALocalProfileLeavesTheResidentCopyOnTheAdapter()
    {
        // The resident copy is a separate snapshot the adapter owns and may be
        // running. A library deletion must not change console behaviour.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo");
        var resident = new KbmProfileInfo(2, KbmLayout.Keyboard, "Halo", 1, 0,
                                          created.Fingerprint, Position: 1);
        var state = ConnectedState([resident]);

        library.Delete(created.Id);

        // Gone from the library, still on the adapter.
        Assert.Empty(library.Value.Profiles);
        Assert.NotNull(state.Profiles.At(KbmLayout.Keyboard, 1));
        Assert.Equal("Halo", state.Profiles.At(KbmLayout.Keyboard, 1)!.Name);
    }

    [Fact]
    public void AResidentProfileWithNoLocalCopyCanBeImportedOnce()
    {
        // The cross-platform bridge: a profile created on Android reaches Windows
        // only as an adapter resident copy.
        var library = Library();
        IReadOnlyList<KbmBinding> content =
            [new KbmBinding(Key(0x04), KbmDestination.Zr, Custom: true)];

        var first = library.Import(KbmLayout.Keyboard, "Halo", content,
                                   new KbmMouseConfig());
        var again = library.Import(KbmLayout.Keyboard, "Halo", content,
                                   new KbmMouseConfig());

        Assert.Equal(first.Id, again.Id);
        Assert.Single(library.Value.Profiles);
    }

    [Fact]
    public void RemovingAResidentCopyDoesNotTouchTheLibrary()
    {
        // The other half of the independence. Removal is an adapter operation;
        // the library keeps the profile, so the user can assign it again later.
        var library = Library();
        var created = library.Create(KbmLayout.Keyboard, "Halo");
        var resident = new KbmProfileInfo(2, KbmLayout.Keyboard, "Halo", 1, 0,
                                          created.Fingerprint, Position: 1);

        // Before: on the adapter.
        Assert.Equal(KbmLocalState.OnAdapter,
                     KbmBankView.Library(library.Value, ConnectedState([resident]),
                                         KbmLayout.Keyboard)[0].State);

        // After: the adapter reports the position empty. The library row survives
        // and simply reads as local only.
        var afterRemoval = KbmBankView.Library(library.Value, ConnectedState([]),
                                               KbmLayout.Keyboard);
        Assert.Single(afterRemoval);
        Assert.Equal(KbmLocalState.LocalOnly, afterRemoval[0].State);
        Assert.Equal(created.Id, afterRemoval[0].Profile.Id);

        // And the bank position really is free again.
        Assert.DoesNotContain(
            KbmBankView.Bank(ConnectedState([]), KbmLayout.Keyboard),
            slot => !slot.Empty && slot.Position != KbmPositions.Default);
    }

    [Fact]
    public void ReconnectingRestoresResidentStateWithoutDisturbingTheLibrary()
    {
        // The library is on disk and the resident set is on the adapter, so a
        // disconnect/reconnect changes only the second one. Nothing about the
        // user's profiles depends on a session.
        var library = Library();
        var halo = library.Create(KbmLayout.Keyboard, "Halo");
        library.Create(KbmLayout.Keyboard, "Zelda");
        var resident = new KbmProfileInfo(2, KbmLayout.Keyboard, "Halo", 1, 0,
                                          halo.Fingerprint, Position: 1);

        // Disconnected: both profiles are still there, both simply local.
        var offline = KbmBankView.Library(library.Value, DisconnectedState(),
                                          KbmLayout.Keyboard);
        Assert.Equal(2, offline.Count);
        Assert.All(offline, row => Assert.Equal(KbmLocalState.LocalOnly, row.State));

        // Reconnected: the library is byte-identical, and the resident
        // relationship reappears on the row it belongs to.
        var reopened = Library().Value;
        Assert.Equal(2, reopened.Profiles.Count);

        var online = KbmBankView.Library(reopened, ConnectedState([resident]),
                                         KbmLayout.Keyboard);
        Assert.Equal(KbmLocalState.OnAdapter,
                     online.Single(row => row.Profile.Id == halo.Id).State);
        Assert.Equal(KbmLocalState.LocalOnly,
                     online.Single(row => row.Profile.Name == "Zelda").State);
    }

    private static KeyboardMouseState DisconnectedState() => new()
    {
        // Never read: no session, so no resident set and no active mapping.
        Readiness = KeyboardMouseReadiness.NotRead,
        Capability = CapabilityState.Unknown,
    };

    private static KeyboardMouseState ConnectedState(
        IEnumerable<KbmProfileInfo> residents) => new()
    {
        Readiness = KeyboardMouseReadiness.Ready,
        Capability = CapabilityState.Available,
        Profiles = new KbmProfiles(
            new ValueList<KbmProfileInfo>(residents),
            new ValueList<KbmActiveMapping>(
            [
                new KbmActiveMapping(KbmLayout.Keyboard, SourceId: KbmProfileIds.Default,
                                     Revision: 0, Fingerprint: 0, MatchesSaved: true),
                new KbmActiveMapping(KbmLayout.KeyboardMouse, KbmProfileIds.Default,
                                     0, 0, true),
            ]),
            Max: KbmLimits.MaxProfiles),
    };
}
