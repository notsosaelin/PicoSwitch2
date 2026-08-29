using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Persistence, against a real temporary directory.
///
/// A fake filesystem would prove nothing here: the properties under test are that
/// an interrupted write leaves the previous document intact and that a corrupt
/// file on disk does not stop the app launching, and both are about actual files.
/// </summary>
public sealed class AdapterRegistryStoreTests : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(),
        "picoswitch-tests-" + Guid.NewGuid().ToString("N"));

    private readonly WindowsDocumentStore documents;

    public AdapterRegistryStoreTests() => documents = new WindowsDocumentStore(root);

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

    [Fact]
    public void AnAbsentDocumentLoadsAsNothingKnown()
    {
        Assert.Empty(new AdapterRegistryStore(documents).Load().Records);
        Assert.Empty(new PeerHistoryStore(documents).Load().ByAdapter);
    }

    [Fact]
    public void TheRegistrySurvivesARestart()
    {
        var store = new AdapterRegistryStore(documents);
        var id = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        var registry = new AdapterRegistry()
            .With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with { UserAlias = "Living room" })
            .Selecting(id);

        Assert.True(store.Save(registry));

        // A different store instance, as a relaunch would build.
        var reloaded = new AdapterRegistryStore(new WindowsDocumentStore(root)).Load();
        Assert.Equal(id, reloaded.ActiveId);
        Assert.Equal("Living room", Assert.Single(reloaded.Records).UserAlias);
    }

    [Fact]
    public void PeerHistorySurvivesARestart()
    {
        var id = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        var inventory = new PeerInventory
        {
            Peers = new ValueList<PeerInfo>(
            [
                new PeerInfo("p_1", "AA:BB:CC:DD:EE:FF", PeerRole.PhysicalController,
                    PeerTransportSet.Of(PeerTransport.Classic), Bonded: true, Name: "DualSense"),
            ]),
            Complete = true,
            Total = 1,
        };

        var store = new PeerHistoryStore(documents);
        Assert.True(store.Save(new PeerHistoryBook()
            .With(id, new AdapterPeerHistory().Observing(inventory, 1_000))));

        var reloaded = new PeerHistoryStore(new WindowsDocumentStore(root)).Load();
        Assert.Equal("DualSense", Assert.Single(reloaded.ForAdapter(id).Records).LastKnownName);
    }

    [Fact]
    public void ACorruptDocumentCostsItsContentsAndNotTheLaunch()
    {
        Directory.CreateDirectory(root);
        File.WriteAllText(Path.Combine(root, AdapterRegistryStore.DocumentName), "{not json");
        Assert.Empty(new AdapterRegistryStore(documents).Load().Records);
    }

    [Fact]
    public void AFailedWriteLeavesThePreviousDocumentIntact()
    {
        var store = new AdapterRegistryStore(documents);
        var good = new AdapterRegistry().With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!);
        Assert.True(store.Save(good));

        // Hold the destination open exclusively, which is what an interrupted or
        // contended write looks like from here.
        var path = documents.PathFor(AdapterRegistryStore.DocumentName);
        using (var _ = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            Assert.False(store.Save(new AdapterRegistry()));
        }

        // The registry is read before anything else can be shown; a half-written
        // document would be an empty adapter list on next launch.
        Assert.Single(store.Load().Records);
    }

    [Fact]
    public void NoTemporaryFileIsLeftBehindAfterASuccessfulWrite()
    {
        var store = new AdapterRegistryStore(documents);
        Assert.True(store.Save(new AdapterRegistry().With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!)));
        Assert.Empty(Directory.GetFiles(root, "*.tmp"));
    }

    [Fact]
    public void TheTwoDocumentsAreIndependent()
    {
        // Different lifetimes and different write rates: history changes on every
        // complete peer read, and rewriting the registry on a refresh would be both
        // wasteful and a wider blast radius for a bad write.
        Assert.NotEqual(AdapterRegistryStore.DocumentName, PeerHistoryStore.DocumentName);

        new AdapterRegistryStore(documents).Save(
            new AdapterRegistry().With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!));
        Assert.Empty(new PeerHistoryStore(documents).Load().ByAdapter);
    }

    [Fact]
    public void TheDefaultLocationIsUnderLocalAppDataSoBothPackageFlavoursAgree()
    {
        // ApplicationData.Current throws in an unpackaged process, and under MSIX
        // %LOCALAPPDATA% is redirected into the package's own folder — so this one
        // path resolves correctly in both flavours without the app knowing which it
        // is.
        var store = new WindowsDocumentStore();
        Assert.StartsWith(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            store.Directory,
            StringComparison.OrdinalIgnoreCase);
        Assert.EndsWith(WindowsDocumentStore.DefaultFolderName, store.Directory);
    }
}
