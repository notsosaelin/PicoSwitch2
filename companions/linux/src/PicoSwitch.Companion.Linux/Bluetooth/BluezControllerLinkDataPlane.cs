using PicoSwitch.Companion.Platform.ControllerLink;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// Path C over BlueZ: the two Controller Link characteristics on the management
/// service that is already connected.
///
/// The same contract and the same bounded writer as the Windows
/// <c>GattControllerLinkDataPlane</c>: one latest-state mailbox, at most one
/// write in flight, write-without-response (<c>type=command</c>), and a failed
/// write superseded rather than retried. It never opens a connection of its
/// own; the management transport owns the link and closes this plane when the
/// link goes.
///
/// Writes go through <c>GattCharacteristic1.WriteValue</c>. BlueZ also offers
/// <c>AcquireWrite</c> (a socket per characteristic, no D-Bus round trip per
/// frame); whether the D-Bus path is the latency floor on a Steam Deck is
/// unmeasured, so the simpler path is used until a measurement says otherwise.
/// </summary>
internal sealed class BluezControllerLinkDataPlane : IControllerLinkDataPlane
{
    private readonly IBluezBus bus;
    private readonly BluezGattService service;
    private readonly Func<int> attMtu;
    private readonly Action<string, string>? log;
    private readonly ControllerLinkWriter writer;

    private string? input;
    private IAsyncDisposable? outputSubscription;
    private long outputFramesReceived;
    private volatile bool disposed;
    private int closedRaised;

    public BluezControllerLinkDataPlane(
        IBluezBus bus,
        BluezGattService service,
        Func<int> attMtu,
        Action<string, string>? log = null)
    {
        this.bus = bus;
        this.service = service;
        this.attMtu = attMtu;
        this.log = log;
        writer = new ControllerLinkWriter(WriteFrameAsync);
    }

    public int AttMtu => attMtu();

    public event Action<byte[]>? OutputFrameReceived;

    public event Action<string>? Closed;

    public long StatesPublished => writer.StatesPublished;

    public long StatesCoalesced => writer.StatesCoalesced;

    public long FramesWritten => writer.WritesIssued;

    public long FrameWriteFailures => writer.WriteFailures;

    public long OutputFramesReceived => Interlocked.Read(ref outputFramesReceived);

    public int MaximumInFlight => writer.MaximumInFlight;

    public bool Busy => writer.Busy;

    public TimeSpan AverageWriteLatency => writer.AverageWriteLatency;

    public TimeSpan MaximumWriteLatency => writer.MaximumWriteLatency;

    public async Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        // An older firmware simply does not declare these. That is "update the
        // adapter", not a failure, exactly as on Windows.
        var inPath = service.Characteristic(BleManagementContract.ControllerLinkInUuid);
        var outPath = service.Characteristic(BleManagementContract.ControllerLinkOutUuid);
        if (inPath is null || outPath is null)
        {
            return false;
        }

        try
        {
            outputSubscription = await bus.SubscribeAsync(outPath, OnOutputValue, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (BluezException error)
        {
            log?.Invoke("controller-link", $"output subscribe failed: {error}");
            return false;
        }

        input = inPath;
        return true;
    }

    public void PublishInput(ReadOnlySpan<byte> frame)
    {
        if (disposed || input is null)
        {
            return;
        }

        writer.Publish(frame.ToArray());
    }

    private async Task<bool> WriteFrameAsync(byte[] frame)
    {
        var characteristic = input;
        if (characteristic is null || disposed)
        {
            return false;
        }

        try
        {
            await bus.WriteAsync(characteristic, frame, withResponse: false).ConfigureAwait(false);
            return true;
        }
        catch (BluezException)
        {
            // Superseded by the next frame, never retried: an old controller
            // state has no value once a newer one exists.
            return false;
        }
    }

    /// <summary>Raised by the management transport when the link it owns goes away.</summary>
    public void NotifyClosed(string reason)
    {
        if (Interlocked.Exchange(ref closedRaised, 1) == 0)
        {
            Closed?.Invoke(reason);
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        writer.Close();
        input = null;
        if (outputSubscription is { } subscription)
        {
            outputSubscription = null;
            await subscription.DisposeAsync().ConfigureAwait(false);
        }
    }

    private void OnOutputValue(byte[] payload)
    {
        if (disposed)
        {
            return;
        }

        Interlocked.Increment(ref outputFramesReceived);
        OutputFrameReceived?.Invoke(payload);
    }
}
