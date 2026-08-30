using System.Text;
using System.Text.Json;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// Cross-language management conformance.
///
/// Runs the same cases as the Kotlin <c>ProtocolConformanceTest</c> against the
/// SAME file, <c>tools/fixtures/management/protocol-v1.json</c>. That shared
/// authority is what makes the Level 1 duplication (WINDOWS_PASS.md §9.4) safe:
/// a protocol change made in one language and not the other fails here.
/// </summary>
public sealed class ProtocolConformanceTests
{
    private readonly JsonDocument fixtureRoot =
        JsonDocument.Parse(RepositoryFixtures.ReadText(RepositoryFixtures.ManagementProtocolFixture));

    private JsonElement Root => fixtureRoot.RootElement;

    [Fact]
    public void FixtureLimitsAndBleConstantsMatchTheImplementation()
    {
        var limits = Root.GetProperty("limits");
        Assert.Equal(ManagementProtocol.MaxCommandBytes, limits.GetProperty("commandPayloadBytes").GetInt32());
        Assert.Equal(
            BleManagementContract.MaxReplyPayloadBytes,
            limits.GetProperty("bleReplyPayloadBytes").GetInt32());
        Assert.Equal(ManagementProtocol.AmiiboChunkBytes, limits.GetProperty("amiiboChunkBytes").GetInt32());
        Assert.Equal(
            ManagementProtocol.BondsProtocolVersion,
            limits.GetProperty("bondEnvelopeVersion").GetInt32());
        Assert.Equal(
            ManagementProtocol.PeersProtocolVersion,
            limits.GetProperty("peerEnvelopeVersion").GetInt32());

        var ble = Root.GetProperty("ble");
        Assert.Equal(BleManagementContract.ServiceUuid, ble.GetProperty("serviceUuid").GetString());
        Assert.Equal(BleManagementContract.RxUuid, ble.GetProperty("rxUuid").GetString());
        Assert.Equal(BleManagementContract.TxUuid, ble.GetProperty("txUuid").GetString());
        Assert.Equal(
            BleManagementContract.AttPayloadWithDefaultMtu,
            ble.GetProperty("minimumMtuPayloadBytes").GetInt32());
    }

    [Fact]
    public void LanguageNeutralBuilderVectorsMatchCommandAuthority()
    {
        var generated = new Dictionary<string, string>
        {
            ["inputNone"] = ManagementCommands.InputActive(0),
            ["setPersonality"] = ManagementCommands.SetPersonality(Personality.JoyConRight),
            ["bondPage"] = ManagementCommands.BondsPage(3),
            ["peerPage"] = ManagementCommands.PeersPage(2),
            ["peerForget"] = ManagementCommands.PeersForget("p_5E6F7A8B"),
            ["kbmDefault"] = ManagementCommands.KbmBind(
                KbmLayout.KeyboardMouse,
                new KbmSource(KbmSourceKind.Key, 0x1A),
                null),
            ["kbmNone"] = ManagementCommands.KbmBind(
                KbmLayout.KeyboardMouse,
                new KbmSource(KbmSourceKind.Key, 0x1A),
                KbmDestination.None),
            ["mouseSensitivityX"] = ManagementCommands.KbmMouse(KbmMouseField.SensitivityX, 512),
            ["bodyColor"] = ManagementCommands.Color(ColorTarget.Body, new RgbColor(1, 2, 3)),
            ["saveStatus"] = ManagementCommands.SaveStatus,
            ["amiiboRead"] = ManagementCommands.AmiiboRead(32, 16),
        };

        var covered = 0;
        foreach (var builder in Root.GetProperty("builders").EnumerateArray())
        {
            var operation = builder.GetProperty("operation").GetString()!;
            Assert.True(generated.ContainsKey(operation), $"no C# builder for '{operation}'");
            Assert.Equal(builder.GetProperty("command").GetString(), generated[operation]);
            covered++;
        }

        // Every C# builder must be exercised by the shared fixture, so a new
        // command cannot be added on one side alone.
        Assert.Equal(generated.Count, covered);
    }

