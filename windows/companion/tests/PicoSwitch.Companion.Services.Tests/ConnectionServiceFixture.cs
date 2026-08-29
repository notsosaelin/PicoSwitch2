using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Companion.Windows.Storage;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// A whole <see cref="AdapterConnectionService"/> over a scriptable transport, a
/// scripted radio and a real temporary document directory.
///
/// Real files rather than an in-memory store, because the service's own job
/// includes persisting the registry and history, and a fake filesystem would
/// prove nothing about that.
///
/// The service is built LAZILY, so a test can seed the registry document first
/// and then observe the service loading it exactly as a relaunch would.
/// </summary>
public sealed class ConnectionServiceFixture : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(),
        "picoswitch-svc-" + Guid.NewGuid().ToString("N"));

    private AdapterConnectionService? service;

    public ConnectionServiceFixture(TrustingTransport? transport = null)
    {
        Transport = transport ?? new TrustingTransport();
        Diagnostics = new DiagnosticLog();
        Documents = new WindowsDocumentStore(root);
    }

    public TrustingTransport Transport { get; }

    public DiagnosticLog Diagnostics { get; }

    public WindowsDocumentStore Documents { get; }

    public AdapterId Id { get; private set; }

    public AdapterConnectionService Service => service ??= new AdapterConnectionService(
        new AdapterRepository(Transport),
        Documents,
        Diagnostics,
        nowMillis: () => 1_000_000,

        // Scripted so the suite does not depend on the developing machine's own
        // Bluetooth radio being present and switched on.
        probeRadio: () => Task.FromResult(new BluetoothRadioCapabilities
        {
            RadioPresent = true,
            RadioOn = true,
            LowEnergySupported = true,
            CentralRoleSupported = true,
        }));

    /// <summary>
    /// Put one remembered, connected adapter in place.
    ///
    /// The row is written to disk before the service is built, so the service picks
    /// it up through its ordinary load path rather than through a back door.
    /// </summary>
    public async Task RememberAdapterAsync(string address)
    {
        Id = AdapterId.FromAddress(address)!.Value;
        Assert.True(new AdapterRegistryStore(Documents).Save(
            new AdapterRegistry().With(AdapterRecord.Of(address)!).Selecting(Id)));

        Transport.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        Transport.FailDirectConnect = null;

        await Service.ConnectAsync(Id);
        Assert.Equal(AdapterRelationshipPhase.Connected, Service.Relationship.Value.Phase);
        Transport.ResetCounters();
    }

    /// <summary>
    /// Model the adapter being flashed: the link drops, Windows keeps its pairing,
    /// and the adapter comes back with no key for us.
    ///
    /// The disconnect is not scaffolding -- a reconnect request is deliberately
    /// inert while the relationship is Connected, so without the link actually
    /// dropping there is no attempt for the signature to classify.
    /// </summary>
    public async Task ReflashAsync(GattFailureStage stage)
    {
        await Service.DisconnectAsync();
        Transport.WindowsPaired = true;
        Transport.PeerReachable = true;
        Transport.FailDirectConnect = new GattTransportException(
            "refused",
            stage,
            GattCommunicationOutcome.AccessDenied);
    }

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
        catch (IOException)
        {
            // A leaked temp directory is not worth failing a test run over.
        }
    }
}
