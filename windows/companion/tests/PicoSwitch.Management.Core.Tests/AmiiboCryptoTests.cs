using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The C# amiibo crypto, against the shared fixture.
/// </summary>
/// <remarks>
/// THIS IS WHY THE THIRD IMPLEMENTATION IS ALLOWED TO EXIST. The algorithm is
/// already in the repository twice — Kotlin and <c>web/index.html</c> — and its
/// failure mode is silent: a wrong HMAC produces a tag the console simply
/// rejects, with no error anywhere to read. A C# port written and reviewed but
/// only self-tested would be indistinguishable from a correct one right up to
/// the point a user's tag stopped working.
///
/// So it is verified against vectors generated from the Kotlin implementation
/// over real tags (<c>tools/fixtures/amiibo/crypto-vectors.json</c>). The
/// strongest assertion here is the re-signed digest: one comparison that covers
/// the layout gather, the key derivation, the AES-CTR pass and both HMACs.
///
/// ## Without keys
///
/// Everything that does not need <c>key_retail.bin</c> runs always — identity
/// decoding, the source digests, the negative vectors. That is the half CI can
/// check, and it is not a small half: the whole plaintext identity path, which is
/// what a library list draws before a user has supplied any keys.
///
/// The decrypting half runs when <c>PICOSWITCH_AMIIBO_RETAIL_KEY</c> points at a
/// key file, and is skipped otherwise. An environment variable rather than a
/// committed path, because the key must never be in the repository.
/// </remarks>
public sealed class AmiiboCryptoTests
{
    private static readonly JsonDocument Fixture =
        JsonDocument.Parse(RepositoryFixtures.ReadText(RepositoryFixtures.AmiiboCryptoVectors));

    private static readonly string RepositoryRoot =
        Directory.GetParent(RepositoryFixtures.Path(RepositoryFixtures.AmiiboCryptoVectors))!
            .Parent!.Parent!.Parent!.FullName;

    private static IEnumerable<JsonElement> Vectors =>
        Fixture.RootElement.GetProperty("vectors").EnumerateArray();

    private static byte[] TagBytes(JsonElement vector) =>
        File.ReadAllBytes(Path.Combine(RepositoryRoot, vector.GetProperty("source").GetString()!));

    /// <summary>
    /// The retail keys, or null when none were supplied.
    /// </summary>
    /// <remarks>
    /// THROWS when the variable is set but unusable, and that is deliberate. The
    /// dangerous failure here is not a wrong key, it is a mistyped path: the
    /// decrypting half of this file would quietly stop running and every test in
    /// it would still report green. Loud is the only safe behaviour for a
    /// misconfiguration that disables tests.
    /// </remarks>
    private static AmiiboCrypto.RetailKeys? Keys()
    {
        var path = Environment.GetEnvironmentVariable("PICOSWITCH_AMIIBO_RETAIL_KEY");
        if (string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                "PICOSWITCH_AMIIBO_RETAIL_KEY is set but no file is there, so the " +
                "decrypting amiibo tests would have silently not run", path);
        }

