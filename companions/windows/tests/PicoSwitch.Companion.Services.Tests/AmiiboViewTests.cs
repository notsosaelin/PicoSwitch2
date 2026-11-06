using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Amiibo page's rules, tested where they live rather than on a page.
/// </summary>
/// <remarks>
/// THE RULE THAT MATTERS: a game writing to a tag produces a change that exists
/// ONLY on the adapter. Until it is synced, the bytes on the adapter are the sole
/// copy of something a save just produced, so the two operations that would
/// destroy them — uploading over it and clearing it — must be refused, and
/// syncing must stay available. Every enablement assertion below is really that
/// one rule seen from a different angle.
/// </remarks>
public sealed class AmiiboViewTests
{
    private static AmiiboView View(
        AmiiboStatus? status = null,
        bool connected = true,
        CapabilityState capability = CapabilityState.Available,
        bool keys = true,
        IEnumerable<AmiiboLibraryItem>? library = null,
        string? selectedId = null) =>
        AmiiboView.From(
            new AdapterSnapshot
            {
                Amiibo = status ?? new AmiiboStatus(),
                Capabilities = new AdapterCapabilities(Amiibo: capability),
            },
            connected,
            new ValueList<AmiiboLibraryItem>(library ?? []),
            keys,
            selectedId);

    private static AmiiboLibraryItem Item(string id = "a", string uid = "0400000000000000") => new()
    {
        Id = id,
        DisplayName = "Tom Nook",
        FileName = $"{id}.bin",
        Size = 540,
        Crc32 = "00000000",
        Uid = uid,
        FigureId = "0183000002420502",
        Imported = DateTimeOffset.UnixEpoch,
    };

    // ------------------------------------------------------------ slot state

    [Fact]
    public void AnEmptyAdapterSaysSo()
    {
        var view = View();
        Assert.Equal(AmiiboSlotState.Empty, view.Slot);
        Assert.False(view.AnythingLoaded);
        Assert.False(view.NeedsSync);
    }

    [Fact]
    public void LoadedAndPresentedAreDifferentStates()
    {
        // Holding a tag and offering it to the console are separate facts, and a
        // user who cannot tell them apart cannot tell why a game sees nothing.
        var loaded = View(new AmiiboStatus { Loaded = true });
        Assert.Equal(AmiiboSlotState.Loaded, loaded.Slot);

        var presented = View(new AmiiboStatus { Loaded = true, Presented = true });
        Assert.Equal(AmiiboSlotState.Presented, presented.Slot);
    }

    [Fact]
    public void AV3ImageCountsAsLoaded()
    {
        // The firmware reports the two tag families through different flags;
        // "is something loaded" must not miss one of them.
        var view = View(new AmiiboStatus { V3Loaded = true });
        Assert.True(view.AnythingLoaded);
        Assert.Equal(AmiiboSlotState.Loaded, view.Slot);
    }

    [Fact]
    public void ModifiedOutranksPresented()
    {
        // A tag can be presented AND dirty at once. The unsynced change is the
        // more important fact and the one the user has to act on.
        var view = View(new AmiiboStatus { Loaded = true, Presented = true, Dirty = true });
        Assert.Equal(AmiiboSlotState.Modified, view.Slot);
        Assert.True(view.NeedsSync);
        Assert.Contains("only on the adapter", view.SlotDetail);
    }

    // ------------------------------------------------------------ enablement

    [Fact]
    public void UnsyncedChangesBlockTheTwoDestructiveActions()
    {
        // The heart of it. Uploading replaces the adapter's copy; clearing
        // discards it. Either would destroy the only record of what a game just
        // wrote.
        var view = View(
            new AmiiboStatus { Loaded = true, Dirty = true },
            library: [Item()],
            selectedId: "a");

        Assert.False(view.CanUpload);
        Assert.False(view.CanClear);

        // And the way out stays open.
        Assert.True(view.CanSync);
    }

    [Fact]
    public void WithNothingUnsyncedTheOrdinaryActionsAreOffered()
    {
        var view = View(new AmiiboStatus { Loaded = true }, library: [Item()], selectedId: "a");
        Assert.True(view.CanUpload);
        Assert.True(view.CanClear);
        Assert.True(view.CanSync);
        Assert.True(view.CanPresent);
        Assert.False(view.CanEject);
    }

    [Fact]
    public void PresentAndEjectAreNeverBothOffered()
    {
        var loaded = View(new AmiiboStatus { Loaded = true });
        Assert.True(loaded.CanPresent);
        Assert.False(loaded.CanEject);

        var presented = View(new AmiiboStatus { Loaded = true, Presented = true });
        Assert.False(presented.CanPresent);
        Assert.True(presented.CanEject);
    }

