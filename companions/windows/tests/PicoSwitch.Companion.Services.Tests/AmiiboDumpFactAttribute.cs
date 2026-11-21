using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// A test that reads genuine amiibo dumps from <c>tests/fixtures/amiibo/</c>.
/// </summary>
/// <remarks>
/// The dumps were removed from the repository on 2026-11-11 and the directory is
/// git-ignored: a tag image is a copy of a physical figure, and a set-up one
/// carries its owner's names. A maintainer who has dumps of their own puts them
/// back in that directory (see <c>tests/README.md</c>) and these tests run
/// again. Without them the test is SKIPPED, visibly, rather than passed.
/// </remarks>
public sealed class AmiiboDumpFactAttribute : FactAttribute
{
    private const string Marker = "tests/fixtures/amiibo/ntag215-animal-crossing-tom-nook.bin";

    // The exchange archive is built from the same dumps (AmiiboCryptoFixtureTool,
    // -Damiibo.regenerate=true), so it left with them.
    private const string Archive = "contracts/amiibo/library-archive-v3.zip";

    public AmiiboDumpFactAttribute()
    {
        if (!Present)
        {
            Skip = "needs the local amiibo dumps in tests/fixtures/amiibo/ and contracts/amiibo/library-archive-v3.zip (not in the repository)";
        }
    }

    private static bool Present { get; } = Find();

    private static bool Find()
    {
        for (var cursor = new DirectoryInfo(AppContext.BaseDirectory); cursor is not null; cursor = cursor.Parent)
        {
            if (File.Exists(Path.Combine(cursor.FullName, Marker)) &&
                File.Exists(Path.Combine(cursor.FullName, Archive)))
            {
                return true;
            }
        }

        return false;
    }
}
