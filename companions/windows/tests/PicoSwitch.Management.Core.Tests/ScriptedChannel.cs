using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// A management channel that expects an exact command sequence.
///
/// Asserting the command as well as returning the reply is deliberate: most of
/// what <see cref="ManagementClient"/> owns is WHICH commands it sends and in
/// what order (mutation then authoritative readback, cursor progression, the
/// cancel on a failed Amiibo upload). A fake that only replied would let that
/// half go untested.
/// </summary>
public sealed class ScriptedChannel(params (string Command, object Reply)[] exchanges)
    : IManagementChannel
{
    private readonly Queue<(string Command, object Reply)> pending = new(exchanges);

    public List<string> Sent { get; } = [];

    public Task<string> TransactAsync(
        string command,
        long timeoutMillis = ManagementChannel.DefaultTimeoutMillis,
        CancellationToken cancellationToken = default)
    {
        Sent.Add(command);
        Assert.True(pending.Count > 0, $"Unexpected command: {command}");
        var next = pending.Dequeue();
        Assert.Equal(next.Command, command);
        if (next.Reply is Exception error)
        {
            throw error;
        }

        return Task.FromResult((string)next.Reply);
    }

    public void AssertDrained() =>
        Assert.True(
            pending.Count == 0,
            "Unconsumed exchanges: " + string.Join(", ", pending.Select(entry => entry.Command)));
}

/// <summary>A channel that always answers the same reply, for bounded-timeout tests.</summary>
public sealed class ConstantChannel(string expectedCommand, string reply) : IManagementChannel
{
    public int Calls { get; private set; }

    public Task<string> TransactAsync(
        string command,
        long timeoutMillis = ManagementChannel.DefaultTimeoutMillis,
        CancellationToken cancellationToken = default)
    {
        Assert.Equal(expectedCommand, command);
        Calls++;
        return Task.FromResult(reply);
    }
}

/// <summary>
/// A deterministic clock and a delay that advances it without waiting.
///
/// The polling workflows (persistence, Amiibo, wake) are bounded in wall-clock
/// time. Testing them against the real clock would put six seconds into the
/// suite for one assertion, so the client takes both as seams and this drives
/// them.
/// </summary>
public sealed class VirtualClock
{
    public long NowMillis { get; private set; }

    public Func<long> Now => () => NowMillis;

    public Func<TimeSpan, CancellationToken, Task> Delay => (duration, _) =>
    {
        NowMillis += (long)duration.TotalMilliseconds;
        return Task.CompletedTask;
    };
}
