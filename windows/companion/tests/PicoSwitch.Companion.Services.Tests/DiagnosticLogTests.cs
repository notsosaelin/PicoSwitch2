using PicoSwitch.Companion.Services.Diagnostics;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// One stream, every layer — and a bounded one.
///
/// The properties that matter are the ones that keep a support bundle honest:
/// the ring never grows without limit, and what it dropped is reported rather
/// than silently missing.
/// </summary>
public sealed class DiagnosticLogTests
{
    [Fact]
    public void EntriesAreRecordedInOrderWithTheirSource()
    {
        var log = new DiagnosticLog();
        log.Info("ble", "scan started");
        log.Warn("mgmt", "reply late");

        var entries = log.Snapshot();
        Assert.Equal(2, entries.Count);
        Assert.Equal("ble", entries[0].Source);
        Assert.Equal(DiagnosticLevel.Warn, entries[1].Level);
        Assert.Contains("reply late", entries[1].ToString());
    }

    [Fact]
    public void TheRingIsBoundedAndSaysHowMuchItDropped()
    {
        // A log that grew without limit would be a leak on a path that runs at
        // the publish cadence (250 Hz), and would change the timing it exists
        // to observe.
        var log = new DiagnosticLog(capacity: 4);
        for (var index = 0; index < 10; index++)
        {
            log.Debug("bridge", $"report {index}");
        }

        var entries = log.Snapshot();
        Assert.Equal(4, entries.Count);
        Assert.Equal(6, log.Dropped);
        Assert.Contains("report 9", entries[^1].Message);
        Assert.Contains("report 6", entries[0].Message);
    }

    [Fact]
    public void TheRenderedLogAlwaysStatesTheDroppedCount()
    {
        // A bundle that silently omitted "and 4000 entries before this" would be
        // read as a complete history of the session.
        var log = new DiagnosticLog(capacity: 2);
        log.Info("ui", "one");
        Assert.Contains("dropped=0", log.Render());

        log.Info("ui", "two");
        log.Info("ui", "three");
        Assert.Contains("dropped=1", log.Render());
    }

    [Fact]
    public void ObserversSeeEachEntryAsItIsRecorded()
    {
        var log = new DiagnosticLog();
        var seen = new List<DiagnosticEntry>();
        log.Recorded += seen.Add;

        log.Error("mgmt", "session invalidated");
        var entry = Assert.Single(seen);
        Assert.Equal(DiagnosticLevel.Error, entry.Level);
    }

    [Fact]
    public void ClearingResetsBothTheRingAndTheDroppedCount()
    {
        var log = new DiagnosticLog(capacity: 1);
        log.Info("ui", "one");
        log.Info("ui", "two");
        Assert.Equal(1, log.Dropped);

        log.Clear();
        Assert.Empty(log.Snapshot());
        Assert.Equal(0, log.Dropped);
    }

    [Fact]
    public void ANonPositiveCapacityIsRefusedRatherThanSilentlyCorrected() =>
        Assert.Throws<ArgumentOutOfRangeException>(() => new DiagnosticLog(capacity: 0));

    [Fact]
    public void ConcurrentWritersDoNotLoseOrCorruptEntries()
    {
        // Every layer reports into one log, from its own thread: the HID callback
        // thread, the GATT notification thread, and the UI thread.
        const int writers = 8;
        const int perWriter = 250;
        var log = new DiagnosticLog(capacity: writers * perWriter);

        Parallel.For(0, writers, writer =>
        {
            for (var index = 0; index < perWriter; index++)
            {
                log.Info($"w{writer}", index.ToString());
            }
        });

        Assert.Equal(writers * perWriter, log.Snapshot().Count);
        Assert.Equal(0, log.Dropped);
    }
}
