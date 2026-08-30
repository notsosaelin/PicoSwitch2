namespace PicoSwitch.Management;

/// <summary>
/// Portable management workflows over an already-connected channel.
///
/// This class owns command construction, reply parsing, pagination,
/// mutation/readback, and transactional adapter workflows. It owns no platform
/// connection objects and no presentation or local-library state.
///
/// The clock and the delay are injectable seams. They exist so the pagination,
/// persistence and Amiibo workflows are testable in milliseconds rather than in
/// real seconds — the C# equivalent of the Kotlin suite's virtual time — and for
/// no other reason. Production passes neither.
/// </summary>
public sealed class ManagementClient(
    IManagementChannel channel,
    Func<long>? nowMillis = null,
    Func<TimeSpan, CancellationToken, Task>? delay = null)
{
    // A syntactically valid peer id that cannot name a real peer: ids are a hash
    // of an identity address and this is the all-zero hash, which no address
    // produces. Used only to probe whether `peers forget` exists.
    private const string UnaddressablePeerId = "p_00000000";

    private const int MaxKbmMapPages = 32;
    private const int MaxBondPages = 128;

    // 32 possible peers and at least one per page, so this bound can only be
    // reached by an adapter that is misbehaving.
    private const int MaxPeerPages = 64;

    public const int WakeStatusPolls = 6;
    public const long WakeStatusPollMillis = 150L;
    public const long AmiiboPersistTimeoutMillis = 6_000L;
    public const long AmiiboPollMillis = 200L;
    public const long PersistTimeoutMillis = 6_000L;
    public const long PersistPollMillis = 100L;

    /// <summary>
    /// Application-level remote-pairing status poll. The firmware owns the
    /// 30-second deadline it reports as <c>remaining_ms</c>; this is only how
    /// often a client asks.
    /// </summary>
    public const long PairingPollMillis = 1_000L;

    private const long Uint32Mask = 0xFFFF_FFFFL;
    private const long Uint32HalfRange = 0x8000_0000L;
    private const string UnavailablePayloadCrc = "00000000";

    private static readonly int[] AmiiboSupportedSizes = [540, 572, 2048];

    private readonly Func<long> now =
        nowMillis ?? (() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());

    private readonly Func<TimeSpan, CancellationToken, Task> sleep =
        delay ?? ((duration, token) => Task.Delay(duration, token));

    public Task<FirmwareInfo> FirmwareAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.Info, ManagementProtocol.Firmware, ct);

    public Task<ControllerInfo> ControllerAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.Device, ManagementProtocol.Controller, ct);

    public Task<AdapterConfig> ConfigAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.GetConfig, ManagementProtocol.Config, ct);

    public Task<PersonalityState> PersonalityAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.Personality, ManagementProtocol.PersonalityState, ct);

    public Task<AmiiboStatus> AmiiboStatusAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.AmiiboStatus, ManagementProtocol.Amiibo, ct);

    public Task<AdapterInputState> InputSourcesAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.InputSources, ManagementProtocol.InputSources, ct);

    public Task<bool?> ManagementEnabledAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.ManagementStatus, ManagementProtocol.ManagementEnabled, ct);

    public Task<KbmStatus> KbmStatusAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.KbmStatus, ManagementProtocol.KbmStatus, ct);

    public Task<KbmMouseConfig> KbmMouseAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.KbmMouseStatus, ManagementProtocol.KbmMouse, ct);

    public Task<PersistenceStatus> PersistenceStatusAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.SaveStatus, ManagementProtocol.PersistenceStatus, ct);

    /// <summary>Initial portable state composition. Unsupported optional families remain explicit.</summary>
    public async Task<ManagementRefresh> RefreshAllAsync(
        AdapterSnapshot? previous = null,
        CancellationToken ct = default)
    {
        previous ??= new AdapterSnapshot();

        var firmware = await FirmwareAsync(ct).ConfigureAwait(false);
        var config = await ConfigAsync(ct).ConfigureAwait(false);
        var controller = await ControllerAsync(ct).ConfigureAwait(false);
        var personality = await OptionalAsync(() => PersonalityAsync(ct)).ConfigureAwait(false);
        var amiibo = await OptionalAsync(() => AmiiboStatusAsync(ct)).ConfigureAwait(false);
        var management = await OptionalAsync(() => ManagementEnabledAsync(ct)).ConfigureAwait(false);
        var bonds = await OptionalAsync(() => ListBondsAsync(ct)).ConfigureAwait(false);
        var input = await OptionalAsync(() => InputSourcesAsync(ct)).ConfigureAwait(false);
        var kbmStatus = await OptionalAsync(() => KbmStatusAsync(ct)).ConfigureAwait(false);
        var kbmMouse = kbmStatus.Value is not null
            ? await OptionalAsync(() => KbmMouseAsync(ct)).ConfigureAwait(false)
            : new OptionalResult<KbmMouseConfig>(null, kbmStatus.State);

        var bondCapability = bonds.Value is null
            ? bonds.State
            : bonds.Value.Complete
                ? CapabilityState.Available
                : CapabilityState.Unknown;

        var snapshot = new AdapterSnapshot
        {
            Firmware = firmware,
            Controller = controller,
            Personality = personality.Value ?? previous.Personality,
            Config = config,
            Amiibo = amiibo.Value ?? previous.Amiibo,
            ManagementEnabled = management.Value ?? previous.ManagementEnabled,
            Bonds = bonds.Value?.Entries ?? ValueList<BondInfo>.Empty,
            BondsComplete = bonds.Value?.Complete,
            BondsTotal = bonds.Value?.Total,
            Input = input.Value ?? previous.Input,
            Capabilities = new AdapterCapabilities(
                Core: CapabilityState.Available,
                Personality: personality.State,
                Colors: CapabilityState.Available,
                Amiibo: amiibo.State,
                ManagementGate: management.State,
                Bonds: bondCapability,
                Wake: previous.Capabilities.Wake,
                ActiveInput: input.State,
                Kbm: kbmStatus.State),
            RefreshedAtMillis = now(),
        };

        return new ManagementRefresh(snapshot, kbmStatus.Value, kbmMouse.Value);
    }

    public async Task<AdapterInputState> SetActiveInputAsync(
        long sourceId,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.InputActive(sourceId), ct).ConfigureAwait(false);
        return await InputSourcesAsync(ct).ConfigureAwait(false);
    }

    public Task<CommandAcknowledgement> SetPersonalityAsync(
        Personality personality,
        CancellationToken ct = default) =>
        AcknowledgeAsync(ManagementCommands.SetPersonality(personality), ct);

    /// <summary>Runtime colour mutation, optional persistence request, then authoritative readback.</summary>
    public async Task<(AdapterConfig Config, PersistenceAcknowledgement? Persistence)> SetColorAsync(
        ColorTarget target,
        RgbColor color,
        bool persist = true,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.Color(target, color), ct).ConfigureAwait(false);
        var persistence = persist ? await SaveAsync(ct).ConfigureAwait(false) : null;
        return (await ConfigAsync(ct).ConfigureAwait(false), persistence);
    }

    public async Task ReenumerateUsbAsync(CancellationToken ct = default)
    {
        var acknowledgement = await AcknowledgeAsync(ManagementCommands.Reenumerate, ct)
            .ConfigureAwait(false);
        if (!acknowledgement.Reenumerating)
        {
            throw new ManagementProtocolException("Adapter did not confirm USB re-enumeration");
        }
    }

    public async Task<bool?> SetManagementEnabledAsync(bool enabled, CancellationToken ct = default) =>
        (await AcknowledgeAsync(ManagementCommands.ManagementEnabled(enabled), ct)
            .ConfigureAwait(false)).Enabled;

    public async Task<WakeStatus> WakeConsoleAsync(CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.Wake, ct).ConfigureAwait(false);
        var status = new WakeStatus(WakeResult.Unknown, false, false, 0, 0);
        for (var attempt = 0; attempt < WakeStatusPolls; attempt++)
        {
            await sleep(TimeSpan.FromMilliseconds(WakeStatusPollMillis), ct).ConfigureAwait(false);
            try
            {
                status = await ExchangeAsync(
                    ManagementCommands.WakeStatus,
                    ManagementProtocol.WakeStatus,
                    ct).ConfigureAwait(false);
            }
            catch (AdapterCommandException error) when (error.IsUnsupported())
            {
                return new WakeStatus(WakeResult.Unknown, false, false, 0, 0);
            }

            if (status.Result != WakeResult.Pending)
            {
                return status;
            }
        }

        return status;
    }

    public async Task<KbmMapping> LoadKbmMappingAsync(
        KbmLayout profile,
        CancellationToken ct = default)
    {
        var bindings = new List<KbmBinding>();
        var pageNumber = 0;
        int? expectedTotal = null;
        while (true)
        {
            var command = ManagementCommands.KbmMap(profile, pageNumber);
            var page = await ExchangeAsync(command, ManagementProtocol.KbmMapPage, ct)
                .ConfigureAwait(false);
            if (page.Profile != profile || page.Page != pageNumber)
            {
                throw new ManagementPaginationException(
                    "Adapter returned a different KB/M profile or page");
            }

            expectedTotal ??= page.Total;
            if (expectedTotal != page.Total || bindings.Count + page.Bindings.Count > page.Total)
            {
                throw new ManagementPaginationException(
                    "Adapter changed the KB/M binding total during pagination");
            }

            bindings.AddRange(page.Bindings);
            if (!page.More)
            {
                break;
            }

            if (page.Bindings.Count == 0 || ++pageNumber > MaxKbmMapPages)
            {
                throw new ManagementPaginationException(
                    "Adapter returned a non-progressing KB/M binding list");
            }
        }

        if (bindings.Count != expectedTotal)
        {
            throw new ManagementPaginationException("Adapter returned an incomplete KB/M binding list");
        }

        return new KbmMapping(profile, new ValueList<KbmBinding>(bindings), Loaded: true);
    }

    public async Task<KbmStatus> SetKbmModeAsync(KbmMode mode, CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.KbmMode(mode), ct).ConfigureAwait(false);
        return await KbmStatusAsync(ct).ConfigureAwait(false);
    }

    public async Task<KbmMapping> BindKbmAsync(
        KbmLayout profile,
        KbmSource source,
        KbmDestination? destination,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.KbmBind(profile, source, destination), ct)
            .ConfigureAwait(false);
        return await LoadKbmMappingAsync(profile, ct).ConfigureAwait(false);
    }

    public Task<KbmProfiles> KbmProfilesAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.KbmProfileList,
                      ManagementProtocol.KbmProfiles, ct);

    /// <summary>
    /// Activate a profile on the ADAPTER, then re-read the table so the caller
    /// never has to assume the selection took.
    /// </summary>
    public async Task<KbmProfiles> UseKbmProfileAsync(
        KbmLayout layout,
        int id,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.KbmProfileUse(layout, id), ct)
            .ConfigureAwait(false);
        return await KbmProfilesAsync(ct).ConfigureAwait(false);
    }

    public async Task<KbmMapping> ResetKbmLayoutAsync(
        KbmLayout profile,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.KbmReset(profile), ct).ConfigureAwait(false);
        return await LoadKbmMappingAsync(profile, ct).ConfigureAwait(false);
    }

    public async Task<(KbmStatus Status, KbmMouseConfig Mouse, IReadOnlyDictionary<KbmLayout, KbmMapping> Mappings)>
        ResetAllKbmAsync(CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.KbmResetAll, ct).ConfigureAwait(false);
        var status = await KbmStatusAsync(ct).ConfigureAwait(false);
        var mouse = await KbmMouseAsync(ct).ConfigureAwait(false);
        var mappings = new Dictionary<KbmLayout, KbmMapping>();
        foreach (var profile in KbmLayouts.All)
        {
            mappings[profile] = await LoadKbmMappingAsync(profile, ct).ConfigureAwait(false);
        }

        return (status, mouse, mappings);
    }

    public Task<KbmMouseConfig> SetKbmMouseAsync(
        KbmMouseField field,
        int value,
        CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.KbmMouse(field, value), ManagementProtocol.KbmMouse, ct);

    public async Task<BondEnumeration> ListBondsAsync(CancellationToken ct = default)
    {
        const string legacyCommand = "bonds list";
        var first = await RawAsync(legacyCommand, ct).ConfigureAwait(false);
        bool versioned;
        try
        {
            versioned = ManagementProtocol.IsVersionedBondResponse(legacyCommand, first);
        }
        catch (AdapterCommandException error)
            when (error.Code == 413 ||
                  error.AdapterMessage.Contains("response_too_large", StringComparison.OrdinalIgnoreCase))
        {
            return await CollectBondPagesAsync(null, ct).ConfigureAwait(false);
        }

        return versioned
            ? await CollectBondPagesAsync(first, ct).ConfigureAwait(false)
            : ManagementProtocol.LegacyBonds(legacyCommand, first);
    }

    public async Task<BondEnumeration> RemoveBondAsync(int index, CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.BondRemove(index), ct).ConfigureAwait(false);
        return await ListBondsAsync(ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Read the adapter's complete logical peer inventory.
    ///
    /// All-or-nothing by design. A partially read inventory is worse than none:
    /// the missing row is a device the user cannot see, and on this page that
    /// means a saved controller they would conclude is already gone. Any
    /// pagination inconsistency is therefore an exception, not a shorter list.
    /// </summary>
    public async Task<PeerInventory> ListPeersAsync(CancellationToken ct = default)
    {
        var entries = new List<PeerInfo>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var cursor = 0;
        int? expectedTotal = null;
        var pages = 0;
        while (true)
        {
            var page = await ExchangeAsync(
                ManagementCommands.PeersPage(cursor == 0 ? null : cursor),
                ManagementProtocol.PeersPage,
                ct).ConfigureAwait(false);

            expectedTotal ??= page.Total;
            if (expectedTotal != page.Total)
            {
                throw new ManagementPaginationException(
                    "Adapter changed the peer-list total during pagination");
            }

            foreach (var entry in page.Entries)
            {
                if (!seen.Add(entry.Id))
                {
                    throw new ManagementPaginationException(
                        "Adapter repeated a peer during pagination");
                }

                entries.Add(entry);
            }

            if (page.Next is not { } next)
            {
                break;
            }

            if (next <= cursor || ++pages > MaxPeerPages)
            {
                throw new ManagementPaginationException(
                    "Adapter returned a non-progressing peer-list cursor");
            }

            cursor = next;
        }

        var total = expectedTotal ?? 0;
        if (entries.Count != total)
        {
            throw new ManagementPaginationException("Adapter returned an incomplete peer list");
        }

        return new PeerInventory
        {
            Peers = new ValueList<PeerInfo>(entries),
            Complete = true,
            Total = total,
        };
    }

    /// <summary>
    /// Forget one peer, and read the adapter's verified answer.
    ///
    /// One command, not a disconnect-then-delete pair: the adapter sequences the
    /// whole operation internally so nothing can race between the steps. The
    /// caller is still expected to re-read the inventory afterwards — the
    /// firmware is authoritative about what remains, and this reply describes one
    /// peer rather than the whole picture.
    /// </summary>
    public Task<PeerForgetOutcome> ForgetPeerAsync(string peerId, CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.PeersForget(peerId), ManagementProtocol.PeersForget, ct);

    /// <summary>
    /// Is selective forget available on this firmware?
    ///
    /// Probed with a well-formed id that no adapter can hold — the peer id space
    /// is a hash of an identity address, and this one addresses nothing — so a
    /// firmware that HAS the command answers <c>already_absent</c> and deletes
    /// nothing, while one that lacks it answers <c>unknown command</c>. There is
    /// no read-only form of a mutation to probe with, so the probe is made
    /// harmless instead.
    /// </summary>
    public async Task<CapabilityState> ProbePeerForgetAsync(CancellationToken ct = default) =>
        (await OptionalAsync(() => ForgetPeerAsync(UnaddressablePeerId, ct)).ConfigureAwait(false))
        .State;

    /// <summary>
    /// Ask the adapter to open its controller pairing window.
    ///
    /// The adapter owns the deadline, so the app is never responsible for closing
    /// it: losing this session, or the PC, cannot leave the adapter discoverable.
    /// <see cref="CancelPairingAsync"/> is a courtesy, not a safety mechanism.
    /// </summary>
    public Task<PairingStatus> StartPairingAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.PairingStart, ManagementProtocol.PairingStatus, ct);

    /// <summary>
    /// Probe remote pairing without starting one.
    ///
    /// <c>pairing status</c> is read-only and answers <c>unknown command</c> on
    /// firmware that predates the feature, which is exactly the signal the
    /// optional-family probe turns into <c>Unsupported</c>. Probing with
    /// <c>start</c> instead would open a real pairing window just to find out
    /// whether it exists.
    /// </summary>
    public async Task<CapabilityState> ProbeRemotePairingAsync(CancellationToken ct = default) =>
        (await OptionalAsync(() => PairingStatusAsync(ct)).ConfigureAwait(false)).State;

    public Task<PairingStatus> PairingStatusAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.PairingStatus, ManagementProtocol.PairingStatus, ct);

    /// <summary>Idempotent: cancelling when idle succeeds and reports idle.</summary>
    public Task<PairingStatus> CancelPairingAsync(CancellationToken ct = default) =>
        ExchangeAsync(ManagementCommands.PairingCancel, ManagementProtocol.PairingStatus, ct);

    public async Task<PersistenceAcknowledgement> SaveAsync(CancellationToken ct = default)
    {
        var acknowledgement = await AcknowledgeAsync(ManagementCommands.Save, ct).ConfigureAwait(false);
        return new PersistenceAcknowledgement(
            acknowledgement.Queued ? PersistenceState.Queued : PersistenceState.Accepted,
            acknowledgement.Requested);
    }

    public async Task<PersistenceStatus> SaveAndAwaitAsync(
        long timeoutMillis = PersistTimeoutMillis,
        CancellationToken ct = default)
    {
        var acknowledgement = await SaveAsync(ct).ConfigureAwait(false);
        return await AwaitPersistenceAsync(acknowledgement, timeoutMillis, ct).ConfigureAwait(false);
    }

    public async Task<PersistenceStatus> AwaitPersistenceAsync(
        PersistenceAcknowledgement acknowledgement,
        long timeoutMillis = PersistTimeoutMillis,
        CancellationToken ct = default)
    {
        if (timeoutMillis <= 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeoutMillis),
                "Persistence timeout must be positive");
        }

        if (acknowledgement.RequestId is not { } requestId)
        {
            throw new ManagementProtocolException("Adapter did not identify the persistence request");
        }

        var deadline = now() + timeoutMillis;
        while (true)
        {
            var status = await PersistenceStatusAsync(ct).ConfigureAwait(false);
            if (RequestReached(status.Completed, requestId))
            {
                return status;
            }

            if (now() >= deadline)
            {
                throw new ManagementException("Adapter did not finish settings persistence in time");
            }

            await sleep(TimeSpan.FromMilliseconds(PersistPollMillis), ct).ConfigureAwait(false);
        }
    }

    public async Task<AmiiboStatus> UploadAmiiboAsync(
        byte[] data,
        bool useSave2 = false,
        Action<int, int>? progress = null,
        CancellationToken ct = default)
    {
        ValidateAmiiboSize(data.Length);
        var current = await AmiiboStatusAsync(ct).ConfigureAwait(false);
        if (current.Dirty)
        {
            throw new ManagementException("Sync the modified Amiibo before replacing it");
        }

        try
        {
            await AcknowledgeAsync(
                ManagementCommands.AmiiboBegin(data.Length, Crc32.Hex(data)),
                ct).ConfigureAwait(false);

            for (var offset = 0; offset < data.Length; offset += ManagementProtocol.AmiiboChunkBytes)
            {
                var length = Math.Min(ManagementProtocol.AmiiboChunkBytes, data.Length - offset);
                await AcknowledgeAsync(
                    ManagementCommands.AmiiboChunk(offset, data.AsSpan(offset, length)),
                    ct).ConfigureAwait(false);
                progress?.Invoke(Math.Min(offset + length, data.Length), data.Length);
            }

            await AcknowledgeAsync(ManagementCommands.AmiiboCommit(useSave2), ct).ConfigureAwait(false);
            await AcknowledgeAsync(ManagementCommands.AmiiboPersist, ct).ConfigureAwait(false);
            await AwaitAmiiboAsync(status => status.Persisted && !status.PersistPending, ct)
                .ConfigureAwait(false);
        }
        catch
        {
            try
            {
                await AcknowledgeAsync(ManagementCommands.AmiiboCancel, ct).ConfigureAwait(false);
            }
            catch
            {
                // The upload already failed; a failed cancel must not replace its cause.
            }

            throw;
        }

        return await AmiiboStatusAsync(ct).ConfigureAwait(false);
    }

    public async Task<AmiiboDownload> DownloadAmiiboAsync(
        Action<int, int>? progress = null,
        CancellationToken ct = default)
    {
        var status = await AmiiboStatusAsync(ct).ConfigureAwait(false);
        if (!status.Loaded && !status.V3Loaded)
        {
            throw new ManagementException("No Amiibo is loaded on the adapter");
        }

        if (!AmiiboSupportedSizes.Contains(status.Size))
        {
            throw new ManagementException(
                $"Adapter reported unsupported Amiibo size {status.Size}; no memory was allocated");
        }

        var output = new byte[status.Size];
        var offset = 0;
        while (offset < output.Length)
        {
            var count = Math.Min(ManagementProtocol.AmiiboChunkBytes, output.Length - offset);
            var command = ManagementCommands.AmiiboRead(offset, count);
            var bytes = await ExchangeAsync(command, ManagementProtocol.ReadData, ct)
                .ConfigureAwait(false);
            if (bytes.Length != count)
            {
                throw new ManagementProtocolException(
                    $"Adapter returned {bytes.Length} of {count} requested bytes");
            }

            bytes.CopyTo(output, offset);
            offset += count;
            progress?.Invoke(offset, output.Length);
        }

        var crc = Crc32.Hex(output);
        var expectedCrc =
            status.V3Loaded ||
            !status.PayloadCrc.Equals(UnavailablePayloadCrc, StringComparison.OrdinalIgnoreCase)
                ? status.PayloadCrc
                : null;
        if (expectedCrc is not null &&
            !expectedCrc.Equals(crc, StringComparison.OrdinalIgnoreCase))
        {
            throw new ManagementProtocolException(
                $"Synced Amiibo failed CRC verification ({expectedCrc} != {crc})");
        }

        return new AmiiboDownload(output, status.Generation, expectedCrc);
    }

    public async Task<AmiiboStatus> AcknowledgeDownloadedAmiiboAsync(
        AmiiboDownload download,
        CancellationToken ct = default)
    {
        var current = await AmiiboStatusAsync(ct).ConfigureAwait(false);
        var crcChanged = download.PayloadCrc is not null &&
            !current.PayloadCrc.Equals(download.PayloadCrc, StringComparison.OrdinalIgnoreCase);
        if (current.Generation != download.Generation || crcChanged)
        {
            throw new ManagementException("Adapter Amiibo changed during Sync; acknowledge was refused");
        }

        await AcknowledgeAsync(ManagementCommands.AmiiboDownloaded, ct).ConfigureAwait(false);
        await AcknowledgeAsync(ManagementCommands.AmiiboPersist, ct).ConfigureAwait(false);
        await AwaitAmiiboAsync(status => status.Persisted && !status.PersistPending, ct)
            .ConfigureAwait(false);
        return await AmiiboStatusAsync(ct).ConfigureAwait(false);
    }

    public async Task<AmiiboStatus> SetAmiiboPresentedAsync(
        bool presented,
        CancellationToken ct = default)
    {
        await AcknowledgeAsync(ManagementCommands.AmiiboPresented(presented), ct).ConfigureAwait(false);
        return await AmiiboStatusAsync(ct).ConfigureAwait(false);
    }

    public async Task<AmiiboStatus> SelectAmiiboCopyAsync(bool used, CancellationToken ct = default)
    {
        var current = await AmiiboStatusAsync(ct).ConfigureAwait(false);
        if (!current.HasSave2 || current.V3Loaded)
        {
            throw new ManagementException("This Amiibo does not expose a separate console-written copy");
        }

        await AcknowledgeAsync(ManagementCommands.AmiiboSelect(used), ct).ConfigureAwait(false);
        return await AmiiboStatusAsync(ct).ConfigureAwait(false);
    }

    public async Task<AmiiboStatus> ClearAmiiboAsync(CancellationToken ct = default)
    {
        var current = await AmiiboStatusAsync(ct).ConfigureAwait(false);
        if (current.Dirty)
        {
            throw new ManagementException("Sync the modified Amiibo before clearing it");
        }

        await AcknowledgeAsync(ManagementCommands.AmiiboClear, ct).ConfigureAwait(false);
        return await AwaitAmiiboAsync(
            status => !status.Loaded && !status.V3Loaded && !status.PersistPending,
            ct).ConfigureAwait(false);
    }

    private async Task<BondEnumeration> CollectBondPagesAsync(
        string? firstResponse,
        CancellationToken ct)
    {
        var entries = new List<BondInfo>();
        var seen = new HashSet<int>();
        var cursor = 0;
        int? expectedTotal = null;
        var response = firstResponse;
        var pages = 0;
        while (true)
        {
            BondPage page;
            if (response is null)
            {
                page = await ExchangeAsync(
                    ManagementCommands.BondsPage(cursor == 0 ? null : cursor),
                    ManagementProtocol.BondsPage,
                    ct).ConfigureAwait(false);
            }
            else
            {
                page = ManagementProtocol.BondsPage("bonds list", response);
                response = null;
            }

            expectedTotal ??= page.Total;
            if (expectedTotal != page.Total)
            {
                throw new ManagementPaginationException(
                    "Adapter changed the bond-list total during pagination");
            }

            foreach (var entry in page.Entries)
            {
                if (!seen.Add(entry.Index))
                {
                    throw new ManagementPaginationException(
                        "Adapter repeated a bond during pagination");
                }

                entries.Add(entry);
            }

            if (page.Next is not { } next)
            {
                break;
            }

            if (next <= cursor || ++pages > MaxBondPages)
            {
                throw new ManagementPaginationException(
                    "Adapter returned a non-progressing bond-list cursor");
            }

            cursor = next;
        }

        var total = expectedTotal ?? 0;
        if (entries.Count != total)
        {
            throw new ManagementPaginationException("Adapter returned an incomplete bond list");
        }

        return new BondEnumeration(new ValueList<BondInfo>(entries), Complete: true, Total: total);
    }

    private async Task<AmiiboStatus> AwaitAmiiboAsync(
        Func<AmiiboStatus, bool> predicate,
        CancellationToken ct)
    {
        var deadline = now() + AmiiboPersistTimeoutMillis;
        while (true)
        {
            var status = await AmiiboStatusAsync(ct).ConfigureAwait(false);
            if (predicate(status))
            {
                return status;
            }

            if (now() >= deadline)
            {
                throw new ManagementException("Adapter did not finish persistence within 6 seconds");
            }

            await sleep(TimeSpan.FromMilliseconds(AmiiboPollMillis), ct).ConfigureAwait(false);
        }
    }

    private async Task<CommandAcknowledgement> AcknowledgeAsync(string command, CancellationToken ct) =>
        ManagementProtocol.Acknowledgement(command, await RawAsync(command, ct).ConfigureAwait(false));

    private async Task<T> ExchangeAsync<T>(
        string command,
        Func<string, string, T> parser,
        CancellationToken ct) =>
        parser(command, await RawAsync(command, ct).ConfigureAwait(false));

    private Task<string> RawAsync(string command, CancellationToken ct)
    {
        // Framing validates length and single-line shape before anything is sent,
        // so an over-long command fails locally rather than being truncated by the
        // carrier.
        ManagementProtocol.Frame(command);
        return channel.TransactAsync(command, ManagementChannel.DefaultTimeoutMillis, ct);
    }

    private static async Task<OptionalResult<T>> OptionalAsync<T>(Func<Task<T>> block)
    {
        try
        {
            return new OptionalResult<T>(await block().ConfigureAwait(false), CapabilityState.Available);
        }
        catch (AdapterCommandException error) when (error.IsUnsupported())
        {
            return new OptionalResult<T>(default, CapabilityState.Unsupported);
        }
    }

    /// <summary>
    /// Has the adapter's completed counter reached this request?
    ///
    /// Compared in <c>uint32</c> modulus so a counter that has wrapped past
    /// 0xFFFFFFFF still reads as "reached" rather than as an eternally pending
    /// save.
    /// </summary>
    private static bool RequestReached(long completed, long requestId) =>
        ((completed - requestId) & Uint32Mask) < Uint32HalfRange;

    private static void ValidateAmiiboSize(int size)
    {
        if (!AmiiboSupportedSizes.Contains(size))
        {
            throw new ManagementException("Amiibo image size must be 540, 572, or 2048 bytes");
        }
    }

    private readonly record struct OptionalResult<T>(T? Value, CapabilityState State);
}
