using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The Controller Link control plane, parsed from the exact JSON
/// <c>reply_clink_status()</c> emits in <c>src/config.c</c>.
/// </summary>
public sealed class ControllerLinkProtocolTests
{
    private const string Command = "clink status";

    private const string FullReply = """
        {"clink":{"active":true,"subscribed":true,"version":1,"frame_bytes":30,
        "att_mtu":517,"min_att_mtu":33,"mtu_ok":true,"generation":3,
        "frames":{"received":1200,"applied":1195,"stale":4,"short":0,"version":0,
        "opcode":1,"rejected_state":0},
        "outputs":{"sent":7,"failed":0},
        "neutralizations":1,"max_gap_ms":29}}
        """;

    [Fact]
    public void ParsesTheFullStatusBlock()
    {
        var state = ManagementProtocol.ControllerLink(Command, FullReply);

        Assert.True(state.Active);
        Assert.True(state.Subscribed);
        Assert.Equal(1, state.Version);
        Assert.Equal(30, state.FrameBytes);
        Assert.Equal(517, state.AttMtu);
        Assert.Equal(33, state.MinimumAttMtu);
        Assert.True(state.MtuOk);
        Assert.Equal(3, state.Generation);

        // The frame breakdown is what makes "did input arrive, was it parsed,
        // was it accepted" answerable without a hardware session.
        Assert.Equal(1200, state.FramesReceived);
        Assert.Equal(1195, state.FramesApplied);
        Assert.Equal(4, state.FramesStale);
        Assert.Equal(0, state.FramesShort);
        Assert.Equal(0, state.FramesVersion);
        Assert.Equal(1, state.FramesOpcode);
        Assert.Equal(0, state.FramesRejectedState);

        Assert.Equal(7, state.OutputsSent);
        Assert.Equal(0, state.OutputsFailed);
        Assert.Equal(1, state.Neutralizations);
        Assert.Equal(29, state.MaxGapMillis);
    }

    [Fact]
    public void ReportsAnMtuTooSmallToStream()
    {
        // The default ATT MTU cannot carry a 30-byte frame. The adapter states
        // both numbers so the companion refuses BEFORE streaming rather than
        // discovering a fragmenting MTU mid-game.
        var state = ManagementProtocol.ControllerLink(
            Command,
            """{"clink":{"active":false,"version":1,"frame_bytes":30,"att_mtu":23,"min_att_mtu":33,"mtu_ok":false}}""");

        Assert.False(state.Active);
        Assert.False(state.MtuOk);
        Assert.Equal(23, state.AttMtu);
        Assert.Equal(33, state.MinimumAttMtu);
    }

    [Fact]
    public void ToleratesAStatusBlockWithoutCounterGroups()
    {
        // A refusal reply carries the capability fields and may omit the
        // counters entirely. Missing groups must read as zero, not throw --
        // the caller needs the MTU verdict out of exactly this reply.
        var state = ManagementProtocol.ControllerLink(
            Command, """{"clink":{"active":false,"version":1,"att_mtu":517,"min_att_mtu":33,"mtu_ok":true}}""");

        Assert.True(state.MtuOk);
        Assert.Equal(0, state.FramesReceived);
        Assert.Equal(0, state.OutputsSent);
    }

    [Fact]
    public void RejectsAReplyThatIsNotAControllerLinkStatus()
    {
        // A reply without the object must not decode to an all-default state:
        // that would read as "supported, idle, MTU 0" and send the companion
        // down the refuse path for the wrong reason.
        Assert.ThrowsAny<Exception>(() =>
            ManagementProtocol.ControllerLink(Command, """{"ok":true}"""));
        Assert.ThrowsAny<Exception>(() =>
            ManagementProtocol.ControllerLink(Command, """{"clink":"status"}"""));
    }

    [Fact]
    public void CommandStringsMatchTheFirmwareDispatch()
    {
        // src/config.c dispatches on exactly these, and `clink status` is the
        // read-only probe an older firmware answers with `unknown command`.
        Assert.Equal("clink start", ManagementCommands.ControllerLinkStart);
        Assert.Equal("clink stop", ManagementCommands.ControllerLinkStop);
        Assert.Equal("clink status", ManagementCommands.ControllerLinkStatus);
    }
}
