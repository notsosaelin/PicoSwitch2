using PicoSwitch.Companion.Linux.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// A scriptable BlueZ: one adapter, at most one PicoSwitch2 device, and a
/// management service whose RX writes are answered through TX notifications by
/// <see cref="Replies"/>. Failures are injected per operation.
/// </summary>
internal sealed class FakeBluezBus : IBluezBus
{
    public const string AdapterPath = "/org/bluez/hci0";
    public const ulong PicoAddress = 0x2CCF67A1B2C3;

    private readonly Lock gate = new();
    private Action<byte[]>? txNotification;
    private Action<byte[]>? clOutNotification;
    private Action<byte[]>? audioDownNotification;
    private Action? disconnected;

    public BluezAdapter? Adapter { get; set; } = new(AdapterPath, "AA:BB:CC:DD:EE:FF", true, ["central", "peripheral"]);

    public bool DeviceKnown { get; set; } = true;

    public bool Paired { get; set; } = true;

    public bool Connected { get; set; }

    public bool HasControllerLink { get; set; } = true;

    /// <summary>The companion audio planes (AUDIO-DOWN, MIC-UP): a current Pico 2 W build.</summary>
    public bool HasCompanionAudio { get; set; } = true;

    public BluezException? MicWriteFailure { get; set; }

    public List<byte[]> MicWrites { get; } = [];

    public int? Mtu { get; set; } = 247;

    /// <summary>Advertisements delivered when discovery starts.</summary>
    public List<BluezAdvertisement> Advertisements { get; } = [];

    /// <summary>Command (without the terminating newline) -> reply.</summary>
    public Dictionary<string, string> Replies { get; } = new(StringComparer.Ordinal);

    public BluezException? ConnectFailure { get; set; }

    /// <summary>The Device1.Disconnected reason raised after <see cref="ConnectFailure"/> fires, if any.</summary>
    public string? ConnectFailureReason { get; set; }

    /// <summary>Dual-mode BlueZ: Connect brings the LE link up, then fails on the refused Classic half.</summary>
    public bool ConnectFailureLeavesLinkUp { get; set; }

    private Action<string, string>? disconnectReason;

    /// <summary>
    /// Dual-mode BlueZ whose most recent sighting of the adapter was Classic:
    /// Device1.Connect goes over BR/EDR (and the adapter refuses it) until a
    /// fresh LE advertisement is seen. The Ally in Game Mode, 2026-11-08.
    /// </summary>
    public bool LastSeenClassic { get; set; }

    /// <summary>The live link is Classic: Connected and ServicesResolved, but no GATT at all.</summary>
    public bool ClassicLink { get; set; }

    public BluezException? SubscribeFailure { get; set; }

    public BluezException? PairFailure { get; set; }

    public BluezException? RemoveFailure { get; set; }

    public bool ServicesResolve { get; set; } = true;

    public List<byte[]> RxWrites { get; } = [];

    public List<byte[]> ControllerLinkWrites { get; } = [];

    public int Connects { get; private set; }

    public int Disconnects { get; private set; }

    public int Pairs { get; private set; }

    public int Removes { get; private set; }

    public int ActiveSubscriptions { get; private set; }

    public bool Trusted { get; private set; }

    private readonly List<byte> pendingCommand = [];

    public static string DevicePath => BluezPaths.DevicePath(AdapterPath, PicoAddress);

    public static string ServicePath => DevicePath + "/service0010";

    public static string RxPath => ServicePath + "/char0011";

    public static string TxPath => ServicePath + "/char0013";

    public static string ClInPath => ServicePath + "/char0016";

    public static string ClOutPath => ServicePath + "/char0018";

    public static string AudioDownPath => ServicePath + "/char001b";

    public static string MicUpPath => ServicePath + "/char001e";

    public Task<BluezAdapter?> GetAdapterAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(Adapter);

