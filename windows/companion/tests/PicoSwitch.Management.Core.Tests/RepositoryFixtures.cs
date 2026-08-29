namespace PicoSwitch.Management.Tests;

/// <summary>
/// Locates the repository's shared fixtures by walking up from the test binary.
///
/// The fixtures are NOT copied into the test project. A copy is a second
/// authority, and the whole point of <c>tools/fixtures/</c> is that C, Kotlin and
/// C# read the SAME file — a one-sided edit has to fail somewhere, and it cannot
/// if each language keeps its own snapshot.
/// </summary>
public static class RepositoryFixtures
{
    public static string Path(string relativePath)
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var candidate = System.IO.Path.Combine(cursor.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            cursor = cursor.Parent;
        }

        throw new FileNotFoundException(
            $"Cannot find {relativePath} above {AppContext.BaseDirectory}");
    }

    public static string ReadText(string relativePath) => File.ReadAllText(Path(relativePath));

    public const string ManagementProtocolFixture = "tools/fixtures/management/protocol-v1.json";
    public const string ControllerLinkFaceMapping = "tools/fixtures/controller_link_face_mapping.csv";
    public const string BridgeReportGoldens = "tools/fixtures/bridge_report_goldens.csv";
    public const string BridgeDescriptorHeader = "tools/fixtures/android_controller_hid.h";
}
