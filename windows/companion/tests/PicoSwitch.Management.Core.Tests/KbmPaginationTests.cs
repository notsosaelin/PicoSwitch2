using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The KB/M paging anti-livelock rules (§13.3), one test per failure mode.
///
/// These exist because the adapter on the other end may be misbehaving or
/// half-upgraded, and the failure they prevent is not a crash — it is a client
/// that spins forever, or quietly assembles a mapping that is missing rows. A
/// keyboard map with a silently dropped binding is worse than an error: the user
/// rebinds a key that is already bound and cannot see why it does not stick.
///
/// Every rule fails LOUDLY, with <see cref="ManagementPaginationException"/>, so
/// the caller can say the read failed rather than presenting a partial map as
/// complete.
/// </summary>
public sealed class KbmPaginationTests
{
    private const string FirstOfTwo =
        """{"profile":"kb","page":0,"pageSize":1,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"more":true}""";

    private static Task AssertRejected(params (string Command, object Reply)[] script) =>
        Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(new ScriptedChannel(script))
                .LoadKbmMappingAsync(KbmProfile.Keyboard));

    [Fact]
    public Task ADifferentProfileThanRequestedIsRejected() =>
        // Assembling this would put the keyboard-and-mouse profile's bindings under
        // the keyboard profile's name, and a bind against them would edit the
        // wrong profile.
        AssertRejected(
            ("kbm map kb 0",
                """{"profile":"kbm","page":0,"pageSize":1,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":false}],"more":false}"""));

    [Fact]
    public Task ADifferentPageThanRequestedIsRejected() =>
        // A page number that does not match means the adapter and the client have
        // lost track of each other; continuing would duplicate or skip rows.
        AssertRejected(
            ("kbm map kb 0", FirstOfTwo),
            ("kbm map kb 1",
                """{"profile":"kb","page":0,"pageSize":1,"total":2,"bindings":[{"src":"key:05","dst":"b","custom":false}],"more":false}"""));

    [Fact]
    public async Task ASinglePageThatOvershootsItsOwnTotalIsRejectedByTheDECODER()
    {
        // Two guards, at different layers, catching different over-counts. This one
        // is a malformed PAGE -- more rows than the page or the total allows -- and
        // it never reaches the paginator.
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(new ScriptedChannel(
                ("kbm map kb 0",
                    """{"profile":"kb","page":0,"pageSize":2,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":false},{"src":"key:05","dst":"b","custom":false}],"more":false}""")))
                .LoadKbmMappingAsync(KbmProfile.Keyboard));
    }

    [Fact]
    public Task AnACCUMULATEDOvercountAcrossPagesIsRejectedByThePAGINATOR() =>
        // Each page is individually valid; together they exceed the declared total.
        // Only the paginator can see this, which is why it checks the running sum
        // rather than trusting page-local validity.
        AssertRejected(
            ("kbm map kb 0", FirstOfTwo),
            ("kbm map kb 1",
                """{"profile":"kb","page":1,"pageSize":2,"total":2,"bindings":[{"src":"key:05","dst":"b","custom":false},{"src":"key:06","dst":"x","custom":false}],"more":false}"""));

    [Fact]
    public Task AnEmptyPageWhileMoreIsSetIsRejected() =>
        // The livelock case. An adapter that keeps answering "there is more" while
        // sending nothing would spin the client until its deadline with no
        // diagnosis at all.
        AssertRejected(
            ("kbm map kb 0", FirstOfTwo),
            ("kbm map kb 1", """{"profile":"kb","page":1,"pageSize":1,"total":2,"bindings":[],"more":true}"""));

    [Fact]
    public async Task PagingStopsAtTheHardPageCeiling()
    {
        // The other livelock guard: pages that each carry one row and never say
        // "done". MAX_KBM_MAP_PAGES bounds the work even when every individual
        // page looks valid.
        var script = new List<(string, object)>();
        for (var page = 0; page <= 40; page++)
        {
            script.Add((
                $"kbm map kb {page}",
                $$"""{"profile":"kb","page":{{page}},"pageSize":1,"total":9999,"bindings":[{"src":"key:04","dst":"a","custom":false}],"more":true}"""));
        }

        await Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(new ScriptedChannel([.. script]))
                .LoadKbmMappingAsync(KbmProfile.Keyboard));
    }

    [Fact]
    public Task AFinalCountBelowTheTotalIsRejected() =>
        // The adapter said two and sent one, then stopped. Publishing that as a
        // complete mapping would hide a binding the user cannot then remove.
        AssertRejected(
            ("kbm map kb 0",
                """{"profile":"kb","page":0,"pageSize":1,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"more":false}"""));

    [Fact]
    public async Task ASingleCompletePageNeedsNoSecondRequest()
    {
        // The ordinary case, pinned so a future guard cannot make paging mandatory
        // and double every read.
        var channel = new ScriptedChannel(
            ("kbm map kb 0",
                """{"profile":"kb","page":0,"pageSize":1,"total":1,"bindings":[{"src":"key:04","dst":"a","custom":true}],"more":false}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmProfile.Keyboard);

        Assert.Single(mapping.Bindings);
        Assert.True(mapping.Loaded);
        channel.AssertDrained();
    }

    [Fact]
    public async Task AnEmptyProfileIsAValidCompleteAnswer()
    {
        // Zero bindings and zero total is a real state -- a profile the user has
        // cleared -- and must not be mistaken for a failed read.
        var channel = new ScriptedChannel(
            ("kbm map kb 0",
                """{"profile":"kb","page":0,"pageSize":1,"total":0,"bindings":[],"more":false}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmProfile.Keyboard);

        Assert.Empty(mapping.Bindings);
        Assert.True(mapping.Loaded);
        channel.AssertDrained();
    }
}