    public Task<BluezDevice?> FindDeviceAsync(string adapterPath, ulong address, CancellationToken cancellationToken = default) =>
        Task.FromResult(DeviceKnown && address == PicoAddress
            ? new BluezDevice(DevicePath, PicoAddress, "PicoSwitch2", Paired, Connected, Connected)
            : null);

    public Task<IAsyncDisposable> StartDiscoveryAsync(
        string adapterPath,
        string serviceUuid,
        Action<BluezAdvertisement> found,
        CancellationToken cancellationToken = default)
    {
        foreach (var advertisement in Advertisements)
        {
            DeviceKnown |= advertisement.Address == PicoAddress;

            // Only a received advertisement (it has an RSSI) is an LE sighting.
            if (advertisement.Address == PicoAddress && advertisement.Rssi is not null)
            {
                LastSeenClassic = false;
            }

            found(advertisement);
        }

        return Task.FromResult<IAsyncDisposable>(new Disposable(() => { }));
    }

    public Task ConnectAsync(string devicePath, CancellationToken cancellationToken = default)
    {
        Connects++;
        if (LastSeenClassic)
        {
            throw new BluezException(BluezErrors.Failed, "br-connection-refused");
        }

        if (ConnectFailure is { } failure)
        {
            Connected = ConnectFailureLeavesLinkUp;

            // BlueZ order: the method error first, the reason signal after it.
            if (ConnectFailureReason is { } reason && disconnectReason is { } raise)
            {
                _ = Task.Run(async () =>
                {
                    await Task.Delay(20).ConfigureAwait(false);
                    raise(reason, "Connection terminated due to authentication failure");
                });
            }

            throw failure;
        }

        Connected = true;
        return Task.CompletedTask;
    }

    public Task DisconnectAsync(string devicePath)
    {
        Disconnects++;
        Connected = false;
        ClassicLink = false;
        return Task.CompletedTask;
    }

    // Services resolve only on a live link, as on BlueZ.
    public Task<bool> WaitServicesResolvedAsync(string devicePath, CancellationToken cancellationToken = default) =>
        Task.FromResult(ServicesResolve && Connected);

    public Task<BluezGattService?> FindServiceAsync(string devicePath, string serviceUuid, CancellationToken cancellationToken = default)
    {
        if (ClassicLink)
        {
            return Task.FromResult<BluezGattService?>(null);
        }

        var characteristics = new Dictionary<string, string>
        {
            [BleManagementContract.RxUuid] = RxPath,
            [BleManagementContract.TxUuid] = TxPath,
        };
        if (HasControllerLink)
        {
            characteristics[BleManagementContract.ControllerLinkInUuid] = ClInPath;
            characteristics[BleManagementContract.ControllerLinkOutUuid] = ClOutPath;
        }

        if (HasCompanionAudio)
        {
            characteristics[BleManagementContract.CompanionAudioDownUuid] = AudioDownPath;
            characteristics[BleManagementContract.CompanionMicUpUuid] = MicUpPath;
        }

        return Task.FromResult<BluezGattService?>(new BluezGattService(ServicePath, characteristics));
    }

    public Task WriteAsync(string characteristicPath, byte[] value, bool withResponse, CancellationToken cancellationToken = default)
    {
        if (characteristicPath == ClInPath)
        {
            if (withResponse)
            {
                throw new BluezException(BluezErrors.NotPermitted, "CL-IN is write-without-response only");
            }

            lock (gate)
            {
                ControllerLinkWrites.Add(value);
            }

            return Task.CompletedTask;
        }

        if (characteristicPath == MicUpPath)
        {
            if (withResponse)
            {
                throw new BluezException(BluezErrors.NotPermitted, "MIC-UP is write-without-response only");
            }

            if (MicWriteFailure is { } micFailure)
            {
                throw micFailure;
            }

            lock (gate)
            {
                MicWrites.Add(value);
            }

            return Task.CompletedTask;
        }

        if (characteristicPath != RxPath || !withResponse)
        {
            throw new BluezException(BluezErrors.NotPermitted, "unexpected write");
        }

        string? command = null;
        lock (gate)
        {
            RxWrites.Add(value);
            foreach (var b in value)
            {
                if (b == (byte)'\n')
                {
                    command = System.Text.Encoding.UTF8.GetString(pendingCommand.ToArray());
                    pendingCommand.Clear();
                }
                else
                {
                    pendingCommand.Add(b);
                }
            }
        }

        if (command is not null && Replies.TryGetValue(command, out var reply))
        {
            // Split across two notifications, the way a reply larger than one
            // ATT payload arrives.
            var bytes = System.Text.Encoding.UTF8.GetBytes(reply + "\n");
            var half = bytes.Length / 2;
            _ = Task.Run(() =>
            {
                txNotification?.Invoke(bytes[..half]);
                txNotification?.Invoke(bytes[half..]);
            });
        }

        return Task.CompletedTask;
    }

