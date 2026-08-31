using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// END-TO-END coverage of the resident upload: the real page-level operation,
/// through the real <see cref="AdapterConnectionService"/>, the real
/// <c>AdapterRepository</c> and the real <c>ManagementClient</c>, down to the
/// exact bytes a transport would write.
///
/// ## Why this exists rather than more builder unit tests
///
/// The KB/M pagination defect of 2026-08-30 escaped a green suite because the
/// fixtures and the implementation shared one misunderstanding: every command
/// builder was tested in isolation and each one was right about a contract that
/// was wrong. Nothing asserted the CONVERSATION.
///
/// So what is pinned here is the ordered command sequence, in full, for a
/// realistic profile — not that a builder produces a string. If the transaction
/// gains a command, loses one, reorders them, or starts expanding the canonical
/// defaults into per-binding writes, this fails and names the difference.
///
/// The replies are the firmware's own JSON, in the same shapes
/// <c>tools/fixtures/management/kbm-wire-corpus.json</c> carries, and the
/// readback fingerprint is computed by the shared digest rather than pasted, so
/// a change to the canonical form cannot be absorbed by editing a constant.
/// </summary>
public sealed class KbmResidentUploadTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";
    private const string Ok = """{"ok":true}""";

    /// <summary>
    /// A profile of the size Windows actually uploads: twelve overrides across
    /// the keyboard and both mouse buttons, including the longest destination
    /// names, and non-default tuning on every profile-owned mouse field.
    ///
    /// Deliberately not two bindings. The failure being guarded against appeared
    /// partway through a long run of `kbm draft bind` commands, and a two-entry
    /// profile cannot express command ordering, the mouse block's position after
    /// the binds, or the sparse-versus-expanded question at all.
    /// </summary>
    private static KbmLocalProfile Realistic() => new()
    {
        Id = "11111111-1111-1111-1111-111111111111",
        Layout = KbmLayout.Keyboard,
        Name = "Competitive Loadout",
        Bindings = new ValueList<KbmBinding>(
        [
            Bind(0x04, KbmDestination.LStickLeft),
            Bind(0x07, KbmDestination.LStickRight),
            Bind(0x16, KbmDestination.LStickDown),
            Bind(0x1A, KbmDestination.LStickUp),
            Bind(0x1B, KbmDestination.B),
            Bind(0x1D, KbmDestination.A),
            Bind(0x06, KbmDestination.X),
            Bind(0x19, KbmDestination.Y),
            Bind(0x2C, KbmDestination.Zr),
            Bind(0x3A, KbmDestination.Home),
            Bind(0x3B, KbmDestination.Capture),
            new KbmBinding(new KbmSource(KbmSourceKind.MouseButton, 1),
                           KbmDestination.RStickRight, Custom: true),
        ]),
        Mouse = new KbmMouseConfig
        {
            SensitivityX = 512,
            SensitivityY = 448,
            VelocityWindowMs = 12,
            InvertX = false,
            InvertY = true,
            AntiDeadzone = 24,
        },
    };

    private static KbmBinding Bind(int usage, KbmDestination destination) =>
        new(new KbmSource(KbmSourceKind.Key, usage), destination, Custom: true);

    /// <summary>The digest the adapter must report back for this content.</summary>
    private static long FingerprintOf(KbmLocalProfile profile) =>
        KbmFingerprint.Compute(
            profile.Layout,
            KbmFingerprint.Canonical(profile.Bindings),
            profile.Mouse);

    /// <summary>
    /// The whole transaction, in order, as the client should emit it.
    ///
    /// Written out rather than generated, so a change to the grammar has to be
    /// made here deliberately instead of being mirrored automatically by the code
    /// under test.
    /// </summary>
    private static string[] ExpectedSequence(int position, string name) =>
    [
        $"kbm draft begin kb pos:{position} 0 {name}",
        "kbm draft bind key:04 lstick_left",
        "kbm draft bind key:07 lstick_right",
        "kbm draft bind key:16 lstick_down",
        "kbm draft bind key:1A lstick_up",
        "kbm draft bind key:1B b",
        "kbm draft bind key:1D a",
        "kbm draft bind key:06 x",
        "kbm draft bind key:19 y",
        "kbm draft bind key:2C zr",
        "kbm draft bind key:3A home",
        "kbm draft bind key:3B capture",
        "kbm draft bind mouse:1 rstick_right",
        "kbm draft mouse sensitivityx 512",
        "kbm draft mouse sensitivityy 448",
        "kbm draft mouse recenter 12",
        "kbm draft mouse invertx 0",
        "kbm draft mouse inverty 1",
        "kbm draft mouse antideadzone 24",
        "kbm draft commit",
        // The readback. `kbm active` is part of it, not an extra: the bank list
        // and what each layout is actually realizing are read as one unit, which
        // is what lets the page say "resident updated — activate to use it"
        // truthfully instead of implying the console changed.
        "kbm profiles 0",
        "kbm active",
    ];

    /// <summary>Everything the KB/M page reads when it opens.</summary>
    private static void ScriptFullRead(ConnectionServiceFixture fixture)
    {
        fixture.Transport.Replies["kbm status"] =
            """{"ok":true,"mode":"keyboard","override":"auto","profile":"kb","keyboard":true,"mouse":false,"nativeMouse":false}""";
        fixture.Transport.Replies["kbm counters"] =
            """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}""";
        fixture.Transport.Replies["kbm mouse"] =
            """{"ok":true,"sensitivityX":256,"sensitivityY":256,"recenterMs":8,"invertX":false,"invertY":false,"antiDeadzone":10,"sensitivityMin":64,"sensitivityMax":1024,"recenterMinMs":2,"recenterMaxMs":32,"antiDeadzoneMax":100}""";
        fixture.Transport.Replies["kbm switches"] =
            """{"switches":[],"positions":3}""";
        fixture.Transport.Replies["kbm active"] =
            """
            {"active":[
              {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,"matchesSaved":true},
              {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
            ]}
            """;
        fixture.Transport.Replies["kbm map kb 0"] =
            """{"ok":true,"profile":"kb","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":true}],"next":null}""";
        fixture.Transport.Replies["kbm map kbm 0"] =
            """{"ok":true,"profile":"kbm","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"key:05","dst":"b","custom":true}],"next":null}""";
    }

    private static void ScriptUpload(
        ConnectionServiceFixture fixture,
        KbmLocalProfile profile,
        int position,
        long? readbackFingerprint = null,
        int overrides = 12)
    {
        // Only the staged writes answer with a bare ok; the commit and the
        // readback carry real payloads and are scripted below.
        foreach (var command in ExpectedSequence(position, profile.Name)
                     .Where(command => command.StartsWith("kbm draft ", StringComparison.Ordinal)))
        {
            fixture.Transport.Replies[command] = Ok;
        }

        fixture.Transport.Replies["kbm draft commit"] =
            """{"ok":true,"id":4,"revision":1}""";
        fixture.Transport.Replies["kbm draft abort"] = Ok;
        fixture.Transport.Replies["kbm active"] =
            """
            {"active":[
              {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,"matchesSaved":true},
              {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
            ]}
            """;

        var fingerprint = readbackFingerprint ?? FingerprintOf(profile);
        fixture.Transport.Replies["kbm profiles 0"] =
            $$"""
            {"cursor":0,"total":1,"max":6,"profiles":[
              {"id":4,"position":{{position}},"layout":"kb","name":"{{profile.Name}}",
               "revision":1,"overrides":{{overrides}},"fingerprint":{{fingerprint}}}
            ],"next":null}
            """;
    }

    [Fact]
    public async Task TheWholeTransactionIsBeginBindsMouseCommitReadback()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1);

        await fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile);

        // Exact, and in order. Not "contains begin and commit": the defect class
        // this guards is a transaction that sends the right commands in the wrong
        // shape, and a containment assertion cannot see that.
        Assert.Equal(ExpectedSequence(1, profile.Name), fixture.Transport.Sent);
    }

    [Fact]
    public async Task OnlyTheUsersOverridesAreUploadedNotTheCanonicalTable()
    {
        // The firmware seeds a draft from the layout's Default and applies the
        // binds on top, so the client only has to send what DIFFERS. A client that
        // expanded the effective mapping would send one command per key on the
        // keyboard, turn a one-second upload into a minute of round trips, and
        // freeze the profile against every future change to the defaults.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1);

        await fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile);

        var binds = fixture.Transport.Sent
            .Count(command => command.StartsWith("kbm draft bind ", StringComparison.Ordinal));
        Assert.Equal(profile.Bindings.Count, binds);
    }

    [Fact]
    public async Task NothingIsWrittenToFlashUntilCommit()
    {
        // The staged transaction exists because a loop of `kbm bind` writes is not
        // a transaction: every step erases a flash sector, and a disconnect
        // halfway leaves half of one mapping and half of another. Nothing before
        // commit may touch the stored or realized state, and the legacy
        // immediate-write verbs must not appear on this path at all.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 2);

        await fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 2, profile);

        Assert.DoesNotContain(fixture.Transport.Sent, command =>
            command.StartsWith("kbm bind ", StringComparison.Ordinal) ||
            command.StartsWith("kbm mouse ", StringComparison.Ordinal) ||
            command.StartsWith("kbm apply ", StringComparison.Ordinal) ||
            command.StartsWith("kbm boot ", StringComparison.Ordinal));
    }

    [Fact]
    public async Task AssignmentDoesNotActivateWhatTheConsoleIsRunning()
    {
        // Updating a stored copy must not change gameplay mid-session. Activation
        // is a separate, explicit act.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1);

        await fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile);

        Assert.DoesNotContain("kbm apply kb 4", fixture.Transport.Sent);
    }

    [Fact]
    public async Task ACommandThatNeverAnswersAbortsTheDraftAndFails()
    {
        // The reported failure, reproduced at this layer: the seventh command of
        // the transaction goes unanswered and the session is retired.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1);
        fixture.Transport.Failures["kbm draft bind key:1D a"] =
            new ManagementException(
                "The adapter did not answer 'kbm draft bind key:1D a' within 10000 ms.");

        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile));

        // The draft is discarded rather than left staged for the next caller to
        // commit by accident, and the position is never created half-formed.
        Assert.Contains("kbm draft abort", fixture.Transport.Sent);
        Assert.DoesNotContain("kbm draft commit", fixture.Transport.Sent);
    }

    [Fact]
    public async Task AnEmptyBankPositionAfterCommitIsAFailureNotASuccess()
    {
        // The adapter answered `ok` to the commit and then reported the position
        // still empty. Trusting the acknowledgement is what let the page print
        // "'X' is now Profile 1 on the adapter" over a bank that showed Empty.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1);
        fixture.Transport.Replies["kbm profiles 0"] =
            """{"cursor":0,"total":0,"max":6,"profiles":[],"next":null}""";

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile));

        Assert.Contains("still empty", error.Message);
    }

    [Fact]
    public async Task ContentThatDoesNotMatchWhatWasSentIsAFailure()
    {
        // A transfer that lost an entry commits cleanly and reads back as a
        // perfectly valid profile with the right name in the right position. Only
        // the content digest can tell the difference.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        ScriptUpload(fixture, profile, position: 1,
                     readbackFingerprint: FingerprintOf(profile) ^ 1L,
                     overrides: 11);

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile));

        Assert.Contains("does not match yours", error.Message);
    }

    [Fact]
    public async Task ReplacingAnOccupiedPositionCarriesItsCurrentRevision()
    {
        // "Update adapter copy". The occupant's revision goes out with the begin,
        // so a change made from another client is a refusal rather than a silent
        // overwrite.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var profile = Realistic();
        var fingerprint = FingerprintOf(profile);

        // Seed the bank through the page's own read, so the revision the client
        // carries is the one the adapter reported rather than one a test injected.
        ScriptFullRead(fixture);
        fixture.Transport.Replies["kbm profiles 0"] =
            """
            {"cursor":0,"total":1,"max":6,"profiles":[
              {"id":4,"position":1,"layout":"kb","name":"Competitive Loadout",
               "revision":5,"overrides":12,"fingerprint":1}
            ],"next":null}
            """;
        await fixture.Service.RefreshKeyboardMouseAsync();

        foreach (var command in ExpectedSequence(1, profile.Name)
                     .Where(command => command.StartsWith("kbm draft ", StringComparison.Ordinal)))
        {
            fixture.Transport.Replies[command] = Ok;
        }

        fixture.Transport.Replies["kbm draft begin kb pos:1 5 Competitive Loadout"] = Ok;
        fixture.Transport.Replies["kbm draft commit"] =
            """{"ok":true,"id":4,"revision":6}""";
        fixture.Transport.Replies["kbm draft abort"] = Ok;
        fixture.Transport.Replies["kbm profiles 0"] =
            $$"""
            {"cursor":0,"total":1,"max":6,"profiles":[
              {"id":4,"position":1,"layout":"kb","name":"Competitive Loadout",
               "revision":6,"overrides":12,"fingerprint":{{fingerprint}}}
            ],"next":null}
            """;

        fixture.Transport.ResetCounters();
        await fixture.Service.AssignKbmPositionAsync(KbmLayout.Keyboard, 1, profile);

        Assert.Equal("kbm draft begin kb pos:1 5 Competitive Loadout",
                     fixture.Transport.Sent[0]);
    }
}
