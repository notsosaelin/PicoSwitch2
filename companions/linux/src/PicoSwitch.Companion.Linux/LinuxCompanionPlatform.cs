using PicoSwitch.Companion.Linux.Bluetooth;
using PicoSwitch.Companion.Platform;
using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Companion.Platform.Storage;

namespace PicoSwitch.Companion.Linux;

/// <summary>
/// The BlueZ implementation of <see cref="ICompanionPlatform"/>.
///
/// Documents live in <c>$XDG_DATA_HOME/PicoSwitch2</c> (normally
/// <c>~/.local/share/PicoSwitch2</c>): .NET maps
/// <c>Environment.SpecialFolder.LocalApplicationData</c> to exactly that on
/// Linux, so the shared <see cref="DocumentStore"/> needs no Linux branch. On a
/// Steam Deck that directory is under <c>/home</c>, which survives SteamOS
/// updates; the executable itself holds no state.
/// </summary>
public sealed class LinuxCompanionPlatform : ICompanionPlatform, IAsyncDisposable
{
    private readonly DBusBluezBus bus = new();

    /// <param name="documentsRoot">
    /// Overrides the document directory. Production passes null; tooling (the
    /// screenshot harness) passes a scratch directory so it can never read or
    /// write a real user's adapters, profiles or amiibo.
    /// </param>
    public LinuxCompanionPlatform(string? documentsRoot = null)
    {
        Documents = new DocumentStore(documentsRoot);
        Pairing = new BluezPairingGateway(bus);
    }

    public string Name => "linux";

    public DocumentStore Documents { get; }

    public IAdapterPairingGateway Pairing { get; }

    public IManagementTransport CreateManagementTransport(Action<string, string> log) =>
        new BluezManagementTransport(bus, ProbeRadioAsync, log);

    public Task<BluetoothRadioCapabilities> ProbeRadioAsync() => BluezRadio.ProbeAsync(bus);

    public ValueTask DisposeAsync() => bus.DisposeAsync();
}
