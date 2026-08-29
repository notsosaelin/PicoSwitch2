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
        Pairing = new FakePairingGateway();
    }

    public TrustingTransport Transport { get; }

    public FakePairingGateway Pairing { get; }

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
        }),
        pairing: Pairing);

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

        // A remembered adapter is a PAIRED adapter. Leaving the gateway at its
        // default would make the service reach RepairRequired before it ever tried
        // to connect, and every test built on this fixture would be testing that
        // instead of what it meant to.
        Pairing.State = WindowsPairingKnown.Paired;
        Transport.WindowsPaired = true;

        await Service.ConnectAsync(Id);
        Assert.Equal(AdapterRelationshipPhase.Connected, Service.Relationship.Value.Phase);
        Transport.ResetCounters();
    }

    /// <summary>
    /// Model the adapter being flashed, exactly as the hardware behaved on
    /// 2026-08-29: the link drops, Windows keeps its pairing, the adapter is still
    /// advertising, and every route to its GATT server returns
    /// <c>Unreachable</c> at service discovery.
    ///
    /// The disconnect is not scaffolding -- a reconnect request is deliberately
    /// inert while the relationship is Connected, so without the link actually
    /// dropping there is no attempt for the signature to classify.
    /// </summary>
    public async Task ReflashAsync(GattFailureStage stage = GattFailureStage.Services)
    {
        await Service.DisconnectAsync();
        Pairing.State = WindowsPairingKnown.Paired;
        Transport.WindowsPaired = true;
        Transport.PeerObserved = true;
        Transport.PeerAnsweredGatt = false;

        // NOT preset: the fake accumulates it per failing connect, exactly as the
        // real transport does. Scripting the total here would let a test reach the
        // signature without the fallback ever running, which is the one thing the
        // link-layer shape actually requires.
        Transport.LinkFailuresAfterResolve = 0;
        Transport.FailDirectConnect = new GattTransportException(
            "unreachable",
            stage,
            GattCommunicationOutcome.Unreachable);
        Transport.FailScanConnect = new GattTransportException(
            "unreachable",
            stage,
            GattCommunicationOutcome.Unreachable);
    }

    /// <summary>
    /// The other stale-bond shape: an attribute-layer refusal, conclusive on the
    /// first failure. Retained because a different radio or Windows build may
    /// produce it even though this hardware did not.
    /// </summary>
    public async Task ReflashWithAttributeRefusalAsync(GattFailureStage stage)
    {
        await Service.DisconnectAsync();
        Pairing.State = WindowsPairingKnown.Paired;
        Transport.WindowsPaired = true;
        Transport.PeerObserved = true;
        Transport.PeerAnsweredGatt = true;
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
