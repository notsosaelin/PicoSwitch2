using PicoSwitch.Companion.Platform;
using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Companion.Platform.Storage;
using PicoSwitch.Companion.Windows.Bluetooth;

namespace PicoSwitch.Companion.Windows;

/// <summary>The WinRT implementation of <see cref="ICompanionPlatform"/>.</summary>
public sealed class WindowsCompanionPlatform : ICompanionPlatform
{
    public string Name => "windows";

    /// <summary>
    /// <c>%LOCALAPPDATA%\PicoSwitch2</c>, which MSIX redirects into the package's
    /// own folder, so packaged and unpackaged builds read the same documents.
    /// </summary>
    public DocumentStore Documents { get; } = new();

    public IAdapterPairingGateway Pairing { get; } = new WindowsPairingGateway();

    public IManagementTransport CreateManagementTransport(Action<string, string> log) =>
        new BleGattManagementTransport(log);

    public Task<BluetoothRadioCapabilities> ProbeRadioAsync() => WindowsBluetoothRadio.ProbeAsync();
}