    [Fact]
    public void LanguageNeutralPagingVectorsFormCompleteSequences()
    {
        var paging = Root.GetProperty("pagination");

        var bonds = paging.GetProperty("bonds").EnumerateArray()
            .Select(item => ManagementProtocol.BondsPage(
                item.GetProperty("command").GetString()!,
                item.GetProperty("reply").GetRawText()))
            .ToList();
        Assert.Equal([3, null], bonds.Select(page => page.Next).ToList());
        Assert.Equal(2, bonds.Sum(page => page.Entries.Count));

        var peers = paging.GetProperty("peers").EnumerateArray()
            .Select(item => ManagementProtocol.PeersPage(
                item.GetProperty("command").GetString()!,
                item.GetProperty("reply").GetRawText()))
            .ToList();
        Assert.Equal([2, null], peers.Select(page => page.Next).ToList());
        Assert.Equal(3, peers.Sum(page => page.Entries.Count));
        Assert.Equal([3, 3], peers.Select(page => page.Total).ToList());

        // The vectors carry the shape that matters: the management client holds
        // records on both transports, a controller holds one, and a stored peer
        // the adapter cannot identify is reported as unknown rather than guessed.
        var all = peers.SelectMany(page => page.Entries).ToList();
        Assert.Equal(PeerRole.ManagementCompanion, all[0].Role);
        Assert.True(all[0].MultiTransport);
        Assert.Equal(PeerRole.PhysicalController, all[1].Role);
        Assert.Equal(PeerRole.Unknown, all[2].Role);

        var kbm = paging.GetProperty("kbm").EnumerateArray()
            .Select(item => ManagementProtocol.KbmMapPage(
                item.GetProperty("command").GetString()!,
                item.GetProperty("reply").GetRawText()))
            .ToList();
        Assert.Equal([true, false], kbm.Select(page => page.More).ToList());
        Assert.Equal(2, kbm.Sum(page => page.Bindings.Count));
    }

    [Fact]
    public void LogicalFramingIsNewlineTerminatedAndByteBounded()
    {
        Assert.Equal(Encoding.UTF8.GetBytes("ping\n"), ManagementProtocol.Frame("ping"));
        Assert.Equal(128, ManagementProtocol.Frame(new string('x', 127)).Length);
        Assert.Throws<ArgumentException>(() => ManagementProtocol.Frame(new string('x', 128)));
        Assert.Throws<ArgumentException>(() => ManagementProtocol.Frame("ping\nget"));
    }

    [Fact]
    public void BleChunksPreserveTheExactLogicalFrame()
    {
        var joined = BleManagementContract.CommandChunks("amiibo status", 5)
            .SelectMany(chunk => chunk)
            .ToArray();
        Assert.Equal(Encoding.UTF8.GetBytes("amiibo status\n"), joined);
    }

    [Fact]
    public void BleReplyAssemblerHandlesFragmentsCrlfAndPayloadLimit()
    {
        var assembler = new BleReplyAssembler();
        Assert.Null(assembler.Accept("{\"ok\":"u8));
        Assert.Equal("{\"ok\":true}", assembler.Accept("true}\r\n"u8));

        var boundary = new BleReplyAssembler();
        Assert.Null(boundary.Accept(Fill(511, (byte)'x')));
        Assert.Equal(new string('x', 511), boundary.Accept("\n"u8));

        var oversized = new BleReplyAssembler();
        Assert.Throws<ManagementReplyTooLargeException>(
            () => oversized.Accept(Fill(512, (byte)'x')));
    }

    [Fact]
    public void IdentityConfigurationControllerAndPersonalityFixturesDecode()
    {
        var info = Vector("info");
        Assert.Equal("picoswitch", ManagementProtocol.Firmware(info.Command, info.Reply).Id);

        var config = Vector("config");
        Assert.Equal(
            new RgbColor(4, 5, 6),
            ManagementProtocol.Config(config.Command, config.Reply).LeftAccent);

        var device = Vector("device");
        Assert.Equal(75, ManagementProtocol.Controller(device.Command, device.Reply).BatteryPercent);

        var personality = Vector("personality");
        Assert.Equal(
            Personality.Pro2,
            ManagementProtocol.PersonalityState(personality.Command, personality.Reply).Current);
    }

    [Fact]
    public void InputSourceFixturePreservesOwnershipFields()
    {
        var vector = Vector("inputSources");
        var state = ManagementProtocol.InputSources(vector.Command, vector.Reply);
        Assert.Equal(7, state.ActiveId);
        Assert.True(state.Explicit);
        Assert.Equal(2, Assert.Single(state.Sources).Connection);
    }

