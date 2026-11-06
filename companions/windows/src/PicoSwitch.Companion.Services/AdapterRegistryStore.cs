using PicoSwitch.Companion.Windows.Storage;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The remembered adapters, and the peer history that goes with them.
///
/// Two documents rather than one, because they have different lifetimes and
/// different write rates: the registry changes when the user pairs, renames,
/// selects or removes an adapter, while history changes on every complete peer
/// read. Folding them together would rewrite the registry on a refresh, which is
/// both wasteful and a wider blast radius for a bad write.
///
/// Both are decoded TOTALLY. These are read before anything can be shown, so an
/// unreadable document must cost the user that document's contents and never
/// their ability to launch.
/// </summary>
public sealed class AdapterRegistryStore(WindowsDocumentStore documents)
{
    public const string DocumentName = "adapters.json";

    public AdapterRegistry Load() => AdapterRegistryCodec.Decode(documents.Read(DocumentName));

    public bool Save(AdapterRegistry registry) =>
        documents.Write(DocumentName, AdapterRegistryCodec.Encode(registry));
}

public sealed class PeerHistoryStore(WindowsDocumentStore documents)
{
    public const string DocumentName = "peer-history.json";

    public PeerHistoryBook Load() => PeerHistoryCodec.Decode(documents.Read(DocumentName));

    public bool Save(PeerHistoryBook book) =>
        documents.Write(DocumentName, PeerHistoryCodec.Encode(book));
}