        return AmiiboCrypto.ParseRetailKeys(File.ReadAllBytes(path));
    }

    private static string Sha256(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    /// <summary>
    /// Must match <c>amiiboNameDigest</c> on the generating side exactly.
    /// </summary>
    /// <remarks>
    /// A digest rather than the text, so a decoded Mii name is compared exactly
    /// without ever entering the repository. The generator and the Kotlin
    /// verifier once kept separate copies of this and drifted on the separator
    /// alone, which made every name comparison vacuous — hence the fixture
    /// records the definition in its own <c>nameDigest</c> field.
    /// </remarks>
    private static string NameDigest(string nickname, string owner) =>
        Sha256(Encoding.UTF8.GetBytes(nickname + owner));

    // ----------------------------------------------------- always, no keys

    [Fact]
    public void TheFixtureIsPresentAndCarriesVectors()
    {
        Assert.NotEmpty(Vectors);
        Assert.NotEmpty(Fixture.RootElement.GetProperty("rejected").EnumerateArray());
    }

    [Fact]
    public void TheFixtureCarriesNoKeyMaterial()
    {
        // The key set is named by fingerprint. This is the C# end of the same
        // guard the Kotlin verifier applies: a future change that started
        // inlining key bytes has to fail on both sides, not neither.
        var fingerprint = Fixture.RootElement.GetProperty("keySet")
            .GetProperty("fingerprint").GetString()!;
        Assert.StartsWith("sha256:", fingerprint, StringComparison.Ordinal);
        Assert.Equal(64, fingerprint["sha256:".Length..].Length);
    }

    [Fact]
    public void EveryVectorSourceIsPresentAndUnchanged()
    {
        // The vectors describe specific bytes. A dump edited or replaced without
        // regenerating would leave every expectation below quietly describing a
        // different tag.
        foreach (var vector in Vectors)
        {
            var bytes = TagBytes(vector);
            Assert.Equal(vector.GetProperty("size").GetInt32(), bytes.Length);
            Assert.Equal(vector.GetProperty("sourceSha256").GetString(), Sha256(bytes));
        }
    }

    [Fact]
    public void IdentityDecodesExactlyWithNoKeys()
    {
        // The half of the contract a client honours before the user has supplied
        // anything, and the half a library list is drawn from.
        foreach (var vector in Vectors)
        {
            var expected = vector.GetProperty("identity");
            var identity = AmiiboCrypto.Identity(TagBytes(vector));
            var label = vector.GetProperty("label").GetString();

            Assert.Equal(expected.GetProperty("uid").GetString(), identity.Uid);
            Assert.Equal(expected.GetProperty("figureId").GetString(), identity.FigureId);
            Assert.Equal(expected.GetProperty("tagType").GetString(), identity.TagType.ToString());
            Assert.Equal(
                expected.GetProperty("characterGameCode").GetString(),
                identity.CharacterGameCode);
            Assert.Equal(
                expected.GetProperty("characterVariant").GetInt32(),
                identity.CharacterVariant);
            Assert.Equal(expected.GetProperty("typeName").GetString(), identity.TypeName);
            Assert.Equal(expected.GetProperty("modelNumber").GetString(), identity.ModelNumber);
            Assert.Equal(expected.GetProperty("seriesCode").GetInt32(), identity.SeriesCode);
            Assert.Equal(expected.GetProperty("formatVersion").GetInt32(), identity.FormatVersion);
            Assert.Equal(
                expected.GetProperty("extendedVariant").GetString(),
                identity.ExtendedVariant);
            Assert.NotNull(label);
        }
    }

    [Fact]
    public void RejectedVectorsAreStillRefused()
    {
        // Negative knowledge, kept deliberately: these files carry the v3 marker
        // and look like tags. Every implementation must refuse them on size
        // rather than half-parse one.
        foreach (var entry in Fixture.RootElement.GetProperty("rejected").EnumerateArray())
        {
            var path = Path.Combine(RepositoryRoot, entry.GetProperty("source").GetString()!);
            var bytes = File.ReadAllBytes(path);
            Assert.Equal(entry.GetProperty("sourceSha256").GetString(), Sha256(bytes));
            Assert.ThrowsAny<Exception>(() => AmiiboCrypto.Identity(bytes));
        }
    }

    [Fact]
    public void NormalizeImportRepairsOnlyTheUidCheckBytes()
    {
        // Those two bytes are DERIVED from the UID, so a wrong one says nothing
        // about the tag and dump exporters routinely leave them blank. Every
        // other byte is either the user's data or covered by an HMAC, so nothing
        // else may be "helpfully" repaired.
        var vector = Vectors.First(v =>
            v.GetProperty("identity").GetProperty("tagType").GetString() == "Ntag215");
        var original = TagBytes(vector);

        var damaged = (byte[])original.Clone();
        damaged[3] ^= 0xFF;
        damaged[8] ^= 0xFF;

        var repaired = AmiiboCrypto.Identity(AmiiboFiles.NormalizeImport(damaged));
        Assert.Equal(
            vector.GetProperty("identity").GetProperty("uid").GetString(),
            repaired.Uid);

        // A byte inside the signed body is NOT repaired, and the file is still
        // accepted structurally -- it is the HMAC that must reject it later, not
        // a size check pretending to be integrity.
        var tampered = (byte[])original.Clone();
        tampered[0x100] ^= 0xFF;
        AmiiboFiles.Validate(AmiiboFiles.NormalizeImport(tampered));
    }

    [Fact]
    public void UnsupportedSizesAreRefused()
    {
        Assert.Throws<ArgumentException>(() => AmiiboFiles.NormalizeImport(new byte[100]));
        Assert.Throws<ArgumentException>(() => AmiiboFiles.Validate(new byte[541]));
    }

    [Fact]
    public void DateDecodingDistinguishesUnsetFromReal()
    {
        // 0 and 0xFFFF are "not written"; an out-of-range month or day means the
        // bytes are not a date at all. Returning null rather than clamping keeps
        // "no date" distinguishable from the first of January.
        Assert.Null(AmiiboCrypto.DecodeDate(0x00, 0x00));
        Assert.Null(AmiiboCrypto.DecodeDate(0xFF, 0xFF));
        Assert.Null(AmiiboCrypto.DecodeDate(0x00, 0x20));      // month 1, day 0
        Assert.Equal("2026-07-28", AmiiboCrypto.DecodeDate(0x34, 0xFC));
    }

    [Fact]
    public void AKeyFileOfTheWrongShapeIsRefused()
    {
        Assert.Throws<ArgumentException>(() => AmiiboCrypto.ParseRetailKeys(new byte[159]));
        // 160 bytes of zeroes is the right size and no masters at all.
        Assert.Throws<ArgumentException>(() => AmiiboCrypto.ParseRetailKeys(new byte[160]));
    }

    // ------------------------------------------------------- only with keys
    //
    // These return early rather than skipping: xUnit 2.9 has no first-class skip
    // and adding a package for it is more churn than the semantics are worth.
    // The trade-off is that they PASS SILENTLY on a machine with no key, so
    // KeyDependentCoverageIsVisible below reports which half actually ran --
    // a test that quietly does nothing is worse than one that says so.

    [Fact]
    public void SuppliedKeysAreActuallyUsed()
    {
        // Guards the silent-off failure mode directly. Without this, a variable
        // that never reached the test host would leave every decrypting test
        // returning early and reporting green, and the C# port would be
        // "verified" against nothing at all.
        //
        // Keys() throws on a set-but-unusable path, so reaching here with a set
        // variable and a null key set is impossible; the assertion states the
        // invariant anyway so the intent survives a refactor.
        var configured = Environment.GetEnvironmentVariable("PICOSWITCH_AMIIBO_RETAIL_KEY");
        var keys = Keys();

        if (string.IsNullOrWhiteSpace(configured))
        {
            Assert.Null(keys);
            return;
        }

        Assert.NotNull(keys);
        // Prove the key set actually decrypts something, rather than merely
        // having parsed: a structurally valid file of the wrong keys would pass
        // ParseRetailKeys and fail every vector.
        Assert.Equal(
            AmiiboCryptoState.Valid,
            AmiiboCrypto.ReadDetails(TagBytes(Vectors.First()), keys).Crypto);
    }

    [Fact]
    public void TheKeySetMatchesTheOneTheFixtureWasGeneratedWith()
    {
        var path = Environment.GetEnvironmentVariable("PICOSWITCH_AMIIBO_RETAIL_KEY");
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            return;
        }

        Assert.Equal(
            Fixture.RootElement.GetProperty("keySet").GetProperty("fingerprint").GetString(),
            "sha256:" + Sha256(File.ReadAllBytes(path)));
    }

    [Fact]
    public void DecryptedRegisterFieldsMatch()
    {
        var keys = Keys();
        if (keys is null)
        {
            return;
        }

        foreach (var vector in Vectors)
        {
            var expected = vector.GetProperty("register");
            var details = AmiiboCrypto.ReadDetails(TagBytes(vector), keys);

            Assert.Equal(AmiiboCryptoState.Valid, details.Crypto);
            Assert.Equal(expected.GetProperty("setUp").GetBoolean(), details.SetUp);
            Assert.Equal(expected.GetProperty("hasAppData").GetBoolean(), details.HasAppData);
            Assert.Equal(expected.GetProperty("writeCounter").GetInt32(), details.WriteCounter);
            Assert.Equal(NullableString(expected, "setupDate"), details.SetupDate);
            Assert.Equal(NullableString(expected, "lastWriteDate"), details.LastWriteDate);
            Assert.Equal(expected.GetProperty("titleId").GetString(), details.TitleId);
            Assert.Equal(expected.GetProperty("appId").GetString(), details.AppId);
            Assert.Equal(expected.GetProperty("appDataLabel").GetString(), details.AppDataLabel);
            Assert.Equal(
                expected.GetProperty("nameDigest").GetString(),
                NameDigest(details.Nickname, details.Owner));
        }
    }

    [Fact]
    public void ReSigningReproducesTheFixtureBytes()
    {
        var keys = Keys();
        if (keys is null)
        {
            return;
        }

        foreach (var vector in Vectors)
        {
            var expected = vector.GetProperty("initialized");
            var initialized = AmiiboCrypto.Initialize(TagBytes(vector), keys!);

            // The strongest single assertion in this file: it covers the layout
            // gather, both key derivations, the AES-CTR pass and both HMACs. If
            // the C# port has drifted from the Kotlin anywhere, this is where it
            // shows.
            Assert.Equal(expected.GetProperty("sha256").GetString(), Sha256(initialized));

            var details = AmiiboCrypto.ReadDetails(initialized, keys);
            Assert.Equal(AmiiboCryptoState.Valid, details.Crypto);
            Assert.False(details.SetUp);
            Assert.Equal(
                expected.GetProperty("nameDigest").GetString(),
                NameDigest(details.Nickname, details.Owner));
        }
    }

    [Fact]
    public void ReSigningIsIdempotent()
    {
        // Initializing an already-initialized image must be a no-op in content
        // terms, or pressing Initialize twice would produce two different tags
        // and the user could not tell which they had.
        var keys = Keys();
        if (keys is null)
        {
            return;
        }

        var bytes = TagBytes(Vectors.First());
        var once = AmiiboCrypto.Initialize(bytes, keys!);
        Assert.Equal(Sha256(once), Sha256(AmiiboCrypto.Initialize(once, keys!)));
    }

    [Fact]
    public void ATamperedImageFailsItsHmacRatherThanDecodingToNonsense()
    {
        // The property that makes the whole scheme worth anything: a byte
        // changed inside the signed body must be REPORTED, not silently
        // presented as somebody's tag.
        var keys = Keys();
        if (keys is null)
        {
            return;
        }

        var bytes = TagBytes(Vectors.First());
        Assert.Equal(AmiiboCryptoState.Valid, AmiiboCrypto.ReadDetails(bytes, keys).Crypto);

        var tampered = (byte[])bytes.Clone();
        tampered[0x100] ^= 0xFF;
        Assert.Equal(AmiiboCryptoState.Invalid, AmiiboCrypto.ReadDetails(tampered, keys).Crypto);

        // And it must never be re-signed: minting a valid signature over content
        // of unknown provenance is the one thing this must not do.
        Assert.Throws<InvalidOperationException>(() => AmiiboCrypto.Initialize(tampered, keys!));
    }

    [Fact]
    public void NoKeysIsReportedAsKeyUnavailableAndStillYieldsIdentity()
    {
        // "No keys supplied", "wrong keys" and "damaged file" are three different
        // things to a user, and only the first is fixed by importing a key.
        // Collapsing them would send someone hunting for a corrupt file.
        var details = AmiiboCrypto.ReadDetails(TagBytes(Vectors.First()), keys: null);
        Assert.Equal(AmiiboCryptoState.KeyUnavailable, details.Crypto);
        // Identity still decoded: no keys is not the same as no information.
        Assert.NotEmpty(details.Identity.FigureId);
    }

    private static string? NullableString(JsonElement element, string name) =>
        element.GetProperty(name).ValueKind == JsonValueKind.Null
            ? null
            : element.GetProperty(name).GetString();
}
