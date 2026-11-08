using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// Companion audio downlink over BlueZ (EXPERIMENTAL): <c>StartNotify</c> on the
/// AUDIO-DOWN characteristic of the management service that is already
/// connected. The counterpart of the Windows <c>GattCompanionAudioDownlink</c>;
/// like it, this opens no connection and owns none.
///
/// Notifications arrive as <c>Value</c> property changes on the D-Bus thread
/// and go straight to <see cref="FrameReceived"/>: the receiver decodes and
/// hands off, so nothing here queues behind management replies.
///
/// BlueZ caches a bonded device's GATT database. An adapter flashed with a
/// build that added this characteristic is seen once BlueZ re-reads the
/// database (Service Changed, or a re-pair); until then <see cref="OpenAsync"/>
/// reports the plane missing, which the app shows as "does not expose the
/// audio channel".
/// </summary>
internal sealed class BluezCompanionAudioDownlink(IBluezBus bus, BluezGattService service, Func<int> attMtu)
    : ICompanionAudioDownlink
{
    private IAsyncDisposable? subscription;
    private long notifications;
    private volatile bool disposed;

    public event Action<byte[]>? FrameReceived;

    public int AttMtu => attMtu();

    public long NotificationsReceived => Interlocked.Read(ref notifications);

    public async Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        var path = service.Characteristic(BleManagementContract.CompanionAudioDownUuid);
        if (path is null)
        {
            return false;
        }

        try
        {
            subscription = await bus.SubscribeAsync(path, OnValue, cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (BluezException)
        {
            return false;
        }
    }

    private void OnValue(byte[] payload)
    {
        if (disposed)
        {
            return;
        }

        Interlocked.Increment(ref notifications);
        FrameReceived?.Invoke(payload);
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        if (subscription is { } live)
        {
            subscription = null;
            try
            {
                await live.DisposeAsync().ConfigureAwait(false);
            }
            catch (BluezException)
            {
                // Best effort; the adapter drops its subscriber on `caudio stop`
                // and on management loss anyway.
            }
        }
    }
}

/// <summary>
/// Companion microphone uplink over BlueZ (EXPERIMENTAL): write-without-response
/// (<c>type=command</c>) on MIC-UP through the shared bounded
/// <see cref="CompanionMicWriter"/>. The counterpart of the Windows
/// <c>GattCompanionMicUplink</c>.
/// </summary>
internal sealed class BluezCompanionMicUplink : ICompanionMicUplink
{
    private readonly IBluezBus bus;
    private readonly BluezGattService service;
    private readonly CompanionMicWriter writer;
    private string? characteristic;
    private volatile bool disposed;

    public BluezCompanionMicUplink(IBluezBus bus, BluezGattService service)
    {
        this.bus = bus;
        this.service = service;
        writer = new CompanionMicWriter(WriteFrameAsync);
    }

    public CompanionMicWriter.Counters Stats => writer.Stats;

    public Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        characteristic = service.Characteristic(BleManagementContract.CompanionMicUpUuid);
        return Task.FromResult(characteristic is not null);
    }

    public void Send(byte[] frame)
    {
        if (!disposed && characteristic is not null)
        {
            writer.Enqueue(frame);
        }
    }

    private async Task<bool> WriteFrameAsync(byte[] frame)
    {
        var target = characteristic;
        if (target is null || disposed)
        {
            return false;
        }

        try
        {
            await bus.WriteAsync(target, frame, withResponse: false).ConfigureAwait(false);
            return true;
        }
        catch (BluezException)
        {
            // Never retried: the adapter conceals the gap.
            return false;
        }
    }

    public ValueTask DisposeAsync()
    {
        disposed = true;
        writer.Close();
        characteristic = null;
        return ValueTask.CompletedTask;
    }
}
