using System.Collections.Concurrent;
using System.Text;

namespace PicoSwitch.Companion.Services.Diagnostics;

public enum DiagnosticLevel
{
    Debug,
    Info,
    Warn,
    Error,
}

/// <summary>
/// One diagnostic entry, from any layer.
///
/// <c>Source</c> names the layer that produced it (<c>ble</c>, <c>mgmt</c>,
/// <c>bridge</c>, <c>ui</c>) so a bundle can be read as a sequence across
/// boundaries rather than as several unrelated logs.
/// </summary>
public sealed record DiagnosticEntry(
    DateTimeOffset Timestamp,
    DiagnosticLevel Level,
    string Source,
    string Message)
{
    public override string ToString() =>
        $"{Timestamp:HH:mm:ss.fff} {Level.ToString().ToUpperInvariant(),-5} [{Source}] {Message}";
}

/// <summary>
/// One stream, every layer (WINDOWS_PASS.md §22.1).
///
/// A bounded ring, not a file and not a growing list: the paths that report into
/// it run at up to 125 Hz, and an unbounded log is a memory leak that also
/// changes the timing it is supposed to observe. The ring is rendered only when
/// something asks for it.
///
/// Deliberately in Services rather than in either Core: a Core project that
/// logged would need a logging abstraction, and the two Cores are worth more
/// dependency-free than they are observable. They report through return values
/// and counters instead.
/// </summary>
public sealed class DiagnosticLog(int capacity = DiagnosticLog.DefaultCapacity)
{
    public const int DefaultCapacity = 2000;

    private readonly ConcurrentQueue<DiagnosticEntry> entries = new();
    private readonly int capacity = capacity > 0
        ? capacity
        : throw new ArgumentOutOfRangeException(nameof(capacity), "Capacity must be positive");

    private long dropped;

    /// <summary>Raised after an entry is recorded, so a live view can follow along.</summary>
    public event Action<DiagnosticEntry>? Recorded;

    /// <summary>Entries discarded because the ring was full. Reported, never hidden.</summary>
    public long Dropped => Interlocked.Read(ref dropped);

    public void Record(DiagnosticLevel level, string source, string message)
    {
        var entry = new DiagnosticEntry(DateTimeOffset.Now, level, source, message);
        entries.Enqueue(entry);
        while (entries.Count > capacity && entries.TryDequeue(out _))
        {
            Interlocked.Increment(ref dropped);
        }

        Recorded?.Invoke(entry);
    }

    public void Debug(string source, string message) =>
        Record(DiagnosticLevel.Debug, source, message);

    public void Info(string source, string message) =>
        Record(DiagnosticLevel.Info, source, message);

    public void Warn(string source, string message) =>
        Record(DiagnosticLevel.Warn, source, message);

    public void Error(string source, string message) =>
        Record(DiagnosticLevel.Error, source, message);

    public IReadOnlyList<DiagnosticEntry> Snapshot() => entries.ToArray();

    public void Clear()
    {
        while (entries.TryDequeue(out _))
        {
            // Draining rather than reallocating keeps a concurrent writer valid.
        }

        Interlocked.Exchange(ref dropped, 0);
    }

    /// <summary>
    /// The log as text, newest last.
    ///
    /// The dropped count is rendered even when it is zero: a support bundle that
    /// silently omitted "and 4000 entries before this" would be read as a
    /// complete history of the session.
    /// </summary>
    public string Render()
    {
        var text = new StringBuilder();
        text.Append("dropped=").Append(Dropped).AppendLine();
        foreach (var entry in Snapshot())
        {
            text.AppendLine(entry.ToString());
        }

        return text.ToString();
    }
}