    [Fact]
    public void KbmProfilesCarryIdentityRevisionAndFingerprint()
    {
        var profiles = ManagementProtocol.KbmProfileList(
            "kbm profiles",
            """
            {"profiles":[
              {"id":2,"layout":"kb","name":"Splatoon","revision":3,"overrides":3,"fingerprint":123},
              {"id":3,"layout":"kbm","name":"Zelda","revision":1,"overrides":9,"fingerprint":456}
            ],"max":6,"more":false}
            """);

        Assert.Equal(2, profiles.Count);
        Assert.Equal(3, profiles[0].Revision);
        Assert.Equal(123, profiles[0].Fingerprint);
        // Default is NOT in the adapter's list: it is a template that consumes
        // no slot, and the client synthesises it.
        Assert.DoesNotContain(profiles, p => p.Builtin);

        // A row this build cannot make sense of is skipped, not shown as a
        // selectable profile the adapter would refuse. An id colliding with the
        // reserved Default is refused for the same reason.
        var partial = ManagementProtocol.KbmProfileList(
            "kbm profiles",
            """
            {"profiles":[
              {"id":2,"layout":"kb","name":"Real","revision":1,"overrides":0,"fingerprint":1},
              {"id":4,"layout":"future","name":"Nope","revision":1,"overrides":0,"fingerprint":2},
              {"id":5,"layout":"kb","name":"","revision":1,"overrides":0,"fingerprint":3},
              {"id":1,"layout":"kb","name":"Impostor","revision":1,"overrides":0,"fingerprint":4}
            ],"max":6,"more":false}
            """);
        Assert.Equal("Real", Assert.Single(partial).Name);
    }

    [Fact]
    public void KbmActiveReportsWhatIsReallyRunning()
    {
        var active = ManagementProtocol.KbmActive(
            "kbm active",
            """
            {"active":[
              {"layout":"kb","sourceId":2,"revision":3,"fingerprint":123,"matchesSaved":false},
              {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":456,"matchesSaved":true}
            ]}
            """);

        Assert.Equal(2, active.Count);
        // matchesSaved=false is the "saved but not applied" case: the id still
        // names the profile that produced the mapping, and the content no longer
        // matches what that profile now holds.
        Assert.False(active[0].MatchesSaved);
        Assert.Equal(2, active[0].SourceId);
        Assert.True(active[1].MatchesSaved);
        Assert.Equal(KbmProfileIds.Default, active[1].SourceId);
    }

    [Fact]
    public void KbmCommandsKeepTheirLayoutSpelling()
    {
        // `kb` and `kbm` name a LAYOUT and act on its REALIZED mapping, exactly
        // as their existing clients rely on. Keeping the spelling is what makes
        // the profile system invisible to a user who does not want it.
        Assert.Equal("kbm map kb 0", ManagementCommands.KbmMap(KbmLayout.Keyboard, 0));
        Assert.Equal("kbm reset kbm",
                     ManagementCommands.KbmReset(KbmLayout.KeyboardMouse));

        // Apply names a profile; `default` is the built-in template.
        Assert.Equal("kbm apply kb 2",
                     ManagementCommands.KbmApply(KbmLayout.Keyboard, 2));
        Assert.Equal("kbm apply kbm default",
                     ManagementCommands.KbmApply(KbmLayout.KeyboardMouse,
                                                 KbmProfileIds.Default));

        Assert.Equal("kbm profile rename 3 Zelda",
                     ManagementCommands.KbmProfileRename(3, "Zelda"));
        Assert.Equal("kbm profile dup 3 Zelda Copy",
                     ManagementCommands.KbmProfileDuplicate(3, "Zelda Copy"));
        Assert.Equal("kbm profile delete 3", ManagementCommands.KbmProfileDelete(3));
        Assert.Equal("kbm pmap 3 0", ManagementCommands.KbmProfileMap(3, 0));

        // The staged write. `new` creates; an id updates against its revision.
        Assert.Equal("kbm draft begin kb new 0 My mapping",
                     ManagementCommands.KbmDraftBegin(KbmLayout.Keyboard,
                                                      KbmProfileIds.None, 0,
                                                      "My mapping"));
        Assert.Equal("kbm draft begin kb 2 3 Work",
                     ManagementCommands.KbmDraftBegin(KbmLayout.Keyboard, 2, 3,
                                                      "Work"));
        Assert.Equal("kbm draft bind key:04 a",
                     ManagementCommands.KbmDraftBind(
                         new KbmSource(KbmSourceKind.Key, 4), KbmDestination.A));
        Assert.Equal("kbm draft mouse sensitivityx 512",
                     ManagementCommands.KbmDraftMouse(KbmMouseField.SensitivityX, 512));
        Assert.Equal("kbm draft commit", ManagementCommands.KbmDraftCommit);
        Assert.Equal("kbm draft abort", ManagementCommands.KbmDraftAbort);
    }

