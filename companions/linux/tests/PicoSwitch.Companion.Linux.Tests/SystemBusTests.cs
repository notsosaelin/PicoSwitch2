using System.Diagnostics;
using PicoSwitch.Companion.Linux.Bluetooth;
using Xunit;
using Xunit.Abstractions;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// The real D-Bus implementation against whatever system bus this machine has.
///
/// CI runners and WSL have a system bus but no bluetoothd, which is exactly the
/// "Bluetooth is not available" case a Steam Deck with Bluetooth disabled also
/// presents. The invariant is that the app gets an ANSWER, promptly — never a
/// hang that leaves every page waiting on a probe.
/// </summary>
public sealed class SystemBusTests(ITestOutputHelper output)
{
    [Fact]
    public async Task AMissingBluetoothDaemonIsAPromptAnswerNotAHang()
    {
        if (!OperatingSystem.IsLinux())
        {
            return;
        }

        // Every step is bounded, so a regression FAILS here rather than hanging
        // the suite — which is how this test first found the hang it guards.
        var bus = new DBusBluezBus();
        var clock = Stopwatch.StartNew();
        string outcome;
        try
        {
            var adapter = await bus.GetAdapterAsync().WaitAsync(Budget);
            outcome = adapter is null ? "no adapter" : $"adapter {adapter.Path} powered={adapter.Powered}";
        }
        catch (BluezException error)
        {
            outcome = $"BluezException {error.ErrorName}: {error.Message}";
        }
        catch (TimeoutException)
        {
            outcome = "GetAdapterAsync did not return";
        }

        output.WriteLine($"{clock.ElapsedMilliseconds} ms: {outcome}");
        Assert.DoesNotContain("did not return", outcome);

        var disposed = bus.DisposeAsync().AsTask();
        Assert.True(await Task.WhenAny(disposed, Task.Delay(Budget)) == disposed, "DisposeAsync did not return");
    }

    private static readonly TimeSpan Budget = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task TheRadioProbeNeverThrows()
    {
        if (!OperatingSystem.IsLinux())
        {
            return;
        }

        var bus = new DBusBluezBus();
        var capabilities = await BluezRadio.ProbeAsync(bus).WaitAsync(Budget);
        output.WriteLine($"present={capabilities.RadioPresent} on={capabilities.RadioOn}");
    }
}
