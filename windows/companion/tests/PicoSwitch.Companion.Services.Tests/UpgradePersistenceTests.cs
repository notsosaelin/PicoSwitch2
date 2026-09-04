using PicoSwitch.Companion.Services;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// A user's configuration survives an upgrade.
///
/// ## Why the round-trip tests are not enough
///
/// `AdapterRegistryStoreTests` already proves each document survives a RESTART:
/// today's encoder writes it and today's decoder reads it. That passes even when
/// both ends change together — which is exactly what an upgrade is not. The
/// version a user upgrades FROM is gone; only the bytes on their disk remain.
///
/// So the documents below are pinned as literal JSON, in the shape the shipping
/// build actually writes (captured from the encoder, not written by hand). A
/// codec change that would drop, rename or re-nest a field fails here, and it
/// fails naming the field rather than showing up later as a stranger's adapter
/// list quietly emptying after an update.
///
/// ## What is and is not asserted
///
/// These assert that KNOWN fields still decode, never that unknown ones are
/// rejected. A newer build reading an older document is the case that matters
/// and the case that is tested. An older build reading a newer one is not
/// supported.
///
/// ## Scope
///
/// `WINDOWS_PASS.md` §31 Phase 9 wants an upgrade "preserving adapter registry,
/// aliases, active selection, peer history, library and settings". The half that
/// needs two signed packages installed in sequence is a human step and cannot be
/// automated here. The half that actually breaks — a decoder that no longer
/// understands what the last release wrote — is this file.
/// </summary>
public sealed class UpgradePersistenceTests
{
    private static readonly AdapterId Adapter = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;

    /// <summary>`adapters.json`, exactly as the shipping build writes it.</summary>
    private const string RegistryDocument =
        """
        {"schema":1,"active":"AA:BB:CC:DD:EE:01","adapters":[{"id":"AA:BB:CC:DD:EE:01",
        "address":"AA:BB:CC:DD:EE:01","alias":"Living room","name":"PicoSwitch2",
        "lastSeen":1756900000000,"firmware":"2.0","personality":"pro2"}]}
        """;

    [Fact]
    public void RememberedAdaptersSurviveAnUpgrade()
    {
        var registry = AdapterRegistryCodec.Decode(RegistryDocument);
        var record = Assert.Single(registry.Records);

        Assert.Equal("AA:BB:CC:DD:EE:01", record.Address);

        // The alias is the one field the user typed themselves, so losing it is
        // the most visible upgrade failure there is.
        Assert.Equal("Living room", record.UserAlias);

        // The active selection decides what the app opens onto.
        Assert.Equal(Adapter, registry.ActiveId);

        // Cached facts. Not critical -- they are re-read on connect -- but a
        // silent rename here is the same class of defect as losing the alias.
        Assert.Equal("PicoSwitch2", record.LastKnownName);
        Assert.Equal("2.0", record.LastFirmwareVersion);
        Assert.Equal("pro2", record.LastPersonality);
        Assert.Equal(1756900000000, record.LastSeenAtMillis);
    }

    /// <summary>`peer-history.json`, exactly as the shipping build writes it.</summary>
    private const string PeerHistoryDocument =
        """
        {"schema":1,"adapters":[{"adapter":"AA:BB:CC:DD:EE:01","peers":[{"id":"p_1",
        "addr":"AA:BB:CC:DD:EE:FF","name":"DualSense","role":"controller","tr":1,
        "firstSeen":1000,"lastSeen":1000,"bonded":true}]}]}
        """;

    [Fact]
    public void PeerHistorySurvivesAnUpgrade()
    {
        var book = PeerHistoryCodec.Decode(PeerHistoryDocument);
        var record = Assert.Single(book.ForAdapter(Adapter).Records);

        Assert.Equal("p_1", record.PeerId);
        Assert.Equal("AA:BB:CC:DD:EE:FF", record.Address);
        Assert.Equal("DualSense", record.LastKnownName);
        Assert.True(record.Bonded);
    }

    [Fact]
    public void AnUnknownFieldDoesNotDiscardTheDocument()
    {
        // Forward compatibility in the direction that actually happens: a
        // document written by a build that knew about something this one does
        // not. It must decode what it understands rather than treat the whole
        // file as corrupt -- otherwise moving between two versions, on one
        // machine or two, wipes the user's adapters.
        var registry = AdapterRegistryCodec.Decode(
            """
            {"schema":1,"active":"AA:BB:CC:DD:EE:01","fromLater":{"nested":true},
            "adapters":[{"id":"AA:BB:CC:DD:EE:01","address":"AA:BB:CC:DD:EE:01",
            "alias":"Living room","futureField":7}]}
            """);

        Assert.Equal("Living room", Assert.Single(registry.Records).UserAlias);
        Assert.Equal(Adapter, registry.ActiveId);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("null")]
    [InlineData("[]")]
    [InlineData("{")]
    [InlineData("{\"schema\":1}")]
    public void AnUnreadableDocumentCostsItsContentsAndNotTheLaunch(string hostile)
    {
        // The upgrade-shaped disaster. A document this build genuinely cannot
        // read must degrade to "nothing remembered", never to a crash on start:
        // a user who loses their adapter list can re-pair, while one who cannot
        // launch has no way back at all.
        var registry = AdapterRegistryCodec.Decode(hostile);
        Assert.NotNull(registry);
        Assert.Empty(registry.Records);

        Assert.Empty(PeerHistoryCodec.Decode(hostile).ByAdapter);
    }

    [Fact]
    public void TheEncodersStillWriteTheShapeThatIsPinnedAbove()
    {
        // The other half of the guard. Without this, someone could change BOTH
        // the encoder and the pinned literals together and the tests above would
        // keep passing while every existing user's document became unreadable.
        //
        // Compared as a decoded pair rather than as text, so key ORDER and
        // whitespace stay free to change -- neither is part of the contract.
        var registry = new AdapterRegistry()
            .With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with
            {
                UserAlias = "Living room",
                LastKnownName = "PicoSwitch2",
                LastSeenAtMillis = 1756900000000,
                LastFirmwareVersion = "2.0",
                LastPersonality = "pro2",
            })
            .Selecting(Adapter);

        var written = AdapterRegistryCodec.Decode(AdapterRegistryCodec.Encode(registry));
        var pinned = AdapterRegistryCodec.Decode(RegistryDocument);

        Assert.Equal(pinned.ActiveId, written.ActiveId);
        Assert.Equal(pinned.Records, written.Records);
    }
}
