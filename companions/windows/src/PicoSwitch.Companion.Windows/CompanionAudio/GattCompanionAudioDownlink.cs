using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace PicoSwitch.Companion.Windows.CompanionAudio;

/// <summary>
/// Companion audio downlink over the live management GATT service
/// (EXPERIMENTAL). Like <c>GattControllerLinkDataPlane</c> it opens nothing and
/// owns no connection: it subscribes to one characteristic on a service the
/// management transport already resolved, and unsubscribes on dispose.
/// </summary>
public sealed class GattCompanionAudioDownlink(GattDeviceService service, Func<int> attMtu)
    : ICompanionAudioDownlink
{
    private static readonly Guid DownUuid = new(BleManagementContract.CompanionAudioDownUuid);

    private GattCharacteristic? characteristic;
    private long notifications;
    private bool subscribed;
    private volatile bool disposed;

    public event Action<byte[]>? FrameReceived;

    public int AttMtu => attMtu();

    public long NotificationsReceived => Interlocked.Read(ref notifications);

    public async Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        // Uncached for the same reason as the Controller Link pair: a firmware
        // update that added this characteristic must not be judged on the
        // characteristic list Windows cached from the previous build.
        var found = await service
            .GetCharacteristicsForUuidAsync(DownUuid, BluetoothCacheMode.Uncached)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (found.Status != GattCommunicationStatus.Success || found.Characteristics.Count == 0)
        {
            return false;
        }

        var down = found.Characteristics[0];
        down.ValueChanged += OnValue;
        var status = await down
            .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (status.Status != GattCommunicationStatus.Success)
        {
            down.ValueChanged -= OnValue;
            return false;
        }

        characteristic = down;
        subscribed = true;
        return true;
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        var down = characteristic;
        characteristic = null;
        if (down is null)
        {
            return;
        }

        down.ValueChanged -= OnValue;
        if (subscribed)
        {
            try
            {
                await down
                    .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.None)
                    .AsTask().ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Best effort; the adapter drops its subscriber on `caudio stop`
                // and on management loss anyway.
            }
        }

        // The service belongs to the management transport. Not disposed here.
    }

    private void OnValue(GattCharacteristic sender, GattValueChangedEventArgs args)
    {
        if (disposed)
        {
            return;
        }

        var payload = new byte[args.CharacteristicValue.Length];
        DataReader.FromBuffer(args.CharacteristicValue).ReadBytes(payload);
        Interlocked.Increment(ref notifications);
        FrameReceived?.Invoke(payload);
    }
}