    [Fact]
    public void KbmStatusReadsTheCompositeIdentityFields()
    {
        // `group` and `source` have always been in the firmware's reply
        // (ns2_kbm_status.c) and were never read. They are what distinguishes
        // "the keyboard holds its role" from "the composite owns the console
        // slot" -- the two are not the same, and only the second one makes a
        // keypress reach the game.
        //
        // The shared protocol fixture predates them, so this uses a literal
        // reply; absence must stay tolerated, which the second case pins.
        var status = ManagementProtocol.KbmStatus(
            "kbm status",
            """
            {"mode":"keyboard","override":"auto","profile":"kb",
             "keyboard":true,"mouse":false,"nativeMouse":false,
             "keyboardConn":5,"mouseConn":0,"group":2,"source":7,
             "rejectedNoPeerKey":1,"rejectedUnclassified":2,"rejectedNoRole":3,
             "undecodedReports":4}
            """);

        Assert.Equal(2, status.GroupId);
        Assert.Equal(7, status.SourceId);

        // The formerly-silent admission outcomes, each in its own field: a
        // shifted argument list in the firmware formatter still emits valid
        // JSON, so distinct values are what catch it.
        Assert.Equal(1, status.RejectedNoPeerKey);
        Assert.Equal(2, status.RejectedUnclassified);
        Assert.Equal(3, status.RejectedNoRole);
        Assert.Equal(4, status.UndecodedReports);

        // An older adapter that omits them still parses; zero is also the real
        // value the firmware reports for "no composite" and "not owning".
        var older = ManagementProtocol.KbmStatus(
            Vector("kbmStatus").Command, Vector("kbmStatus").Reply);
        Assert.Equal(0, older.GroupId);
        Assert.Equal(0, older.SourceId);
        Assert.Equal(0, older.RejectedNoPeerKey);
        Assert.Equal(0, older.UndecodedReports);
    }

    [Fact]
    public void KbmFixturesDistinguishModeProfileNoneAndAdapterRanges()
    {
        var statusVector = Vector("kbmStatus");
        var status = ManagementProtocol.KbmStatus(statusVector.Command, statusVector.Reply);
        Assert.Equal(KbmMode.KeyboardMouse, status.Mode);
        Assert.Equal(KbmMode.Automatic, status.ModeOverride);

        var mapVector = Vector("kbmMap");
        var page = ManagementProtocol.KbmMapPage(mapVector.Command, mapVector.Reply);
        Assert.Equal(KbmDestination.LStickUp, page.Bindings[0].Destination);
        Assert.True(page.Bindings[^1].Custom);

        var mouseVector = Vector("kbmMouse");
        var mouse = ManagementProtocol.KbmMouse(mouseVector.Command, mouseVector.Reply);
        Assert.Equal(16, mouse.SensitivityMin);
        Assert.Equal(50, mouse.AntiDeadzoneMax);

        Assert.Equal(
            "kbm bind kb key:04 none",
            ManagementCommands.KbmBind(
                KbmLayout.Keyboard,
                new KbmSource(KbmSourceKind.Key, 4),
                KbmDestination.None));
        Assert.Equal(
            "kbm bind kb key:04 default",
            ManagementCommands.KbmBind(
                KbmLayout.Keyboard,
                new KbmSource(KbmSourceKind.Key, 4),
                null));
    }

    [Fact]
    public void BondFixtureExposesCursorAndTotal()
    {
        var vector = Vector("bondsPage");
        var page = ManagementProtocol.BondsPage(vector.Command, vector.Reply);
        Assert.Equal(2, page.Total);
        Assert.Equal(3, page.Next);
        Assert.Equal("001122334455", Assert.Single(page.Entries).Address);
    }

