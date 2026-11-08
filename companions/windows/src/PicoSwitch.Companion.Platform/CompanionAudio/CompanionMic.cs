namespace PicoSwitch.Companion.Platform.CompanionAudio;

/// <summary>
/// The local microphone for the companion microphone uplink (EXPERIMENTAL):
/// 48 kHz mono signed 16-bit, delivered as whole 20 ms blocks of 960 samples on
/// a capture thread.
/// </summary>
public interface ICompanionMicCapture : IDisposable
{
    /// <summary>
    /// Open the default capture device and start delivering blocks.
    /// <paramref name="onBlock"/> must be quick: encode and hand off, never block.
    /// </summary>
    void Start(Action<short[]> onBlock);

    void Stop();

    /// <summary>Blocks the device delivered.</summary>
    long BlocksCaptured { get; }
}

/// <summary>Creates the platform's microphone capture, or null when it has none.</summary>
public interface ICompanionMicCaptureFactory
{
    ICompanionMicCapture? CreateCapture();
}

/// <summary>
/// Companion microphone uplink: writes on the live management GATT service.
/// Opens no connection of its own, like the downlink.
/// </summary>
public interface ICompanionMicUplink : IAsyncDisposable
{
    /// <summary>False when the adapter does not expose the characteristic.</summary>
    Task<bool> OpenAsync(CancellationToken cancellationToken = default);

    /// <summary>Queue one frame without blocking (see <see cref="CompanionMicWriter"/>).</summary>
    void Send(byte[] frame);

    CompanionMicWriter.Counters Stats { get; }
}

/// <summary>Obtains a microphone uplink on the live management session.</summary>
public interface ICompanionMicUplinkProvider
{
    ICompanionMicUplink? TryCreateMicUplink();
}

/// <summary>
/// Bounded FIFO writer for the microphone uplink.
///
/// Unlike Controller Link input -- latest state wins, so one mailbox -- the
/// microphone is a stream: every block is audio the console should hear, in
/// order. So frames queue, at most <see cref="Depth"/> of them (160 ms), with
/// ONE write in flight. A write-without-response completes when the platform
/// stack accepts the buffer, not when the radio sends it; with one in flight
/// the platform holds at most one microphone frame, so a microphone frame can
/// be ahead of a Controller Link frame in the stack's queue by one frame at
/// most. When the radio falls behind by more than the depth, the OLDEST
/// waiting frame gives way: the adapter sees the gap in the sequence and
/// conceals it, and the microphone never lags the player by more than the
/// depth.
///
/// The depth is from the firmware's link simulation
/// (tests/firmware/test_ns2_companion_mic.c): with a fifth of the connection
/// events lost to Bluetooth Classic, four frames shed ~70 frames an hour,
/// eight shed none.
/// </summary>
public sealed class CompanionMicWriter(Func<byte[], Task<bool>> writeAsync)
{
    public const int Depth = 8;

    public sealed record Counters(long Queued, long Written, long Displaced, long Failed);

    private readonly object gate = new();
    private readonly Queue<byte[]> pending = new();
    private bool pumping;
    private bool closed;
    private long queued;
    private long written;
    private long displaced;
    private long failed;

    public Counters Stats
    {
        get
        {
            lock (gate)
            {
                return new Counters(queued, written, displaced, failed);
            }
        }
    }

    /// <summary>Frames waiting, not counting the one in flight.</summary>
    public int Waiting
    {
        get
        {
            lock (gate)
            {
                return pending.Count;
            }
        }
    }

    public void Enqueue(byte[] frame)
    {
        lock (gate)
        {
            if (closed)
            {
                return;
            }

            if (pending.Count == Depth)
            {
                pending.Dequeue();
                displaced++;
            }

            pending.Enqueue(frame);
            queued++;
            if (pumping)
            {
                return;
            }

            pumping = true;
        }

        _ = PumpAsync();
    }

    public void Close()
    {
        lock (gate)
        {
            closed = true;
            pending.Clear();
        }
    }

    private async Task PumpAsync()
    {
        while (true)
        {
            byte[] frame;
            lock (gate)
            {
                if (closed || pending.Count == 0)
                {
                    pumping = false;
                    return;
                }

                frame = pending.Dequeue();
            }

            bool ok;
            try
            {
                ok = await writeAsync(frame).ConfigureAwait(false);
            }
            catch (Exception)
            {
                ok = false;
            }

            lock (gate)
            {
                if (ok)
                {
                    written++;
                }
                else
                {
                    // Never retried: a resent frame would arrive after newer
                    // audio. The adapter conceals the gap.
                    failed++;
                }
            }
        }
    }
}
