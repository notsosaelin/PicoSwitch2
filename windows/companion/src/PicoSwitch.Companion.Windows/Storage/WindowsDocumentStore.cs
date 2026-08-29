using System.Text;

namespace PicoSwitch.Companion.Windows.Storage;

/// <summary>
/// One named JSON document on disk, written atomically.
///
/// ## Why a plain path and not <c>ApplicationData.Current</c>
///
/// <c>ApplicationData.Current</c> throws in an UNPACKAGED process, and this app
/// ships both flavours from one project (packaged MSIX for release, unpackaged for
/// development). A store that worked in only one of them would mean the two
/// flavours read different files, so a developer's adapters would vanish the
/// moment they ran the packaged build.
///
/// Under MSIX, <c>%LOCALAPPDATA%</c> is redirected into the package's own local
/// folder by the runtime, so the SAME path resolves correctly in both flavours
/// without the app having to know which one it is.
///
/// ## Why the write is atomic
///
/// The registry is read before anything else can be shown. A half-written
/// document is not "some adapters missing", it is an unparseable file and an
/// empty adapter list on next launch. Write to a temporary file, flush, then
/// replace — so an interrupted write leaves the PREVIOUS document intact.
/// </summary>
public sealed class WindowsDocumentStore
{
    public const string DefaultFolderName = "PicoSwitch2";

    private readonly string directory;
    private readonly Lock gate = new();

    public WindowsDocumentStore(string? rootDirectory = null)
    {
        directory = rootDirectory ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            DefaultFolderName);
    }

    public string Directory => directory;

    public string PathFor(string documentName) => Path.Combine(directory, documentName);

    /// <summary>
    /// The document's text, or null when it does not exist or cannot be read.
    ///
    /// Never throws. Every caller of this store decodes totally — an unreadable
    /// document must cost the user that document's contents, never their ability
    /// to launch the app — so an exception here would only be re-swallowed one
    /// frame up.
    /// </summary>
    public string? Read(string documentName)
    {
        try
        {
            var path = PathFor(documentName);
            return File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>Returns false when the write could not be completed. The previous document survives.</summary>
    public bool Write(string documentName, string contents)
    {
        lock (gate)
        {
            try
            {
                System.IO.Directory.CreateDirectory(directory);
                var path = PathFor(documentName);
                var temporary = path + ".tmp";

                using (var stream = new FileStream(
                           temporary,
                           FileMode.Create,
                           FileAccess.Write,
                           FileShare.None))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                {
                    writer.Write(contents);
                    writer.Flush();
                    stream.Flush(flushToDisk: true);
                }

                // File.Move with overwrite is atomic on NTFS. File.Replace would
                // also work but requires the destination to already exist, which it
                // does not on a first write.
                File.Move(temporary, path, overwrite: true);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }

    public bool Delete(string documentName)
    {
        lock (gate)
        {
            try
            {
                var path = PathFor(documentName);
                if (File.Exists(path))
                {
                    File.Delete(path);
                }

                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
