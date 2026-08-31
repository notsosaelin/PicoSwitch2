using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Management.Core.Tests;

/// <summary>
/// The carrier policies both clients implement.
/// </summary>
/// <remarks>
/// These describe the ADAPTER's behaviour, not either client's, so the two
/// clients must agree about them. The Kotlin <c>ManagementRetryPolicyTest</c>
/// asserts the same properties against the mirrored implementation.
/// </remarks>
public sealed class ManagementCarrierPolicyTests
{
    /* ------------------------------------------------------------ turnaround */

    /// <summary>
    /// The gap exists because the adapter bridges commands through a single
    /// slot, and a write issued before it is free is accepted by the radio and
    /// never answered.
    /// </summary>
    [Fact]
    public void ACommandSentTooSoonAfterAReplyIsHeldBack()
    {
        Assert.Equal(100L, ManagementTurnaroundPolicy.DelayMillis(1_000L, 1_000L));
        Assert.Equal(60L, ManagementTurnaroundPolicy.DelayMillis(1_040L, 1_000L));
        Assert.Equal(1L, ManagementTurnaroundPolicy.DelayMillis(1_099L, 1_000L));
    }

    [Fact]
    public void OnceTheGapHasElapsedNothingIsAdded()
    {
        Assert.Equal(0L, ManagementTurnaroundPolicy.DelayMillis(1_100L, 1_000L));
        Assert.Equal(0L, ManagementTurnaroundPolicy.DelayMillis(9_000L, 1_000L));
    }

    /// <summary>
    /// No reply yet means no gap: the first command of a session has nothing to
    /// wait behind.
    /// </summary>
    [Fact]
    public void TheFirstCommandOfASessionWaitsForNothing()
    {
        Assert.Equal(0L, ManagementTurnaroundPolicy.DelayMillis(1_000L, 0L));
        Assert.Equal(0L, ManagementTurnaroundPolicy.DelayMillis(1_000L, -1L));
    }

    /// <summary>A clock that appears to run backwards must not produce a wait.</summary>
    [Fact]
    public void AReplyStampedInTheFutureIsIgnored()
    {
        Assert.Equal(0L, ManagementTurnaroundPolicy.DelayMillis(1_000L, 2_000L));
    }

    /* ----------------------------------------------------------------- retry */

    [Fact]
    public void ChunkWritesAreRepeatableBecauseTheyCarryAnAbsoluteOffset()
    {
        Assert.True(ManagementRetryPolicy.IsRepeatable("amiibo chunk 0 00112233"));
        Assert.True(ManagementRetryPolicy.IsRepeatable("amiibo chunk 288 aabbccdd"));
    }

    [Fact]
    public void ReadsAndStatusAreRepeatableBecauseTheyChangeNothing()
    {
        Assert.True(ManagementRetryPolicy.IsRepeatable("amiibo read 64 32"));
        Assert.True(ManagementRetryPolicy.IsRepeatable("amiibo status"));
    }

    /// <summary>
    /// The dangerous ones. A timeout does NOT mean the command was not executed,
    /// so anything whose second execution would differ must end the session
    /// rather than be papered over.
    /// </summary>
    [Fact]
    public void StagingAndPublishingCommandsAreNeverRepeated()
    {
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo begin 540 b136cf6f"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo commit"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo commit save2"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo persist"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo downloaded"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo cancel"));
    }

    [Fact]
    public void AnythingUnrecognisedIsTreatedAsUnsafe()
    {
        Assert.False(ManagementRetryPolicy.IsRepeatable("reenumerate"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("save"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("kbm mode keyboard"));
        Assert.False(ManagementRetryPolicy.IsRepeatable(""));
    }

    /// <summary>The trailing space is what stops a future command inheriting a retry.</summary>
    [Fact]
    public void MatchingIsOnAWholeCommandWord()
    {
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo chunkmode on"));
        Assert.False(ManagementRetryPolicy.IsRepeatable("amiibo readback"));
    }

    [Fact]
    public void MatchingToleratesCaseAndSurroundingSpace()
    {
        Assert.True(ManagementRetryPolicy.IsRepeatable("  AMIIBO CHUNK 0 00  "));
    }

    [Fact]
    public void RetriesAreBounded()
    {
        Assert.InRange(ManagementRetryPolicy.MaxRetries, 1, 3);
    }

    /* ------------------------------------------------------------ fragmenting */

    /// <summary>
    /// A negotiated MTU must actually shorten the command into fewer writes.
    /// </summary>
    /// <remarks>
    /// The defect this pins: both clients asked for a large MTU, recorded the
    /// answer, and then fragmented to 20 bytes anyway — so an 81-byte chunk
    /// command was five sequential ATT writes instead of one.
    /// </remarks>
    [Fact]
    public void ANegotiatedMtuIsUsedForFragmentation()
    {
        Assert.Equal(244, BleManagementContract.AttPayloadFor(247));
        Assert.Equal(514, BleManagementContract.AttPayloadFor(517));

        var command = "amiibo chunk 288 " + new string('a', 64);
        Assert.Single(BleManagementContract.CommandChunks(
            command, BleManagementContract.AttPayloadFor(247)));
        Assert.True(BleManagementContract.CommandChunks(
            command, BleManagementContract.AttPayloadWithDefaultMtu).Count > 4);
    }

    /// <summary>A refused or absent negotiation must behave exactly as before.</summary>
    [Fact]
    public void AnUnraisedMtuFallsBackToTheDefaultPayload()
    {
        Assert.Equal(20, BleManagementContract.AttPayloadFor(BleManagementContract.DefaultAttMtu));
        Assert.Equal(20, BleManagementContract.AttPayloadFor(0));
        Assert.Equal(20, BleManagementContract.AttPayloadFor(-1));
    }
}
