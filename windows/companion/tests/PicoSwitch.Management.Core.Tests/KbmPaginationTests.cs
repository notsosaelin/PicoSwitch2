using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The KB/M cursor-walk rules, one test per failure mode.
///
/// These exist because the adapter on the other end may be misbehaving or
/// half-upgraded, and the failure they prevent is not a crash — it is a client
/// that spins forever, or quietly assembles a mapping that is missing rows. A
/// keyboard map with a silently dropped binding is worse than an error: the user
/// rebinds a key that is already bound and cannot see why it does not stick.
///
/// That is not hypothetical. Firmware shipped that answered a fixed
/// <c>page * PAGE_SIZE</c> offset while emitting only as many rows as its byte
/// budget allowed; every page lost its last row, and the page reported only
/// "incomplete". The contract is now a CURSOR: <c>next</c> is the index of the
/// first item not in the reply, so the adapter — the only party that knows how
/// many rows it managed to serialize — states where to resume.
///
/// Every rule fails LOUDLY, with <see cref="ManagementPaginationException"/> or
/// <see cref="ManagementProtocolException"/>, so the caller can say the read
/// failed rather than presenting a partial map as complete.
/// </summary>
public sealed class KbmPaginationTests
{
    private const string FirstOfTwo =
        """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":1}""";

