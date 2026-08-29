using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Where one peer belongs on the Paired Controllers card.
///
/// The sections are decided by BOND AND CONNECTION STATE, not by role, because
/// those are the two questions the user is actually asking: is it working right
/// now, is it still paired, or is it merely something this adapter used to know.
/// Role only decides one thing here — whether the row is a controller at all, or
/// the user's own PC, which never belongs in a controller list.
/// </summary>
public enum PeerSection
{
    /// <summary>Connected to the adapter right now.</summary>
    Connected,

    /// <summary>
    /// The adapter holds a credential; the controller is simply not here.
    ///
    /// A peer lands here whether or not the adapter can currently NAME it. Role
    /// is live evidence, so after a reboot every paired controller reads
    /// <c>unknown</c> until it reconnects — routing on that would empty this list
    /// on every power cycle and hide real pairings from the person who made them.
    /// Being bonded and not being this PC is enough.
    /// </summary>
    Paired,

    /// <summary>No credential any more. Remembered by this app, not by the adapter.</summary>
    Recent,

    /// <summary>This PC, in either of its relationships. Never offered as a controller.</summary>
    Companion,

    /// <summary>
    /// Neither bonded nor connected, and not this PC.
    ///
    /// Defensive only: a live inventory should not produce one, because a peer
    /// exists in it precisely by holding a credential or a link. Kept as an
    /// explicit destination so an unexpected row goes to Diagnostics rather than
    /// silently into the product list.
    /// </summary>
    Unattributed,
}

/// <summary>
/// One row, resolved from the adapter's live answer and this app's memory.
///
/// <c>Role</c> is always the adapter's live classification and is never
/// rewritten; <c>RememberedRole</c> is what this app has previously seen proven.
/// When they differ the UI says so rather than presenting memory as current fact
/// — the management protocol requires that <c>unknown</c> be rendered as
/// unidentified, and it is.
/// </summary>
public sealed record PeerListing
{
    public required string PeerId { get; init; }

    public required string Address { get; init; }

    public required string DisplayName { get; init; }

    public required PeerSection Section { get; init; }

    /// <summary>The adapter's live answer, verbatim.</summary>
    public required PeerRole Role { get; init; }

    /// <summary>The strongest role this app has ever seen the adapter prove.</summary>
    public required PeerRole RememberedRole { get; init; }

    public PeerTransportSet Transports { get; init; }

    public bool Connected { get; init; }

    public bool Bonded { get; init; }

    public string? Classification { get; init; }

    public long? LastConnectedAtMillis { get; init; }

    /// <summary>
    /// The adapter could not identify this peer and the label came from memory.
    /// Presentation MUST attribute it; a remembered identity shown as a live one
    /// is exactly the promotion the protocol forbids.
    /// </summary>
    public bool IdentifiedFromHistory { get; init; }

    /// <summary>True for a row that exists only in app history, whose only action is to forget it.</summary>
    public bool HistoryOnly { get; init; }
}

public sealed record ControllerInventoryView
{
    public IReadOnlyList<PeerListing> Connected { get; init; } = [];

    public IReadOnlyList<PeerListing> Paired { get; init; } = [];

    public IReadOnlyList<PeerListing> Recent { get; init; } = [];

    public IReadOnlyList<PeerListing> Companion { get; init; } = [];

    public IReadOnlyList<PeerListing> Unattributed { get; init; } = [];

    /// <summary>What Paired Controllers renders.</summary>
    public bool HasControllers => Connected.Count > 0 || Paired.Count > 0 || Recent.Count > 0;

    /// <summary>What Diagnostics renders: this PC's own relationships and unattributable records.</summary>
    public bool HasDiagnosticPeers => Companion.Count > 0 || Unattributed.Count > 0;

    public bool IsEmpty => !HasControllers && !HasDiagnosticPeers;
}

