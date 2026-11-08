using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// One process per session. The abstract socket namespace is Linux-only, so
/// these run where it exists and are skipped (as passes) elsewhere.
/// </summary>
public sealed class SingleInstanceTests
{
    private static string Key() => "ps2-test-" + Guid.NewGuid().ToString("N");

    [Fact]
    public async Task ASecondLaunchIsRefusedAndBringsTheFirstForward()
    {
        if (!OperatingSystem.IsLinux())
        {
            return;
        }

        var key = Key();
        var activated = new TaskCompletionSource();
        using var first = SingleInstance.TryAcquire(key, () => activated.TrySetResult());
        Assert.NotNull(first);

        Assert.Null(SingleInstance.TryAcquire(key, () => { }));
        await activated.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void TheNameIsFreeAgainTheMomentTheOwnerGoes()
    {
        if (!OperatingSystem.IsLinux())
        {
            return;
        }

        // No stale lock to clean up: the kernel releases an abstract name with
        // its socket, which is what a crashed process does implicitly.
        var key = Key();
        SingleInstance.TryAcquire(key, () => { })!.Dispose();

        using var next = SingleInstance.TryAcquire(key, () => { });
        Assert.NotNull(next);
    }

    [Fact]
    public void TheNameIsPerUserAndInTheAbstractNamespace()
    {
        var name = SingleInstance.NameFor("PicoSwitch2.Companion");
        Assert.StartsWith("\0PicoSwitch2.Companion.", name);
        Assert.EndsWith(Environment.UserName, name);
    }
}
