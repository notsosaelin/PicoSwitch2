using System.Diagnostics;

namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Bounded latest-state writer for the Controller Link data plane.
///
/// ## Why this is not fire-and-forget
///
/// <c>GattWriteOption.WriteWithoutResponse</c> means the ADAPTER sends no ATT
/// Write Response. It does not mean the local WinRT operation may be discarded.
/// Discarding it at 125 Hz would leave an unbounded number of overlapping
/// operations inside the Bluetooth stack with no backpressure, no completion
/// status and no way to know how far behind the radio had fallen — an unbounded
/// queue of stale controller frames, built out of concurrency rather than out
/// of a list, which is the same defect wearing a different shape.
///
/// ## The policy
///
/// One mailbox holding the newest frame, and at most ONE write in flight.
/// A frame arriving while a write is outstanding REPLACES the pending frame;
/// it does not join a queue, because an older controller state has no value
/// once a newer one exists. When the operation completes, the newest pending
/// frame goes out immediately; if there is none, the writer idles until the
/// scheduler publishes again.
///
/// Still write-without-response: no ATT response, no gameplay ACK, no per-frame
/// round trip, and no retransmission of a failed historical state. A failed
/// write is superseded, never replayed — replaying it would put stale input on
/// the console after newer input already existed.
///
/// The await is the local operation's lifetime and the backpressure boundary,
/// nothing more.
///
/// Separated from <see cref="GattControllerLinkDataPlane"/> because that class
/// needs a real radio to construct. Policy that can only exist inside an
/// untestable class is policy nobody can prove.
/// </summary>
public sealed class ControllerLinkWriter(Func<byte[], Task<bool>> writeAsync)
{
    private readonly object gate = new();
    private byte[]? pending;
    private bool pumpRunning;
    private bool closed;

    private long statesPublished;
    private long statesCoalesced;
    private long writesIssued;
    private long writeFailures;
    private long totalLatencyTicks;
    private long maximumLatencyTicks;
    private int inFlight;
    private int maximumInFlight;

    /// <summary>Frames handed to the writer by the report scheduler.</summary>
    public long StatesPublished => Interlocked.Read(ref statesPublished);

    /// <summary>
    /// Frames replaced in the mailbox before they could be sent — latest-state
    /// -wins doing its job. A large ratio means the radio is behind the
    /// scheduler, which is a measurement rather than a fault.
    /// </summary>
    public long StatesCoalesced => Interlocked.Read(ref statesCoalesced);

    /// <summary>Writes the transport actually issued and the stack accepted.</summary>
    public long WritesIssued => Interlocked.Read(ref writesIssued);

    public long WriteFailures => Interlocked.Read(ref writeFailures);

    /// <summary>
    /// Must never exceed 1. Exposed rather than asserted so a qualification run
    /// can prove the writer stayed bounded instead of assuming it.
    /// </summary>
    public int MaximumInFlight => Volatile.Read(ref maximumInFlight);

    public TimeSpan AverageWriteLatency
    {
        get
        {
            var issued = Interlocked.Read(ref writesIssued) + Interlocked.Read(ref writeFailures);
            return issued == 0
                ? TimeSpan.Zero
                : TicksToTimeSpan(Interlocked.Read(ref totalLatencyTicks) / issued);
        }
    }

    public TimeSpan MaximumWriteLatency => TicksToTimeSpan(Interlocked.Read(ref maximumLatencyTicks));

    /// <summary>Hand over the newest frame. Never blocks, never queues.</summary>
    public void Publish(byte[] frame)
    {
        bool startPump;
        lock (gate)
        {
            if (closed)
            {
                return;
            }

            if (pending is not null)
            {
                Interlocked.Increment(ref statesCoalesced);
            }

            pending = frame;
            startPump = !pumpRunning;
            if (startPump)
            {
                pumpRunning = true;
            }
        }

        Interlocked.Increment(ref statesPublished);
        if (startPump)
        {
            _ = PumpAsync();
        }
    }

    /// <summary>
    /// Stop writing and drop anything unsent. A frame escaping after teardown
    /// is exactly the held input that Stop exists to prevent; the service
    /// publishes neutral through its own path.
    /// </summary>
    public void Close()
    {
        lock (gate)
        {
            closed = true;
            pending = null;
        }
    }

    private async Task PumpAsync()
    {
        while (true)
        {
            byte[] frame;
            lock (gate)
            {
                if (pending is null || closed)
                {
                    // Nothing newer to send. Idle rather than spin; the next
                    // Publish restarts the pump.
                    pumpRunning = false;
                    return;
                }

                frame = pending;
                pending = null;
            }

            var depth = Interlocked.Increment(ref inFlight);
            UpdateMaximum(ref maximumInFlight, depth);
            var started = Stopwatch.GetTimestamp();

            try
            {
                var ok = await writeAsync(frame).ConfigureAwait(false);
                if (ok)
                {
                    Interlocked.Increment(ref writesIssued);
                }
                else
                {
                    Interlocked.Increment(ref writeFailures);
                }
            }
            catch (Exception)
            {
                // Superseded, not retried. Persistent failure shows as a
                // climbing counter and, when the session dies, as carrier loss.
                Interlocked.Increment(ref writeFailures);
            }
            finally
            {
                var elapsed = Stopwatch.GetTimestamp() - started;
                Interlocked.Add(ref totalLatencyTicks, elapsed);
                UpdateMaximum(ref maximumLatencyTicks, elapsed);
                Interlocked.Decrement(ref inFlight);
            }
        }
    }

    private static void UpdateMaximum(ref int target, int candidate)
    {
        var current = Volatile.Read(ref target);
        while (candidate > current)
        {
            var observed = Interlocked.CompareExchange(ref target, candidate, current);
            if (observed == current)
            {
                return;
            }

            current = observed;
        }
    }

    private static void UpdateMaximum(ref long target, long candidate)
    {
        var current = Volatile.Read(ref target);
        while (candidate > current)
        {
            var observed = Interlocked.CompareExchange(ref target, candidate, current);
            if (observed == current)
            {
                return;
            }

            current = observed;
        }
    }

    private static TimeSpan TicksToTimeSpan(long ticks) =>
        TimeSpan.FromSeconds(ticks / (double)Stopwatch.Frequency);
}
