using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Controller Link data plane over the live management GATT service.
///
/// Constructed with the service the management transport already resolved, so
/// this class opens nothing and owns no connection — it adds two
/// characteristics to a session someone else holds, and drops them again.
///
/// ## The writer is a bounded latest-state mailbox
///
/// <c>WriteWithoutResponse</c> means the ADAPTER sends no ATT Write Response.
/// It does not mean the local WinRT operation should be discarded. Firing and
/// forgetting <c>WriteValueAsync</c> at 125 Hz would leave an unbounded number
/// of overlapping operations in the stack with no backpressure, no completion
/// status, and no way to know how far behind the radio had fallen — an
/// unbounded queue of stale controller frames built out of concurrency instead
/// of out of a list.
///
/// So: one mailbox holding the newest frame, and at most ONE write in flight.
/// A frame arriving while a write is outstanding REPLACES the pending frame
/// rather than joining a queue. When the operation completes, the newest
/// pending frame goes out immediately; if there is none, the writer idles until
/// the scheduler publishes again.
///
/// This is still Write Without Response: no ATT response, no gameplay ACK, no
/// per-frame round trip, and no retransmission of a failed historical state.
/// Latest state wins, and a failed write is simply superseded. The await is the
/// local operation's lifetime and the backpressure boundary, nothing more.
/// </summary>
public sealed class GattControllerLinkDataPlane : IControllerLinkDataPlane
{
    private static readonly Guid InUuid = new(BleManagementContract.ControllerLinkInUuid);
    private static readonly Guid OutUuid = new(BleManagementContract.ControllerLinkOutUuid);

    private readonly GattDeviceService service;
    private readonly Func<int> attMtu;
    private readonly ControllerLinkWriter writer;

    public GattControllerLinkDataPlane(GattDeviceService service, Func<int> attMtu)
    {
        this.service = service;
        this.attMtu = attMtu;
        // Not a field initializer: the writer's sink is an instance method, and
        // `this` is not available before the constructor body.
        writer = new ControllerLinkWriter(WriteFrameAsync);
    }

    private GattCharacteristic? input;
    private GattCharacteristic? output;
    private long outputFramesReceived;
    private bool subscribed;
    private volatile bool disposed;

    public int AttMtu => attMtu();

    public event Action<byte[]>? OutputFrameReceived;

    public event Action<string>? Closed;

    public long StatesPublished => writer.StatesPublished;

    public long StatesCoalesced => writer.StatesCoalesced;

    public long FramesWritten => writer.WritesIssued;

    public long FrameWriteFailures => writer.WriteFailures;

    public long OutputFramesReceived => Interlocked.Read(ref outputFramesReceived);

    public int MaximumInFlight => writer.MaximumInFlight;

    public TimeSpan AverageWriteLatency => writer.AverageWriteLatency;

    public TimeSpan MaximumWriteLatency => writer.MaximumWriteLatency;

    public async Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        // Uncached: an adapter that gained the data plane in a firmware update
        // would otherwise be judged on the characteristic list Windows cached
        // from the previous build, and report "no data plane" forever.
        var found = await service
            .GetCharacteristicsForUuidAsync(InUuid, BluetoothCacheMode.Uncached)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (found.Status != GattCommunicationStatus.Success || found.Characteristics.Count == 0)
        {
            return false;
        }

        input = found.Characteristics[0];

        var outFound = await service
            .GetCharacteristicsForUuidAsync(OutUuid, BluetoothCacheMode.Uncached)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (outFound.Status != GattCommunicationStatus.Success || outFound.Characteristics.Count == 0)
        {
            // Input without feedback is a firmware the companion does not know;
            // refuse rather than stream blind into half a contract.
            input = null;
            return false;
        }

        output = outFound.Characteristics[0];
        output.ValueChanged += OnOutputValue;

        var status = await output
            .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (status.Status != GattCommunicationStatus.Success)
        {
            output.ValueChanged -= OnOutputValue;
            input = null;
            output = null;
            return false;
        }

        subscribed = true;
        return true;
    }

    /// <summary>
    /// Hand the newest frame to the bounded writer. The mailbox policy —
    /// one frame pending, one write in flight, newest wins — lives in
    /// <see cref="ControllerLinkWriter"/> where it is testable without a radio.
    /// </summary>
    public void PublishInput(ReadOnlySpan<byte> frame)
    {
        if (disposed || input is null)
        {
            return;
        }

        writer.Publish(frame.ToArray());
    }

    /// <summary>
    /// The one place a frame reaches the radio. Awaited for backpressure and
    /// for the completion status, NOT for a remote acknowledgement --
    /// WriteWithoutResponse produces none.
    /// </summary>
    private async Task<bool> WriteFrameAsync(byte[] frame)
    {
        var characteristic = input;
        if (characteristic is null || disposed)
        {
            return false;
        }

        var buffer = new DataWriter();
        buffer.WriteBytes(frame);
        var status = await characteristic
            .WriteValueWithResultAsync(buffer.DetachBuffer(),
                                       GattWriteOption.WriteWithoutResponse)
            .AsTask().ConfigureAwait(false);
        return status.Status == GattCommunicationStatus.Success;
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;

        // Drop anything unsent: neutralization is published by the service
        // through its own path, and a stale frame escaping after teardown would
        // be exactly the held input Stop exists to prevent.
        writer.Close();

        var characteristic = output;
        output = null;
        input = null;

        if (characteristic is not null)
        {
            characteristic.ValueChanged -= OnOutputValue;
            if (subscribed)
            {
                try
                {
                    await characteristic
                        .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                            GattClientCharacteristicConfigurationDescriptorValue.None)
                        .AsTask().ConfigureAwait(false);
                }
                catch (Exception)
                {
                    // Best effort. The management session outlives this object,
                    // so a failed unsubscribe is untidy rather than harmful, and
                    // the adapter drops its subscriber state on stop anyway.
                }
            }
        }

        // The service belongs to the management transport. Not disposed here.
    }

    /// <summary>Raised by the transport when the carrier goes away.</summary>
    public void NotifyClosed(string reason) => Closed?.Invoke(reason);

    private void OnOutputValue(GattCharacteristic sender, GattValueChangedEventArgs args)
    {
        var payload = new byte[args.CharacteristicValue.Length];
        DataReader.FromBuffer(args.CharacteristicValue).ReadBytes(payload);
        Interlocked.Increment(ref outputFramesReceived);
        OutputFrameReceived?.Invoke(payload);
    }

}
