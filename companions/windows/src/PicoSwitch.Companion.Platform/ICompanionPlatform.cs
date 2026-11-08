using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Companion.Platform.Storage;

namespace PicoSwitch.Companion.Platform;

/// <summary>
/// The operating-system services the shared companion composes.
///
/// One implementation per host: <c>WindowsCompanionPlatform</c> (WinRT) and
/// <c>LinuxCompanionPlatform</c> (BlueZ over D-Bus). The shell passes its
/// platform to <c>AdapterConnectionService.CreateDefault</c> and never names a
/// transport itself, which keeps the layering rule "a page cannot reach the
/// object that owns the GATT session" true on every platform.
///
/// Deliberately narrow: exactly what the connection service needs to build its
/// production composition. Input sources and other per-feature backends are
/// composed by each shell directly, because they differ in shape per platform.
/// </summary>
public interface ICompanionPlatform
{
    /// <summary>Short host name for the diagnostic trail, e.g. "windows" or "linux".</summary>
    string Name { get; }

    /// <summary>Where the registry, peer history and profile documents live.</summary>
    DocumentStore Documents { get; }

    /// <summary>
    /// A new management transport. Called exactly once, inside
    /// <c>ManagementOwner</c>, which is what guarantees one live session.
    /// </summary>
    IManagementTransport CreateManagementTransport(Action<string, string> log);

    /// <summary>Probe the local radio. Never throws; an absent radio is a value.</summary>
    Task<BluetoothRadioCapabilities> ProbeRadioAsync();

    /// <summary>The host's pairing ceremony and unpair.</summary>
    IAdapterPairingGateway Pairing { get; }
}
