using System.Security.Cryptography;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Windows.Storage;

/// <summary>
/// The user's amiibo retail key set, protected at rest by DPAPI.
/// </summary>
/// <remarks>
/// THE ONE PIECE OF GENUINELY SENSITIVE MATERIAL THIS APP HOLDS. Everything else
/// it stores — profiles, tag backups, adapter records — is the user's own data;
/// this is a key set they were not supposed to have to manage, and it must never
/// leave the machine it was imported on.
///
/// Protected with <see cref="DataProtectionScope.CurrentUser"/>, so the ciphertext
/// is bound to this Windows user account: copying the file to another machine, or
/// reading it as another user, yields nothing. That is a meaningful improvement
/// over the Android side, which relies on the app sandbox and a backup opt-out.
///
/// DPAPI is not a substitute for the file being private, and it is not claimed to
/// be. Anything running AS this user can unprotect it, exactly as anything in the
/// app's process can. What it defends against is the realistic case: the file
/// being copied out, landing in a sync folder, or ending up in a backup image.
///
/// ## Rules this type enforces
///
/// The key is validated by <see cref="AmiiboCrypto.ParseRetailKeys"/> BEFORE it
/// is stored, so a wrong file is rejected at import rather than at the point a
/// user tries to read a tag. Nothing here logs, and no method returns key bytes —
/// callers get a parsed <see cref="AmiiboCrypto.RetailKeys"/> or a fingerprint,
/// never the raw material to mishandle.
/// </remarks>
public sealed class WindowsAmiiboKeyStore
{
    private const string FileName = "amiibo-retail-key.dpapi";

    /// <summary>
    /// Bound into the protection so a blob from another PicoSwitch2 store — or
    /// another application's DPAPI data — cannot be unprotected here by accident.
    /// </summary>
    private static readonly byte[] Entropy =
        "PicoSwitch2/amiibo-retail-key/v1"u8.ToArray();

    private readonly string path;

    public WindowsAmiiboKeyStore(string directory)
    {
        Directory.CreateDirectory(directory);
        path = Path.Combine(directory, FileName);
    }

    public bool Exists => File.Exists(path);

    /// <summary>
    /// The stored key set, or null when there is none or it cannot be read.
    /// </summary>
    /// <remarks>
    /// Total by design. A key blob that will not unprotect — a restored profile,
    /// a copied file, a new Windows account — is indistinguishable from having
    /// no key, and both mean the same thing to the user: import the file again.
    /// Throwing here would turn that into a crash on a page that should simply
    /// offer the import button.
    /// </remarks>
    public AmiiboCrypto.RetailKeys? Read()
    {
        if (!File.Exists(path))
        {
            return null;
        }

        byte[]? raw = null;
        try
        {
            raw = ProtectedData.Unprotect(
                File.ReadAllBytes(path), Entropy, DataProtectionScope.CurrentUser);
            return AmiiboCrypto.ParseRetailKeys(raw);
        }
        catch (Exception)
        {
            return null;
        }
        finally
        {
            if (raw is not null)
            {
                CryptographicOperations.ZeroMemory(raw);
            }
        }
    }

    /// <summary>
    /// Validate and store a key file the user supplied.
    /// </summary>
    /// <remarks>
    /// Parsed first: storing an unusable file would leave the app reporting a key
    /// as present while every tag failed to decrypt, which sends the user looking
    /// at their tags instead of at their key.
    ///
    /// Written through a temporary file so an interrupted import cannot leave a
    /// truncated blob in place of a working one.
    /// </remarks>
    public AmiiboCrypto.RetailKeys Import(byte[] raw)
    {
        var parsed = AmiiboCrypto.ParseRetailKeys(raw);

        var protectedBytes = ProtectedData.Protect(
            raw, Entropy, DataProtectionScope.CurrentUser);

        var temporary = path + ".tmp";
        File.WriteAllBytes(temporary, protectedBytes);
        File.Move(temporary, path, overwrite: true);

        // Deliberately NOT marked hidden. It bought nothing -- DPAPI is what
        // protects the contents -- and Windows refuses to overwrite a hidden file
        // through the ordinary create path, so it broke Clear() outright.
        return parsed;
    }

    /// <summary>
    /// Forget the key set.
    /// </summary>
    /// <remarks>
    /// Overwritten before deletion. On a journalling or copy-on-write filesystem
    /// that is not a guarantee the old blob is unrecoverable, and it is not
    /// claimed to be — it is the cheap part of doing this properly, and the
    /// ciphertext is useless off this account regardless.
    /// </remarks>
    public void Clear()
    {
        if (!File.Exists(path))
        {
            return;
        }

        try
        {
            var length = (int)new FileInfo(path).Length;
            File.WriteAllBytes(path, new byte[length]);
        }
        catch (IOException)
        {
            // Fall through to the delete; a failed scrub must not leave the file.
        }

        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
            // Nothing further to try, and the caller cannot act on it either.
        }
    }

    /// <summary>
    /// A digest of the stored key set, for telling two key files apart.
    /// </summary>
    /// <remarks>
    /// The same fingerprint the shared crypto fixture records, so a user can be
    /// told whether the keys they imported are the ones a vector set was built
    /// with. Deliberately the ONLY thing derived from the key that this type will
    /// hand out.
    /// </remarks>
    public string? Fingerprint()
    {
        if (!File.Exists(path))
        {
            return null;
        }

        byte[]? raw = null;
        try
        {
            raw = ProtectedData.Unprotect(
                File.ReadAllBytes(path), Entropy, DataProtectionScope.CurrentUser);
            return "sha256:" + Convert.ToHexString(SHA256.HashData(raw)).ToLowerInvariant();
        }
        catch (Exception)
        {
            return null;
        }
        finally
        {
            if (raw is not null)
            {
                CryptographicOperations.ZeroMemory(raw);
            }
        }
    }
}