    [Fact]
    public void PeerFixtureExposesIdentityRoleAndCursorWithoutKeyMaterial()
    {
        var vector = Vector("peersPage");
        var page = ManagementProtocol.PeersPage(vector.Command, vector.Reply);
        Assert.Equal(3, page.Total);
        Assert.Equal(2, page.Next);
        Assert.Equal("p_1A2B3C4D", page.Entries[0].Id);
        Assert.Equal("DualSense Wireless Controller", page.Entries[1].Name);

        // Derived identity travels beside the claimed name, not instead of it, so
        // a non-Kotlin client inherits both halves of the naming hierarchy.
        Assert.Equal("Sony DualSense", page.Entries[1].Classification);
        Assert.Equal(1356, page.Entries[1].VendorId);
        Assert.Equal(3302, page.Entries[1].ProductId);

        // A peer the adapter cannot identify carries neither, and carries them as
        // absent rather than as empty strings.
        Assert.Null(page.Entries[0].Classification);

        // The gate, stated against the shared vector rather than only in code: the
        // management relationship is on the wire as management.
        Assert.Equal(PeerRole.ManagementCompanion, page.Entries[0].Role);

        AssertNoKeyMaterial(vector.Reply, "peer", "key", "ltk", "irk", "csrk");
    }

    [Fact]
    public void TheForgetVectorCarriesAVerifiedPostStateAndNoKeyMaterial()
    {
        var vector = Vector("peersForget");
        var outcome = ManagementProtocol.PeersForget(vector.Command, vector.Reply);
        Assert.Equal(PeerForgetResult.Removed, outcome.Result);

        // The claim that matters is the adapter's own re-read, not its intent.
        Assert.False(outcome.StillBonded);
        Assert.True(outcome.Transports.IsEmpty);

        AssertNoKeyMaterial(vector.Reply, "forget", "key", "ltk", "irk", "csrk");
    }

    [Fact]
    public void ThePairingVectorCarriesAGenerationACountdownAndNoIdentity()
    {
        var vector = Vector("pairingStatus");
        var status = ManagementProtocol.PairingStatus(vector.Command, vector.Reply);
        Assert.Equal(7L, status.Operation);
        Assert.Equal(PairingState.Discovering, status.State);
        Assert.True(status.Active);
        Assert.Equal(24000L, status.RemainingMillis);

        // Pairing status is progress, not an inventory: no address, no name, no key
        // material, so a non-Kotlin client inherits that guarantee too.
        AssertNoKeyMaterial(vector.Reply, "pairing", "addr", "name", "key", "ltk", "irk");
    }

    [Fact]
    public void AmiiboAndWakeFixturesDecodeExactSemantics()
    {
        var amiiboVector = Vector("amiiboStatus");
        var amiibo = ManagementProtocol.Amiibo(amiiboVector.Command, amiiboVector.Reply);
        Assert.True(amiibo.V3Loaded);
        Assert.Equal(42L, amiibo.Generation);

        var wakeVector = Vector("wakeStatus");
        var wake = ManagementProtocol.WakeStatus(wakeVector.Command, wakeVector.Reply);
        Assert.Equal(WakeResult.Advertised, wake.Result);
        Assert.Equal(1234L, wake.LastAttemptMs);
    }

    [Fact]
    public void QueuedSaveDoesNotClaimCompletedDurability()
    {
        var vector = Vector("saveQueued");
        var acknowledgement = ManagementProtocol.Acknowledgement(vector.Command, vector.Reply);
        Assert.True(acknowledgement.Queued);
        Assert.Equal(7L, acknowledgement.Requested);
        Assert.False(acknowledgement.Reenumerating);

        var statusVector = Vector("saveStatus");
        var status = ManagementProtocol.PersistenceStatus(statusVector.Command, statusVector.Reply);
        Assert.True(status.Pending);
        Assert.Equal(7L, status.Requested);
        Assert.Equal(6L, status.Completed);
    }

    [Fact]
    public void FirmwareErrorsRetainCommandCodeAndMessage()
    {
        var vector = Vector("adapterError");
        var error = Assert.Throws<AdapterCommandException>(
            () => ManagementProtocol.Acknowledgement(vector.Command, vector.Reply));
        Assert.Equal("amiibo commit", error.Command);
        Assert.Equal(8, error.Code);
        Assert.Equal("dirty", error.AdapterMessage);
    }

