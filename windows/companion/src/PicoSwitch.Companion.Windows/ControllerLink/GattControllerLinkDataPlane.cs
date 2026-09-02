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
/// </summary>
public sealed class GattControllerLinkDataPlane(
    GattDeviceService service,
    Func<int> attMtu) : IControllerLinkDataPlane
{
    private static readonly Guid InUuid = new(BleManagementContract.ControllerLinkInUuid);
    private static readonly Guid OutUuid = new(BleManagementContract.ControllerLinkOutUuid);

    private GattCharacteristic? input;
    private GattCharacteristic? output;
    private long framesWritten;
    private long frameWriteFailures;
    private long outputFramesReceived;
    private bool subscribed;
    private bool disposed;

    public int AttMtu => attMtu();

    public event Action<byte[]>? OutputFrameReceived;

    public event Action<string>? Closed;

    public long FramesWritten => Interlocked.Read(ref framesWritten);

    public long FrameWriteFailures => Interlocked.Read(ref frameWriteFailures);

    public long OutputFramesReceived => Interlocked.Read(ref outputFramesReceived);

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

    public bool TryWriteInput(ReadOnlySpan<byte> frame)
    {
        var characteristic = input;
        if (characteristic is null || disposed)
        {
            return false;
        }

        try
        {
            var writer = new DataWriter();
            writer.WriteBytes(frame.ToArray());

            // Fire and forget by design. WriteValueWithoutResponse still returns
            // an IAsyncOperation, but awaiting it at 125 Hz would serialise
            // gameplay behind the stack's completion and defeat the reason this
            // is write-without-response. Delivery is not acknowledged at the
            // protocol level either — the frame's sequence number is what makes
            // a lost or reordered frame harmless.
            _ = characteristic.WriteValueAsync(
                writer.DetachBuffer(), GattWriteOption.WriteWithoutResponse);

            Interlocked.Increment(ref framesWritten);
            return true;
        }
        catch (Exception)
        {
            // A single failed write is not a reason to tear down a healthy
            // link: the next frame supersedes this one. Persistent failure
            // shows up as a climbing counter and, when the session dies, Closed.
            Interlocked.Increment(ref frameWriteFailures);
            return false;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;

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
