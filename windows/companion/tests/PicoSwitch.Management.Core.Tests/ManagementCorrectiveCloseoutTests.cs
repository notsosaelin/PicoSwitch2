using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// Capability degradation and error propagation across a full refresh.
///
/// The rule under test is asymmetric on purpose: an adapter that says "I do not
/// have that command" must degrade one capability, while a transport failure,
/// a malformed reply or a broken pagination must propagate and fail the refresh.
/// Smoothing the second group into the first would render a half-read adapter as
/// a healthy one with fewer features.
/// </summary>
public sealed class ManagementCorrectiveCloseoutTests
{
    [Fact]
    public async Task UnknownOptionalFirmwareFamiliesAreUnsupportedWithoutFailingRefresh()
    {
        var channel = RefreshChannel(
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"));

        var refresh = await new ManagementClient(channel).RefreshAllAsync();
        var capabilities = refresh.Snapshot.Capabilities;
        Assert.Equal(CapabilityState.Unsupported, capabilities.Personality);
        Assert.Equal(CapabilityState.Unsupported, capabilities.Amiibo);
        Assert.Equal(CapabilityState.Unsupported, capabilities.Bonds);
        Assert.Equal(CapabilityState.Unsupported, capabilities.ActiveInput);
        Assert.Equal(CapabilityState.Unsupported, capabilities.Kbm);
        channel.AssertDrained();
    }

    [Theory]
    [InlineData("unavailable")]
    [InlineData("unavailable in config mode")]
    [InlineData("command unavailable over Bluetooth")]
    public async Task ExplicitFirmwareUnavailableIsAnUnsupportedOptionalCapability(string message)
    {
        var channel = RefreshChannel(
            Unsupported(message),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"));

        var refresh = await new ManagementClient(channel).RefreshAllAsync();
        Assert.Equal(CapabilityState.Unsupported, refresh.Snapshot.Capabilities.Personality);
    }

    [Fact]
    public async Task AnAdapterErrorThatIsNotAnUnsupportedShapePropagates()
    {
        // "busy" is a real failure, not a missing feature. Treating every adapter
        // error as a degraded capability would hide it.
        var channel = new ScriptedChannel(
            [.. RequiredRefresh(), ("personality", """{"error":"busy","code":9}""")]);

        var error = await Assert.ThrowsAsync<AdapterCommandException>(
            () => new ManagementClient(channel).RefreshAllAsync());
        Assert.Equal("busy", error.AdapterMessage);
    }

    [Fact]
    public async Task TransportTimeoutChannelLossAndInvalidSessionPropagate()
    {
        foreach (var message in new[]
                 {
                     "timed out", "channel failed", "adapter disconnected", "session invalidated",
                 })
        {
            var expected = new ManagementException(message);
            var channel = new ScriptedChannel([.. RequiredRefresh(), ("personality", expected)]);
            var actual = await Record.ExceptionAsync(
                () => new ManagementClient(channel).RefreshAllAsync());
            Assert.Same(expected, actual);
        }
    }

    [Fact]
    public async Task MalformedAndOversizedOptionalRepliesPropagate()
    {
        var malformed = new ScriptedChannel([.. RequiredRefresh(), ("personality", "{broken")]);
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(malformed).RefreshAllAsync());

        var tooLarge = new ManagementReplyTooLargeException("reply too large");
        var oversized = new ScriptedChannel([.. RequiredRefresh(), ("personality", tooLarge)]);
        Assert.Same(
            tooLarge,
            await Record.ExceptionAsync(() => new ManagementClient(oversized).RefreshAllAsync()));
    }

    [Fact]
    public async Task OptionalBondPaginationFailurePropagates()
    {
        var channel = new ScriptedChannel(
        [
            .. RequiredRefresh(),
            ("personality", Unsupported("unknown command")),
            ("amiibo status", Unsupported("unknown command")),
            ("mgmt status", Unsupported("unknown command")),
            ("bonds list", """{"v":2,"total":2,"bonds":[{"i":0,"addr":"00"}],"next":1}"""),
            ("bonds list v2 1", """{"v":2,"total":3,"bonds":[{"i":1,"addr":"11"}],"next":null}"""),
        ]);

        await Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(channel).RefreshAllAsync());
    }

    [Fact]
    public async Task SupportedKbmIsAvailableAndAKbmTransactionFailurePropagates()
    {
        var supported = RefreshChannel(
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            ManagementWorkflowTests.KbmStatusReply("kbmouse", "auto", "kbm"),
            ManagementWorkflowTests.KbmCountersReply(),
            ManagementWorkflowTests.KbmMouseReply());

        var refresh = await new ManagementClient(supported).RefreshAllAsync();
        Assert.Equal(CapabilityState.Available, refresh.Snapshot.Capabilities.Kbm);
        Assert.Equal(KbmMode.KeyboardMouse, refresh.KbmStatus?.Mode);

        var failure = new ManagementException("KBM session failed");
        var failed = RefreshChannel(
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            Unsupported("unknown command"),
            failure);

        Assert.Same(
            failure,
            await Record.ExceptionAsync(() => new ManagementClient(failed).RefreshAllAsync()));
    }

