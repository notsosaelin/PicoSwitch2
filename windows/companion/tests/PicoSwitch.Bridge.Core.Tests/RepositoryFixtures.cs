using System.Text.RegularExpressions;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// Locates and reads the repository's shared fixtures.
///
/// The fixtures are NOT copied into the test project. A copy is a second
/// authority, and the whole point of <c>tools/fixtures/</c> is that C, Kotlin and
/// C# read the SAME file — a one-sided edit has to fail somewhere, and it cannot
/// if each language keeps its own snapshot.
/// </summary>
public static partial class RepositoryFixtures
{
    public const string BridgeDescriptorHeader = "tools/fixtures/android_controller_hid.h";
    public const string ControllerLinkFaceMapping = "tools/fixtures/controller_link_face_mapping.csv";
    public const string BridgeReportGoldens = "tools/fixtures/bridge_report_goldens.csv";
    public const string TouchFaceMapping = "tools/fixtures/touch_face_mapping.csv";

    public static string RepositoryRoot { get; } = FindRoot();

    public static string Path(string relativePath) =>
        System.IO.Path.Combine(RepositoryRoot, relativePath.Replace('/', System.IO.Path.DirectorySeparatorChar));

    public static string ReadText(string relativePath) => File.ReadAllText(Path(relativePath));

    /// <summary>
    /// Rows of a repository CSV fixture, with <c>#</c> comments and blank lines
    /// removed — the same filtering the Kotlin and C consumers apply.
    /// </summary>
    public static IReadOnlyList<string[]> ReadCsv(string relativePath) =>
        File.ReadAllLines(Path(relativePath))
            .Select(line => line.Trim())
            .Where(line => line.Length > 0 && !line.StartsWith('#'))
            .Select(line => line.Split(','))
            .ToList();

    /// <summary>
    /// The descriptor bytes as the C fixture declares them.
    ///
    /// Parsed rather than copied, and parsed the same way
    /// <c>tools/check_android_descriptor_parity.py</c> does it: strip comments,
    /// take the initialiser body, substitute the two report-id macros the C source
    /// references symbolically, then read every hex literal in order.
    /// </summary>
    public static byte[] CDescriptorBytes()
    {
        var text = StripComments(ReadText(BridgeDescriptorHeader));
        var body = ExtractInitializer(text, "ANDROID_CONTROLLER_V2_HID_DESCRIPTOR");
        body = body
            .Replace("ANDROID_CONTROLLER_REPORT_ID", "0x01")
            .Replace("ANDROID_CONTROLLER_OUTPUT_REPORT_ID", "0x02");
        return HexLiteral().Matches(body)
            .Select(match => Convert.ToByte(match.Value, 16))
            .ToArray();
    }

    /// <summary>The contract version the C fixture declares, which is the bump authority.</summary>
    public static int CContractVersion()
    {
        var match = ContractDefine().Match(ReadText(BridgeDescriptorHeader));
        if (!match.Success)
        {
            throw new InvalidDataException(
                "ANDROID_BRIDGE_CONTRACT_VERSION not found in the C fixture");
        }

        return int.Parse(match.Groups[1].Value);
    }

    private static string StripComments(string text)
    {
        text = BlockComment().Replace(text, string.Empty);
        return LineComment().Replace(text, string.Empty);
    }

    private static string ExtractInitializer(string text, string symbol)
    {
        var match = Regex.Match(text, Regex.Escape(symbol) + @"\s*\[\s*\]\s*=");
        if (!match.Success)
        {
            throw new InvalidDataException($"could not locate {symbol}");
        }

        var open = text.IndexOf('{', match.Index + match.Length);
        var depth = 0;
        for (var index = open; index < text.Length; index++)
        {
            if (text[index] == '{')
            {
                depth++;
            }
            else if (text[index] == '}')
            {
                depth--;
                if (depth == 0)
                {
                    return text[(open + 1)..index];
                }
            }
        }

        throw new InvalidDataException($"unterminated initializer for {symbol}");
    }

    private static string FindRoot()
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            if (File.Exists(System.IO.Path.Combine(cursor.FullName, BridgeDescriptorHeader
                    .Replace('/', System.IO.Path.DirectorySeparatorChar))))
            {
                return cursor.FullName;
            }

            cursor = cursor.Parent;
        }

        throw new DirectoryNotFoundException(
            $"Cannot find {BridgeDescriptorHeader} above {AppContext.BaseDirectory}");
    }

    [GeneratedRegex(@"0[xX][0-9a-fA-F]{1,2}")]
    private static partial Regex HexLiteral();

    [GeneratedRegex(@"#define\s+ANDROID_BRIDGE_CONTRACT_VERSION\s+(\d+)")]
    private static partial Regex ContractDefine();

    [GeneratedRegex(@"/\*.*?\*/", RegexOptions.Singleline)]
    private static partial Regex BlockComment();

    [GeneratedRegex(@"//[^\n]*")]
    private static partial Regex LineComment();
}