    private static Task AssertRejected(params (string Command, object Reply)[] script) =>
        Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(new ScriptedChannel(script))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));

    [Fact]
    public Task ADifferentProfileThanRequestedIsRejected() =>
        // Assembling this would put the keyboard-and-mouse layout's bindings under
        // the keyboard layout's name, and a bind against them would edit the
        // wrong mapping.
        AssertRejected(
            ("kbm map kb 0",
                """{"profile":"kbm","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":null}"""));

    [Fact]
    public Task ADifferentCursorThanRequestedIsRejected() =>
        // An echoed cursor that does not match means the adapter and the client
        // have lost track of each other; continuing would duplicate or skip rows.
        AssertRejected(
            ("kbm map kb 0", FirstOfTwo),
            ("kbm map kb 1",
                """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:05","dst":"b","custom":false}],"next":null}"""));

    [Fact]
    public async Task ANextThatSkipsPastTheRowsSentIsRejectedByTheDECODER()
    {
        // THE SHIPPED BUG, as the client sees it. One row emitted, but `next`
        // claims the walk should resume at 8 — so items 1..7 would never be
        // requested by anyone and the mapping would come back short.
        //
        // Caught at the DECODER, not the accumulator: the reply is internally
        // inconsistent, and the earliest possible rejection is the one that names
        // the real problem rather than a downstream symptom.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","profileId":1,"cursor":0,"total":26,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":8}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task ANextThatOverlapsRowsAlreadySentIsRejected()
    {
        // The mirror image: two rows sent from cursor 0 but `next` is 1, so item 1
        // would arrive twice. A duplicate binding is as wrong as a missing one and
        // far harder to notice, because the count can still look plausible.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","profileId":1,"cursor":0,"total":3,"bindings":[{"src":"key:04","dst":"a","custom":false},{"src":"key:05","dst":"b","custom":false}],"next":1}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task ASinglePageThatOvershootsItsOwnTotalIsRejectedByTheDECODER()
    {
        // More rows than the declared total. Malformed at the page level, so it
        // never reaches the accumulator.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":false},{"src":"key:05","dst":"b","custom":false}],"next":null}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public Task AChangingTotalMidWalkIsRejected() =>
        // Each reply is individually valid; the item count moved underneath the
        // walk. Only the accumulator can see this.
        AssertRejected(
            ("kbm map kb 0", FirstOfTwo),
            ("kbm map kb 1",
                """{"profile":"kb","profileId":1,"cursor":1,"total":9,"bindings":[{"src":"key:05","dst":"b","custom":false}],"next":null}"""));

    [Fact]
    public async Task AnEmptyReplyThatAsksToContinueIsRejected()
    {
        // The livelock case. An adapter that keeps saying "resume at N" while
        // sending nothing would spin the client with no diagnosis at all. Refused
        // at the decoder, because a reply with rows==0 and next!=null cannot be
        // satisfied by any client.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0", FirstOfTwo),
                ("kbm map kb 1",
                    """{"profile":"kb","profileId":1,"cursor":1,"total":2,"bindings":[],"next":1}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task ACursorThatDoesNotAdvanceIsRejected()
    {
        // `next` must move forward. Equal or backwards is a loop.
        //
        // Rejected at the DECODER rather than the accumulator, because `next` not
        // equalling cursor + rows is already an internally inconsistent reply —
        // the earliest and most specific place to catch it. The accumulator's own
        // non-advancing guard still exists for a reply that is self-consistent and
        // still fails to progress; WalkingStopsAtTheHardCeiling covers that.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","profileId":1,"cursor":0,"total":9,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":0}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task WalkingStopsAtTheHardCeiling()
    {
        // The remaining livelock guard: replies that each carry one row, advance
        // by one, and never finish. The bound is the firmware's own maximum item
        // count, so a legitimate mapping can always complete and a runaway cannot.
        var script = new List<(string, object)>();
        for (var cursor = 0; cursor <= KbmLimits.MaxMappingItems + 2; cursor++)
        {
            script.Add((
                $"kbm map kb {cursor}",
                $$"""{"profile":"kb","profileId":1,"cursor":{{cursor}},"total":9999,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":{{cursor + 1}}}"""));
        }

        await Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(new ScriptedChannel([.. script]))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task AFinalCountBelowTheTotalIsRejectedAndSaysByHowMuch()
    {
        // The adapter said two and sent one, then stopped. Publishing that as a
        // complete mapping would hide a binding the user cannot then remove.
        //
        // The message must carry the shortfall and the cursor trail: the version
        // that said only "incomplete" cost a hardware session to turn into a
        // diagnosis.
        var error = await Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":null}""")))
                .LoadKbmMappingAsync(KbmLayout.Keyboard));

        Assert.Contains("1 of 2 bindings", error.Message);
        Assert.Contains("kb", error.Message);
    }

    [Fact]
    public async Task ASingleCompleteReplyNeedsNoSecondRequest()
    {
        // The ordinary case, pinned so a future guard cannot make a second round
        // trip mandatory and double every read.
        var channel = new ScriptedChannel(
            ("kbm map kb 0",
                """{"profile":"kb","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":true}],"next":null}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmLayout.Keyboard);

        Assert.Single(mapping.Bindings);
        Assert.True(mapping.Loaded);
        channel.AssertDrained();
    }

    [Fact]
    public async Task AnEmptyProfileIsAValidCompleteAnswer()
    {
        // Zero bindings and zero total is a real state -- a mapping the user has
        // cleared -- and must not be mistaken for a failed read.
        var channel = new ScriptedChannel(
            ("kbm map kb 0",
                """{"profile":"kb","profileId":1,"cursor":0,"total":0,"bindings":[],"next":null}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmLayout.Keyboard);

        Assert.Empty(mapping.Bindings);
        Assert.True(mapping.Loaded);
        channel.AssertDrained();
    }

    [Fact]
    public async Task AMultiReplyWalkReconstructsEveryItemInOrder()
    {
        // The positive property the shipped bug violated: three replies of
        // differing sizes, and every item arrives exactly once, in order.
        var channel = new ScriptedChannel(
            ("kbm map kb 0",
                """{"profile":"kb","profileId":1,"cursor":0,"total":4,"bindings":[{"src":"key:04","dst":"a","custom":false},{"src":"key:05","dst":"b","custom":false}],"next":2}"""),
            ("kbm map kb 2",
                """{"profile":"kb","profileId":1,"cursor":2,"total":4,"bindings":[{"src":"key:06","dst":"x","custom":false}],"next":3}"""),
            ("kbm map kb 3",
                """{"profile":"kb","profileId":1,"cursor":3,"total":4,"bindings":[{"src":"key:07","dst":"y","custom":false}],"next":null}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmLayout.Keyboard);

        Assert.Equal(["key:04", "key:05", "key:06", "key:07"],
                     mapping.Bindings.Select(binding => binding.Source.Wire).ToList());
        channel.AssertDrained();
    }
}