    public Task<IAsyncDisposable> SubscribeAsync(string characteristicPath, Action<byte[]> notification, CancellationToken cancellationToken = default)
    {
        if (characteristicPath == TxPath && SubscribeFailure is { } failure)
        {
            throw failure;
        }

        if (characteristicPath == TxPath)
        {
            txNotification = notification;
        }
        else if (characteristicPath == ClOutPath)
        {
            clOutNotification = notification;
        }
        else if (characteristicPath == AudioDownPath)
        {
            audioDownNotification = notification;
        }

        ActiveSubscriptions++;
        return Task.FromResult<IAsyncDisposable>(new Disposable(() =>
        {
            ActiveSubscriptions--;
            if (characteristicPath == TxPath)
            {
                txNotification = null;
            }
            else if (characteristicPath == AudioDownPath)
            {
                audioDownNotification = null;
            }
        }));
    }

    public Task<int?> GetMtuAsync(string characteristicPath, CancellationToken cancellationToken = default) =>
        Task.FromResult(Mtu);

    public Task<IDisposable> WatchDisconnectAsync(string devicePath, Action onDisconnected, CancellationToken cancellationToken = default)
    {
        disconnected = onDisconnected;
        return Task.FromResult<IDisposable>(new Disposable(() => disconnected = null));
    }

    public Task<IDisposable> WatchDisconnectReasonAsync(string devicePath, Action<string, string> onDisconnected, CancellationToken cancellationToken = default)
    {
        disconnectReason = onDisconnected;
        return Task.FromResult<IDisposable>(new Disposable(() => disconnectReason = null));
    }

    public Task PairAsync(string devicePath, CancellationToken cancellationToken = default)
    {
        Pairs++;
        if (PairFailure is { } failure)
        {
            throw failure;
        }

        Paired = true;
        return Task.CompletedTask;
    }

    public Task SetTrustedAsync(string devicePath, bool trusted, CancellationToken cancellationToken = default)
    {
        Trusted = trusted;
        return Task.CompletedTask;
    }

    public Task RemoveDeviceAsync(string adapterPath, string devicePath, CancellationToken cancellationToken = default)
    {
        Removes++;
        if (RemoveFailure is { } failure)
        {
            throw failure;
        }

        DeviceKnown = false;
        Paired = false;
        return Task.CompletedTask;
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    /// <summary>The link dropped underneath the app.</summary>
    public void DropLink()
    {
        Connected = false;
        disconnected?.Invoke();
    }

    /// <summary>A Controller Link feedback frame from the adapter.</summary>
    public void RaiseControllerLinkOutput(byte[] frame) => clOutNotification?.Invoke(frame);

    /// <summary>A companion audio frame from the adapter.</summary>
    public void RaiseAudioDown(byte[] frame) => audioDownNotification?.Invoke(frame);

    public bool AudioDownSubscribed => audioDownNotification is not null;

    /// <summary>A notification arriving after the session it belonged to.</summary>
    public void RaiseLateTx(byte[] payload) => txNotification?.Invoke(payload);

    private sealed class Disposable(Action dispose) : IAsyncDisposable, IDisposable
    {
        private int done;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref done, 1) == 0)
            {
                dispose();
            }
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
