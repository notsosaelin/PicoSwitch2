using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// What the adapter is holding, in the terms a user thinks in.
/// </summary>
/// <remarks>
/// Deliberately NOT a copy of <see cref="AmiiboStatus"/>. That record is the
/// firmware's vocabulary — loaded, v3Loaded, dirty, presented, persisted,
/// hasSave2 — and a page that rendered those flags directly would be showing
/// protocol state and calling it a product. The distinctions worth surfacing are
/// different from the flags that carry them.
/// </remarks>
public enum AmiiboSlotState
{
    /// <summary>Nothing on the adapter.</summary>
    Empty,

    /// <summary>A tag is loaded and the console cannot see it.</summary>
    Loaded,

    /// <summary>A tag is loaded and offered to the console right now.</summary>
    Presented,

    /// <summary>
    /// The console has written the tag and the change exists ONLY on the adapter.
    /// </summary>
    /// <remarks>
    /// The state everything else defers to. Uploading over it, or clearing it,
    /// would destroy the only copy of something a game just saved, so both are
    /// refused until it has been synced.
    /// </remarks>
    Modified,
}

/// <summary>The Amiibo page's whole model, projected from adapter truth.</summary>
public sealed record AmiiboView
{
    public required bool Connected { get; init; }

    public required CapabilityState Capability { get; init; }

    public required AmiiboStatus Status { get; init; }

    /// <summary>Whether the user has supplied a retail key set.</summary>
    public required bool KeysAvailable { get; init; }

    public required ValueList<AmiiboLibraryItem> Library { get; init; }

    public string? SelectedId { get; init; }

    /// <summary>Identity and register metadata for the selected backup.</summary>
    public AmiiboDetails? SelectedDetails { get; init; }

    /// <summary>An upload or sync in flight, as (done, total) bytes.</summary>
    public (int Completed, int Total)? Transfer { get; init; }

    public bool Available => Connected && Capability == CapabilityState.Available;

    public AmiiboLibraryItem? Selected =>
        SelectedId is null ? null : Library.FirstOrDefault(item => item.Id == SelectedId);

    public bool AnythingLoaded => Status.Loaded || Status.V3Loaded;

    public AmiiboSlotState Slot => !AnythingLoaded
        ? AmiiboSlotState.Empty
        : Status.Dirty
            ? AmiiboSlotState.Modified
            : Status.Presented
                ? AmiiboSlotState.Presented
                : AmiiboSlotState.Loaded;

    public string SlotHeadline => Slot switch
    {
        AmiiboSlotState.Empty => "No Amiibo loaded",
        AmiiboSlotState.Loaded => "Loaded — not presented",
        AmiiboSlotState.Presented => "Presented to the console",
        AmiiboSlotState.Modified => "Changed by the console — not synced",
        _ => "",
    };

    /// <summary>
    /// The sentence under the headline. Says what to do, not what a flag is.
    /// </summary>
    public string SlotDetail => Slot switch
    {
        AmiiboSlotState.Empty =>
            "Send one from your library to use it on the console.",
        AmiiboSlotState.Loaded =>
            "The adapter is holding this tag. Present it when the game asks for one.",
        AmiiboSlotState.Presented =>
            "The console can read this tag now. Eject it when you are finished.",
        AmiiboSlotState.Modified =>
            "The game wrote to this tag and the change exists only on the adapter. " +
            "Sync it to your library before sending another one or clearing it.",
        _ => "",
    };

    /// <summary>
    /// True when the adapter holds changes that exist nowhere else.
    /// </summary>
    /// <remarks>
    /// The one condition on this page that justifies blocking other actions.
    /// </remarks>
    public bool NeedsSync => Slot == AmiiboSlotState.Modified;

    // ------------------------------------------------------------ enablement
    //
    // Each rule states the reason it exists. A control that is disabled for a
    // reason nobody can reconstruct reads as a broken app.

    /// <summary>Sending replaces what the adapter holds, so unsynced work blocks it.</summary>
    public bool CanUpload => Available && Selected is not null && !NeedsSync;

    public bool CanPresent => Available && AnythingLoaded && !Status.Presented;

    public bool CanEject => Available && AnythingLoaded && Status.Presented;

    /// <summary>Reading back is the ONLY action that stays available when dirty.</summary>
    public bool CanSync => Available && AnythingLoaded;

    /// <summary>Clearing discards the adapter's copy, so unsynced work blocks it too.</summary>
    public bool CanClear => Available && AnythingLoaded && !NeedsSync;

    /// <summary>
    /// Only an NTAG215 tag a game has written keeps two copies side by side.
    /// </summary>
    public bool CanChooseCopy => Available && Status.HasSave2 && !Status.V3Loaded;

    public string CopyLabel => Status.UsingSave2
        ? "Using the console's copy"
        : "Using your original backup";

    /// <summary>Why the adapter half is unavailable, or null when it is not.</summary>
    public string? UnavailableReason => Available
        ? null
        : !Connected
            ? "Connect the adapter to send Amiibo to it."
            : Capability == CapabilityState.Unsupported
                ? "This adapter's firmware does not support Amiibo."
                : "The adapter has not reported its Amiibo state yet.";

    /// <summary>
    /// What the library can say about a backup with no keys imported.
    /// </summary>
    /// <remarks>
    /// Identity is plaintext, so the list is fully usable without keys — only the
    /// owner, nickname and game data need them. Said explicitly because "import
    /// your keys" as a blanket gate on a page that mostly works is the kind of
    /// thing that makes a feature look broken.
    /// </remarks>
    public string? KeyNotice => KeysAvailable
        ? null
        : "Import your amiibo key file to see nicknames, owners and game data. " +
          "Everything else here works without it.";

    /// <summary>Which library entry, if any, the adapter is currently holding.</summary>
    /// <remarks>
    /// Matched by UID: the adapter reports the tag's own identity, and that is
    /// what ties it to a backup regardless of what the user named it.
    /// </remarks>
    public AmiiboLibraryItem? LoadedFromLibrary =>
        !AnythingLoaded || Status.Uid.Length == 0
            ? null
            : Library.FirstOrDefault(item =>
                string.Equals(item.Uid, Status.Uid, StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// Project the page's model from adapter truth.
    /// </summary>
    /// <remarks>
    /// Takes <paramref name="connected"/> as a plain flag rather than a
    /// connection state: this project must not reference the Windows transport
    /// types, and the target-framework split is the guard that keeps it honest.
    /// </remarks>
    public static AmiiboView From(
        AdapterSnapshot snapshot,
        bool connected,
        ValueList<AmiiboLibraryItem> library,
        bool keysAvailable,
        string? selectedId,
        AmiiboDetails? selectedDetails = null,
        (int Completed, int Total)? transfer = null) => new()
        {
            Connected = connected,
            Capability = snapshot.Capabilities.Amiibo,
            Status = snapshot.Amiibo,
            KeysAvailable = keysAvailable,
            Library = library,
            SelectedId = selectedId,
            SelectedDetails = selectedDetails,
            Transfer = transfer,
        };
}
