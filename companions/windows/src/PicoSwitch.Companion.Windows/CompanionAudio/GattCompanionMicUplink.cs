using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace PicoSwitch.Companion.Windows.CompanionAudio;

/// <summary>
/// Companion microphone uplink over the live management GATT service
/// (EXPERIMENTAL): write-without-response on the MIC-UP characteristic, through
/// the bounded <see cref="CompanionMicWriter"/>. Opens nothing and owns no
/// connection, like <see cref="GattCompanionAudioDownlink"/>.
/// </summary>
public sealed class GattCompanionMicUplink : ICompanionMicUplink
{
    private static readonly Guid MicUpUuid = new(BleManagementContract.CompanionMicUpUuid);

    private readonly GattDeviceService service;
    private readonly CompanionMicWriter writer;
    private GattCharacteristic? characteristic;
    private volatile bool disposed;

    public GattCompanionMicUplink(GattDeviceService service)
    {
        this.service = service;
        writer = new CompanionMicWriter(WriteFrameAsync);
    }

    public CompanionMicWriter.Counters Stats => writer.Stats;

    public async Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        // Uncached: a firmware update that added the characteristic must not be
        // judged on the list Windows cached from the previous build.
        var found = await service
            .GetCharacteristicsForUuidAsync(MicUpUuid, BluetoothCacheMode.Uncached)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (found.Status != GattCommunicationStatus.Success || found.Characteristics.Count == 0)
        {
            return false;
        }

        characteristic = found.Characteristics[0];
        return true;
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

        var buffer = new DataWriter();
        buffer.WriteBytes(frame);
        var status = await target
            .WriteValueWithResultAsync(buffer.DetachBuffer(), GattWriteOption.WriteWithoutResponse)
            .AsTask().ConfigureAwait(false);
        return status.Status == GattCommunicationStatus.Success;
    }

    public ValueTask DisposeAsync()
    {
        disposed = true;
        writer.Close();
        characteristic = null;
        // The service belongs to the management transport. Not disposed here.
        return ValueTask.CompletedTask;
    }
}