    [Fact]
    public void NothingIsOfferedForAnEmptySlot()
    {
        var view = View(library: [Item()], selectedId: "a");
        Assert.False(view.CanPresent);
        Assert.False(view.CanEject);
        Assert.False(view.CanSync);
        Assert.False(view.CanClear);
        // Uploading IS offered: it is the action that fills an empty slot.
        Assert.True(view.CanUpload);
    }

    [Fact]
    public void UploadNeedsSomethingSelected()
    {
        Assert.False(View(library: [Item()]).CanUpload);
        Assert.True(View(library: [Item()], selectedId: "a").CanUpload);
    }

    [Fact]
    public void TheConsoleCopyChoiceIsOnlyForATagThatHasOne()
    {
        // Only an NTAG215 tag a game has written keeps the original and the
        // console's version side by side. A v3 image has no such pair.
        Assert.False(View(new AmiiboStatus { Loaded = true }).CanChooseCopy);
        Assert.True(View(new AmiiboStatus { Loaded = true, HasSave2 = true }).CanChooseCopy);
        Assert.False(
            View(new AmiiboStatus { V3Loaded = true, HasSave2 = true }).CanChooseCopy);
    }

    [Fact]
    public void TheCopyLabelSaysWhichOneIsInUse()
    {
        Assert.Contains(
            "original",
            View(new AmiiboStatus { Loaded = true, HasSave2 = true }).CopyLabel);
        Assert.Contains(
            "console",
            View(new AmiiboStatus { Loaded = true, HasSave2 = true, UsingSave2 = true }).CopyLabel);
    }

    // ---------------------------------------------------------- availability

    [Fact]
    public void EveryAdapterActionIsOffWhenDisconnected()
    {
        var view = View(
            new AmiiboStatus { Loaded = true, HasSave2 = true },
            connected: false,
            library: [Item()],
            selectedId: "a");

        Assert.False(view.Available);
        Assert.False(view.CanUpload);
        Assert.False(view.CanPresent);
        Assert.False(view.CanSync);
        Assert.False(view.CanClear);
        Assert.False(view.CanChooseCopy);
        Assert.Contains("Connect the adapter", view.UnavailableReason);
    }

    [Fact]
    public void UnsupportedFirmwareIsDistinguishedFromBeingOffline()
    {
        // Different problems with different fixes: one is "plug it in", the other
        // is "update it". A single "unavailable" would send users to the wrong one.
        var offline = View(connected: false);
        var unsupported = View(capability: CapabilityState.Unsupported);
        var unread = View(capability: CapabilityState.Unknown);

        Assert.Contains("Connect", offline.UnavailableReason);
        Assert.Contains("does not support", unsupported.UnavailableReason);
        Assert.Contains("not reported", unread.UnavailableReason);
        Assert.Null(View(new AmiiboStatus()).UnavailableReason);
    }

    // ----------------------------------------------------------------- keys

    [Fact]
    public void TheKeyNoticeSaysWhatStillWorksWithoutThem()
    {
        // Identity is plaintext, so the library is fully usable with no keys.
        // A blanket "import your keys" gate on a page that mostly works is what
        // makes a feature look broken.
        var view = View(keys: false);
        Assert.NotNull(view.KeyNotice);
        Assert.Contains("without it", view.KeyNotice);
        Assert.Null(View(keys: true).KeyNotice);
    }

    [Fact]
    public void MissingKeysDoNotDisableAnyAdapterAction()
    {
        var view = View(
            new AmiiboStatus { Loaded = true },
            keys: false,
            library: [Item()],
            selectedId: "a");

        Assert.True(view.CanUpload);
        Assert.True(view.CanPresent);
        Assert.True(view.CanSync);
        Assert.True(view.CanClear);
    }

    // -------------------------------------------------------------- matching

    [Fact]
    public void TheLoadedTagIsMatchedToTheLibraryByUid()
    {
        // The adapter reports the tag's own identity; that is what ties it to a
        // backup regardless of what the user called it.
        var view = View(
            new AmiiboStatus { Loaded = true, Uid = "04676FFAE04981" },
            library: [Item("a", "04676FFAE04981"), Item("b", "0400000000000000")]);

        Assert.Equal("a", view.LoadedFromLibrary?.Id);
    }

    [Fact]
    public void ATagThatIsNotInTheLibraryMatchesNothing()
    {
        // Legitimate: the other companion may have sent it. Reporting a wrong
        // match would tell the user a backup exists when it does not.
        var view = View(
            new AmiiboStatus { Loaded = true, Uid = "04ABCDEF012345" },
            library: [Item("a", "04676FFAE04981")]);

        Assert.Null(view.LoadedFromLibrary);
    }

    [Fact]
    public void NoMatchIsClaimedForAnEmptySlot()
    {
        Assert.Null(View(library: [Item()]).LoadedFromLibrary);
    }
}