    [Fact]
    public async Task WakeWorkflowRetainsFirmwareLastAttemptTimestamp()
    {
        var clock = new VirtualClock();
        var channel = new ScriptedChannel(
            ("wake", """{"ok":true}"""),
            ("wake status", """{"result":"advertised","consoleAsleep":true,"identityValid":true,"attempts":2,"lastAttemptMs":1234}"""));

        var status = await new ManagementClient(channel, clock.Now, clock.Delay).WakeConsoleAsync();
        Assert.Equal(1234L, status.LastAttemptMs);
    }

    [Fact]
    public async Task AnUnsupportedWakeStatusIsNotReportedAsAFailedWake()
    {
        var clock = new VirtualClock();
        var channel = new ScriptedChannel(
            ("wake", """{"ok":true}"""),
            ("wake status", Unsupported("unknown command")));

        var status = await new ManagementClient(channel, clock.Now, clock.Delay).WakeConsoleAsync();
        Assert.Equal(WakeResult.Unknown, status.Result);
    }

    [Fact]
    public async Task SaveCompletionPollingFollowsTheAcknowledgedRequestIdentity()
    {
        var clock = new VirtualClock();
        var channel = new ScriptedChannel(
            ("save", """{"ok":true,"queued":true,"requested":7}"""),
            ("save status", """{"pending":true,"requested":7,"completed":6}"""),
            ("save status", """{"pending":false,"requested":7,"completed":7}"""));

        var status = await new ManagementClient(channel, clock.Now, clock.Delay).SaveAndAwaitAsync();
        Assert.False(status.Pending);
        Assert.Equal(7L, status.Completed);
        channel.AssertDrained();
    }

    [Fact]
    public async Task LaterAutomaticSaveDoesNotHideCompletionOfTheRequestedSave()
    {
        var clock = new VirtualClock();
        var channel = new ScriptedChannel(
            ("save status", """{"pending":true,"requested":8,"completed":7}"""));

        var status = await new ManagementClient(channel, clock.Now, clock.Delay).AwaitPersistenceAsync(
            new PersistenceAcknowledgement(PersistenceState.Queued, RequestId: 7));
        Assert.True(status.Pending);
        Assert.Equal(8L, status.Requested);
    }

    [Fact]
    public async Task PersistenceCompletionSurvivesACounterWraparound()
    {
        // The completed counter is uint32 on the wire. A save requested just below
        // the wrap must not read as eternally pending once the counter rolls over.
        var clock = new VirtualClock();
        var channel = new ScriptedChannel(
            ("save status", """{"pending":false,"requested":2,"completed":2}"""));

        var status = await new ManagementClient(channel, clock.Now, clock.Delay).AwaitPersistenceAsync(
            new PersistenceAcknowledgement(PersistenceState.Queued, RequestId: 0xFFFF_FFFEL));
        Assert.False(status.Pending);
    }

    [Fact]
    public async Task LegacySaveAcknowledgementCannotClaimAuthoritativeCompletion()
    {
        var channel = new ScriptedChannel(("save", """{"ok":true,"queued":true}"""));
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(channel).SaveAndAwaitAsync());
    }

    [Fact]
    public async Task SaveCompletionPollingHasABoundedTimeout()
    {
        var clock = new VirtualClock();
        var channel = new ConstantChannel(
            "save status",
            """{"pending":true,"requested":1,"completed":0}""");

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => new ManagementClient(channel, clock.Now, clock.Delay).AwaitPersistenceAsync(
                new PersistenceAcknowledgement(PersistenceState.Queued, RequestId: 1),
                timeoutMillis: 250));

        Assert.Contains("did not finish settings persistence in time", error.Message);

        // Bounded in polls as well as in wall clock: 250 ms at a 100 ms poll is
        // three reads, not an unbounded spin against a stuck adapter.
        Assert.InRange(channel.Calls, 1, 4);
    }

    private static ScriptedChannel RefreshChannel(params object[] optionalReplies)
    {
        string[] commands =
        [
            "personality",
            "amiibo status",
            "mgmt status",
            "bonds list",
            "input sources",
            "kbm status",
            "kbm counters",
            "kbm mouse",
        ];

        var exchanges = new List<(string, object)>(RequiredRefresh());
        for (var index = 0; index < optionalReplies.Length; index++)
        {
            exchanges.Add((commands[index], optionalReplies[index]));
        }

        return new ScriptedChannel([.. exchanges]);
    }

    private static (string Command, object Reply)[] RequiredRefresh() =>
    [
        ("info", """{"id":"picoswitch","version":"2.0"}"""),
        ("get", """{"body_color":[0,0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}"""),
        ("device", "{}"),
    ];

    private static string Unsupported(string message) => $$"""{"error":"{{message}}"}""";
}
