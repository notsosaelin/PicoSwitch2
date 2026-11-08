using System.Security.Cryptography;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Storage;

/// <summary>
/// The user's amiibo retail key set: the Linux counterpart of
/// WindowsAmiiboKeyStore, with the same surface and the same rules.
/// </summary>
/// <remarks>
/// THE ONE PIECE OF GENUINELY SENSITIVE MATERIAL THIS APP HOLDS.
///
/// WHAT PROTECTS IT HERE, STATED HONESTLY. Windows encrypts the file with DPAPI
/// bound to the user account. Linux has no equivalent that is guaranteed to be
/// present: the Secret Service (GNOME Keyring, KWallet) exists on a typical
/// desktop but not reliably in SteamOS Game Mode, and a key that silently
/// cannot be read in Game Mode would look like a lost key. So the file is
/// stored in plain bytes with owner-only permissions (0600, in a 0700
/// directory), which is the Android companion's model: the operating system's
/// per-user access control, not encryption. It is kept in its own directory,
/// apart from the tag backups, so exporting or syncing a library folder cannot
/// sweep it along.
///
/// The key is validated by <see cref="AmiiboCrypto.ParseRetailKeys"/> BEFORE it
/// is stored. Nothing here logs, and no method returns key bytes.
/// </remarks>
public sealed class LinuxAmiiboKeyStore
{
    private const string FileName = "amiibo-retail-key.bin";

    private const UnixFileMode OwnerOnlyFile = UnixFileMode.UserRead | UnixFileMode.UserWrite;
    private const UnixFileMode OwnerOnlyDirectory = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;

    private readonly string path;

    public LinuxAmiiboKeyStore(string directory)
    {
        Directory.CreateDirectory(directory);
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(directory, OwnerOnlyDirectory);
        }

        path = Path.Combine(directory, FileName);
    }

    public bool Exists => File.Exists(path);

    /// <summary>The stored key set, or null when there is none or it cannot be read. Total by design.</summary>
    public AmiiboCrypto.RetailKeys? Read()
    {
        if (!File.Exists(path))
        {
            return null;
        }

        byte[]? raw = null;
        try
        {
            raw = File.ReadAllBytes(path);
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
    /// Validate and store a key file. The temporary file is created owner-only
    /// BEFORE the bytes are written, so there is no moment at which the key sits
    /// in a world-readable file.
    /// </summary>
    public AmiiboCrypto.RetailKeys Import(byte[] raw)
    {
        var parsed = AmiiboCrypto.ParseRetailKeys(raw);

        var temporary = path + ".tmp";
        var options = new FileStreamOptions { Mode = FileMode.Create, Access = FileAccess.Write };
        if (!OperatingSystem.IsWindows())
        {
            options.UnixCreateMode = OwnerOnlyFile;
        }

        using (var stream = new FileStream(temporary, options))
        {
            stream.Write(raw);
            stream.Flush(flushToDisk: true);
        }

        File.Move(temporary, path, overwrite: true);
        return parsed;
    }

    /// <summary>Forget the key set. Overwritten before deletion; not claimed to be unrecoverable.</summary>
    public void Clear()
    {
        if (!File.Exists(path))
        {
            return;
        }

        try
        {
            File.WriteAllBytes(path, new byte[(int)new FileInfo(path).Length]);
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
        }
    }

    /// <summary>A digest of the stored key set, for telling two key files apart. The only thing derived from it.</summary>
    public string? Fingerprint()
    {
        if (!File.Exists(path))
        {
            return null;
        }

        byte[]? raw = null;
        try
        {
            raw = File.ReadAllBytes(path);
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