    [Fact]
    public void UnknownFieldsAreToleratedButRequiredFieldsFailClosed()
    {
        const string withFuture = """{"id":"picoswitch","version":"2.0","future":true}""";
        Assert.Equal("picoswitch", ManagementProtocol.Firmware("info", withFuture).Id);
        Assert.Throws<ManagementProtocolException>(() => ManagementProtocol.Firmware("info", "{}"));
    }

    [Fact]
    public void MalformedJsonAndInvalidCommandIdentifiersFailClosed()
    {
        Assert.Throws<ManagementProtocolException>(
            () => ManagementProtocol.Controller("device", "not-json"));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => ManagementCommands.SetPersonality(Personality.Config));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => ManagementCommands.KbmMap(KbmLayout.Keyboard, 33));
        Assert.Throws<ArgumentOutOfRangeException>(() => ManagementCommands.BondsPage(-1));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => ManagementCommands.AmiiboBegin(541, "12345678"));
        Assert.Throws<ArgumentException>(() => ManagementCommands.AmiiboBegin(540, "not-crc"));
        Assert.Equal("amiibo begin 540 A1B2C3D4", ManagementCommands.AmiiboBegin(540, "a1b2c3d4"));
    }

    [Fact]
    public void WrongJsonValueAndContainerTypesNormalizeToProtocolErrors()
    {
        Action[] malformed =
        [
            () => ManagementProtocol.Firmware("info", """{"id":[],"version":"2.0"}"""),
            () => ManagementProtocol.PersonalityState(
                "personality", """{"current":"pro2","available":{}}"""),
            () => ManagementProtocol.PersonalityState(
                "personality", """{"current":"pro2","available":[2]}"""),
            () => ManagementProtocol.Config(
                "get",
                """{"body_color":"red","joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}"""),
            () => ManagementProtocol.Config(
                "get",
                """{"body_color":["0",0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}"""),
            () => ManagementProtocol.IsVersionedBondResponse("bonds list", """{"v":"2","bonds":[]}"""),
            () => ManagementProtocol.InputSources(
                "input sources",
                """{"active":"0","pending":0,"explicit":false,"fresh":false,"transitions":0,"sources":[],"more":false}"""),
            () => ManagementProtocol.Acknowledgement("save", """{"ok":"true"}"""),
            () => ManagementProtocol.Acknowledgement("save", """{"error":[],"code":413}"""),
            () => ManagementProtocol.Acknowledgement("save", """{"error":"bad","code":"413"}"""),
        ];

        for (var index = 0; index < malformed.Length; index++)
        {
            var operation = malformed[index];
            var error = Record.Exception(operation);
            Assert.True(
                error is ManagementProtocolException,
                $"malformed case {index} threw {error?.GetType().Name ?? "nothing"}");
        }
    }

    [Fact]
    public void LanguageNeutralErrorFixturesFailThroughTheExpectedBoundary()
    {
        foreach (var element in Root.GetProperty("errors").EnumerateArray())
        {
            var command = element.GetProperty("command").GetString()!;
            var reply = element.GetProperty("replyText").GetString()!;
            switch (element.GetProperty("case").GetString())
            {
                case "malformedJson":
                case "incompleteInfo":
                    Assert.Throws<ManagementProtocolException>(
                        () => ManagementProtocol.Firmware(command, reply));
                    break;
                case "responseTooLarge":
                    var error = Assert.Throws<AdapterCommandException>(
                        () => ManagementProtocol.Acknowledgement(command, reply));
                    Assert.Equal(413, error.Code);
                    break;
                case "oddHex":
                    Assert.Throws<ManagementProtocolException>(
                        () => ManagementProtocol.ReadData(command, reply));
                    break;
                default:
                    throw new Xunit.Sdk.XunitException("Unknown error fixture case");
            }
        }
    }

    private static void AssertNoKeyMaterial(string reply, string label, params string[] forbidden)
    {
        foreach (var term in forbidden)
        {
            Assert.False(
                reply.Contains(term, StringComparison.OrdinalIgnoreCase),
                $"{label} vector must not contain '{term}'");
        }
    }

    private static byte[] Fill(int count, byte value)
    {
        var buffer = new byte[count];
        Array.Fill(buffer, value);
        return buffer;
    }

    private FixtureVector Vector(string name)
    {
        var value = Root.GetProperty("vectors").GetProperty(name);
        return new FixtureVector(
            value.GetProperty("command").GetString()!,
            value.GetProperty("reply").GetRawText());
    }

    private readonly record struct FixtureVector(string Command, string Reply);
}
