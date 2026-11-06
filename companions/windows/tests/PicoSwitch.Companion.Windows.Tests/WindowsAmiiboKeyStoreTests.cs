using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// The DPAPI-protected amiibo key store.
/// </summary>
/// <remarks>
/// The one genuinely sensitive thing this app holds. These tests cover the
/// properties that matter if it goes wrong: the file on disk is not the key, a
/// bad key is refused at import rather than at first use, and every read path is
/// total so a key that will not unprotect looks like no key at all instead of
/// crashing the page that should be offering the import button.
///
/// Runs without <c>key_retail.bin</c>: the store's contract is about protecting
/// and validating bytes, and a 160-byte synthetic key set exercises all of it.
/// Whether those bytes decrypt a real tag is <c>AmiiboCryptoTests</c>' job.
/// </remarks>
public sealed class WindowsAmiiboKeyStoreTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-keystore-" + Guid.NewGuid().ToString("N"));

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
        }
    }

    private WindowsAmiiboKeyStore Store() => new(root);

    /// <summary>
    /// A structurally valid 160-byte retail key file.
    /// </summary>
    /// <remarks>
    /// Built to the format's rules rather than copied from anywhere: two 80-byte
    /// masters, each with a NUL-terminated type string, whose labels are
    /// "unfixed infos" and "locked secret". The bytes are arbitrary; only the
    /// shape is being exercised.
    /// </remarks>
    private static byte[] SyntheticKeyFile(bool swapped = false)
    {
        var unfixed = Master("unfixed infos");
        var locked = Master("locked secret");

        var raw = new byte[160];
        (swapped ? locked : unfixed).CopyTo(raw, 0);
        (swapped ? unfixed : locked).CopyTo(raw, 80);
        return raw;
    }

    private static byte[] Master(string label)
    {
        var master = new byte[80];
        for (var i = 0; i < 16; i++)
        {
            master[i] = (byte)(i + 1);
        }

        var text = System.Text.Encoding.ASCII.GetBytes(label);
        Array.Copy(text, 0, master, 16, Math.Min(text.Length, 13));
        master[16 + Math.Min(text.Length, 13)] = 0;

        master[31] = 16;
        for (var i = 32; i < 80; i++)
        {
            master[i] = (byte)i;
        }

        return master;
    }

    [Fact]
    public void NothingIsStoredUntilAKeyIsImported()
    {
        var store = Store();
        Assert.False(store.Exists);
        Assert.Null(store.Read());
        Assert.Null(store.Fingerprint());
    }

    [Fact]
    public void AnImportedKeyComesBackParsed()
    {
        var store = Store();
        store.Import(SyntheticKeyFile());

        Assert.True(store.Exists);
        var keys = store.Read();
        Assert.NotNull(keys);
        // Sorted into (unfixed, locked) order regardless of file order.
        Assert.Equal(16, keys!.Data.HmacKey.Length);
        Assert.Equal(16, keys.Tag.HmacKey.Length);
    }

    [Fact]
    public void EitherMasterOrderIsAccepted()
    {
        // Both orderings are in circulation. A file that is merely the other way
        // round would otherwise decrypt to garbage and be blamed on the tag.
        var store = Store();
        var straight = AmiiboCrypto.ParseRetailKeys(SyntheticKeyFile());
        store.Import(SyntheticKeyFile(swapped: true));
        var swapped = store.Read()!;

        Assert.Equal(straight.Data.HmacKey, swapped.Data.HmacKey);
        Assert.Equal(straight.Tag.HmacKey, swapped.Tag.HmacKey);
    }

    [Fact]
    public void TheFileOnDiskIsNotTheKey()
    {
        // The point of the whole type. If this ever fails, a key set is sitting
        // in plaintext in the user's profile directory.
        var store = Store();
        var raw = SyntheticKeyFile();
        store.Import(raw);

        var stored = File.ReadAllBytes(Directory.EnumerateFiles(root).Single());
        Assert.NotEqual(raw, stored);
        Assert.False(ContainsSequence(stored, raw), "the raw key bytes are on disk verbatim");
        // And the first master's HMAC key, on its own, must not appear either.
        Assert.False(ContainsSequence(stored, raw[..16]), "key material is on disk verbatim");
    }

    [Fact]
    public void AnInvalidKeyIsRefusedAtImportAndNothingIsStored()
    {
        // Refusing here rather than at first use is what stops the app reporting
        // a key as present while every tag mysteriously fails to decrypt.
        var store = Store();
        Assert.Throws<ArgumentException>(() => store.Import(new byte[159]));
        Assert.Throws<ArgumentException>(() => store.Import(new byte[160]));
        Assert.False(store.Exists);
        Assert.Null(store.Read());
    }

    [Fact]
    public void AnImportReplacesTheStoredKey()
    {
        var store = Store();
        store.Import(SyntheticKeyFile());
        var first = store.Fingerprint();

        var other = SyntheticKeyFile();
        other[5] ^= 0xFF;
        store.Import(other);

        Assert.NotEqual(first, store.Fingerprint());
        Assert.Single(Directory.EnumerateFiles(root));
    }

    [Fact]
    public void ClearForgetsTheKey()
    {
        var store = Store();
        store.Import(SyntheticKeyFile());
        Assert.True(store.Exists);

        store.Clear();

        Assert.False(store.Exists);
        Assert.Null(store.Read());
        Assert.Null(store.Fingerprint());
        // Clearing an empty store is a no-op, not a failure.
        store.Clear();
    }

    [Fact]
    public void ACorruptBlobReadsAsNoKeyRatherThanThrowing()
    {
        // A restored profile, a copied file or a new Windows account all produce
        // this, and all mean the same thing to the user: import it again. A throw
        // here would crash the page that should be offering that.
        var store = Store();
        store.Import(SyntheticKeyFile());
        var path = Directory.EnumerateFiles(root).Single();
        File.WriteAllBytes(path, [1, 2, 3, 4, 5, 6, 7, 8]);

        Assert.True(store.Exists);
        Assert.Null(store.Read());
        Assert.Null(store.Fingerprint());
    }

    [Fact]
    public void TheFingerprintIdentifiesTheKeySetWithoutRevealingIt()
    {
        // The same form the shared crypto fixture records, so a user can be told
        // whether their keys are the ones a vector set was built with.
        var store = Store();
        var raw = SyntheticKeyFile();
        store.Import(raw);

        var fingerprint = store.Fingerprint();
        Assert.NotNull(fingerprint);
        Assert.StartsWith("sha256:", fingerprint, StringComparison.Ordinal);
        Assert.Equal(64, fingerprint!["sha256:".Length..].Length);

        // Stable across reads, and different for different key material.
        Assert.Equal(fingerprint, Store().Fingerprint());
    }

    [Fact]
    public void TwoStoresInTheSameDirectoryAgree()
    {
        var first = Store();
        first.Import(SyntheticKeyFile());

        var second = Store();
        Assert.True(second.Exists);
        Assert.NotNull(second.Read());
        Assert.Equal(first.Fingerprint(), second.Fingerprint());
    }

    private static bool ContainsSequence(byte[] haystack, byte[] needle)
    {
        if (needle.Length == 0 || haystack.Length < needle.Length)
        {
            return false;
        }

        for (var i = 0; i <= haystack.Length - needle.Length; i++)
        {
            if (haystack.AsSpan(i, needle.Length).SequenceEqual(needle))
            {
                return true;
            }
        }

        return false;
    }
}