/// <summary>
/// Build the Paired Controllers view from what the adapter reports and what this
/// app remembers.
///
/// The adapter's inventory is authoritative about existence, bonding and live
/// connection. History contributes exactly two things: a readable label for a
/// peer the adapter cannot currently identify, and the rows for peers the adapter
/// no longer holds a key for at all.
/// </summary>
public static class ControllerInventory
{
    public static ControllerInventoryView Build(PeerInventory inventory, AdapterPeerHistory history)
    {
        var live = inventory.Peers
            .Select(peer => Listing(peer, history.Record(peer.Id)))
            .ToList();
        var liveIds = inventory.Peers.Select(peer => peer.Id).ToHashSet(StringComparer.Ordinal);

        var recent = history.Forgotten
            .Where(record => !liveIds.Contains(record.PeerId))

            // A PC this app once managed the adapter with is not a controller the
            // user forgot; it would read as one under "Recent".
            .Where(record => !record.IsCompanionRole)
            .Select(HistoryListing)
            .OrderByDescending(listing => listing.LastConnectedAtMillis ?? 0)
            .ToList();

        return new ControllerInventoryView
        {
            Connected = Sorted(live, PeerSection.Connected),
            Paired = Sorted(live, PeerSection.Paired),
            Recent = recent,
            Companion = Sorted(live, PeerSection.Companion),
            Unattributed = live
                .Where(listing => listing.Section == PeerSection.Unattributed)
                .OrderBy(listing => listing.Address, StringComparer.Ordinal)
                .ToList(),
        };
    }

    private static List<PeerListing> Sorted(List<PeerListing> live, PeerSection section) =>
        live.Where(listing => listing.Section == section)
            .OrderBy(listing => listing.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            .ToList();

    private static PeerListing Listing(PeerInfo peer, PeerHistoryRecord? remembered)
    {
        var rememberedRole = remembered?.ProvenRole ?? PeerRole.Unknown;

        // Companion membership uses the strongest evidence from either source.
        // Excluding the user's own PC from the controller list on remembered
        // evidence is safe in the direction that matters: the cost of being wrong
        // is a PC shown under "This PC", and the cost of the opposite mistake is
        // offering to forget the management relationship.
        var effectiveRole = PeerRoleStrength.Stronger(peer.Role, rememberedRole);
        var companion = effectiveRole is PeerRole.ManagementCompanion or PeerRole.ControllerLink;

        // Bonding, not naming, decides whether this is a controller pairing.
        //
        // Routing on role was wrong and hid real hardware: role is live evidence
        // only, so after the adapter reboots EVERY paired controller reads
        // `unknown` until it reconnects. The person who paired it would open
        // Paired Controllers and find it empty. A peer that holds a credential and
        // is not this PC is a controller pairing whether or not the adapter can
        // currently say which controller it is; the row says "Not yet identified"
        // instead of pretending to know.
        var section =
            companion ? PeerSection.Companion
            : peer.Connected ? PeerSection.Connected
            : peer.Bonded ? PeerSection.Paired
            : PeerSection.Unattributed;

        var liveName = PeerNaming.Label(
            address: peer.Address,
            classification: peer.Classification,
            name: peer.Name,
            vendorId: peer.VendorId,
            productId: peer.ProductId);

        // Fall back to memory only when the adapter offered nothing at all, so a
        // name the adapter can currently see always wins over a stale one.
        var adapterNamedIt = peer.Classification is not null || peer.Name is not null || peer.HasUsbIdentity;
        var rememberedName = remembered?.RememberedName;

        return new PeerListing
        {
            PeerId = peer.Id,
            Address = peer.Address,
            DisplayName = adapterNamedIt || rememberedName is null ? liveName : rememberedName,
            Section = section,
            Role = peer.Role,
            RememberedRole = rememberedRole,
            Transports = peer.Transports,
            Connected = peer.Connected,
            Bonded = peer.Bonded,
            Classification = peer.Classification ?? remembered?.Classification,
            LastConnectedAtMillis = remembered?.LastConnectedAtMillis,
            IdentifiedFromHistory = !adapterNamedIt && rememberedName is not null,
        };
    }

    private static PeerListing HistoryListing(PeerHistoryRecord record) => new()
    {
        PeerId = record.PeerId,
        Address = record.Address,
        DisplayName = record.RememberedName,
        Section = PeerSection.Recent,

        // The adapter has no opinion at all about a peer it no longer stores.
        Role = PeerRole.Unknown,
        RememberedRole = record.ProvenRole,
        Transports = record.Transports,
        Connected = false,
        Bonded = false,
        Classification = record.Classification,
        LastConnectedAtMillis = record.LastConnectedAtMillis,
        IdentifiedFromHistory = true,
        HistoryOnly = true,
    };
}
