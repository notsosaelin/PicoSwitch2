using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// Encoder goldens, as DATA shared with the Kotlin implementation.
///
/// The descriptor is guarded in three languages, and each side has its own
/// encoder tests — but the ENCODER OUTPUT needs cross-checking too. Two
/// implementations can agree on all 161 descriptor bytes and still disagree about
/// which bit GR sets, where the hat byte moved to in contract 4, or whether a
/// battery level is clamped before or after the valid flag is decided. None of
/// that is visible until a console misbehaves.
///
/// <c>tools/fixtures/bridge_report_goldens.csv</c> was generated from the Kotlin
/// encoder and is read by both. WINDOWS_PASS.md §26.2 proposed it; this is the
/// C# half.
/// </summary>
public sealed class BridgeReportGoldenTests
{
    [Fact]
    public void EveryGoldenVectorEncodesToExactlyItsRecordedBytes()
    {
        var rows = GoldenRows();
        Assert.True(rows.Count >= 30, "fixture must carry vectors");

        foreach (var row in rows)
        {
            var state = row.State();
            Assert.Equal(row.V1, Hex(ControllerReportEncoder.EncodeV1(state)));
            Assert.Equal(row.V2, Hex(ControllerReportEncoder.Encode(state)));
        }
    }

    [Fact]
    public void TheGoldensPinTheWirePayloadLengthsOfBothReportVersions()
    {
        foreach (var row in GoldenRows())
        {
            Assert.Equal(ControllerReportEncoder.PayloadSize * 2, row.V1.Length);
            Assert.Equal(ControllerReportEncoder.PayloadSizeV2 * 2, row.V2.Length);
        }
    }

    [Fact]
    public void TheGoldensCoverEveryLogicalButtonEveryHatDirectionAndBothFlagHalves()
    {
        // A golden file that happens to omit a button is a golden file that cannot
        // catch that button moving.
        var rows = GoldenRows();
        foreach (var button in Enum.GetValues<ControllerButton>())
        {
            Assert.Contains(
                rows,
                row => row.Buttons.Count == 1 && row.Buttons.Contains(button));
        }

        for (var hat = 0; hat <= 8; hat++)
        {
            var expected = hat;
            Assert.Contains(rows, row => ControllerReportEncoder.Hat(row.State()) == expected);
        }

        Assert.Contains(rows, row => row.State().Motion.Valid);
        Assert.Contains(rows, row => row.State().Battery is { Valid: true, Charging: true });
    }

    /// <summary>
    /// The v1 report is NOT a prefix of the v2 report, and the goldens say so.
    ///
    /// It was one through contract 3. Contract 4 needed a third button byte for
    /// GL/GR, which moved the hat and everything after it by one. Anyone reading
    /// the two encoders side by side would reasonably assume the old relationship
    /// still holds, and this is where they find out it does not.
    /// </summary>
    [Fact]
    public void V1IsNoLongerAPrefixOfV2()
    {
        var divergent = GoldenRows()
            .Where(row => !row.V2.StartsWith(row.V1, StringComparison.Ordinal))
            .ToList();
        Assert.NotEmpty(divergent);

        // The first six bytes -- the axes -- are still shared by both layouts.
        foreach (var row in GoldenRows())
        {
            Assert.Equal(row.V1[..12], row.V2[..12]);
        }
    }

    private static string Hex(byte[] bytes) => Convert.ToHexString(bytes);

    private static IReadOnlyList<GoldenRow> GoldenRows() =>
        RepositoryFixtures.ReadCsv(RepositoryFixtures.BridgeReportGoldens)
            .Select(fields =>
            {
                Assert.Equal(13, fields.Length);
                return new GoldenRow(
                    fields[0],
                    int.Parse(fields[1]), int.Parse(fields[2]),
                    int.Parse(fields[3]), int.Parse(fields[4]),
                    int.Parse(fields[5]), int.Parse(fields[6]),
                    fields[7], fields[8], fields[9], fields[10],
                    fields[11], fields[12]);
            })
            .ToList();

    private sealed record GoldenRow(
        string Name,
        int LeftX,
        int LeftY,
        int RightX,
        int RightY,
        int LeftTrigger,
        int RightTrigger,
        string ButtonsField,
        string DpadField,
        string MotionField,
        string BatteryField,
        string V1,
        string V2)
    {
        public ControllerButtonSet Buttons => ButtonsField == "-"
            ? ControllerButtonSet.Empty
            : ControllerButtonSet.Of(ButtonsField.Split('|').Select(Enum.Parse<ControllerButton>));

        public ControllerState State() => new()
        {
            LeftX = LeftX,
            LeftY = LeftY,
            RightX = RightX,
            RightY = RightY,
            LeftTrigger = LeftTrigger,
            RightTrigger = RightTrigger,
            Buttons = Buttons,
            DpadUp = DpadField.Contains('U'),
            DpadRight = DpadField.Contains('R'),
            DpadDown = DpadField.Contains('D'),
            DpadLeft = DpadField.Contains('L'),
            Motion = ParseMotion(MotionField),
            Battery = ParseBattery(BatteryField),
        };

        private static ControllerMotion ParseMotion(string field)
        {
            if (field == "-")
            {
                return ControllerMotion.None;
            }

            var valid = !field.StartsWith('!');
            var parts = field.TrimStart('!').Split(':').Select(int.Parse).ToArray();
            Assert.Equal(7, parts.Length);
            return new ControllerMotion(
                parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], valid);
        }

        private static ControllerBattery ParseBattery(string field)
        {
            if (field == "-")
            {
                return ControllerBattery.Unknown;
            }

            var valid = !field.StartsWith('!');
            var parts = field.TrimStart('!').Split(':').Select(int.Parse).ToArray();
            Assert.Equal(2, parts.Length);
            return new ControllerBattery(parts[0], parts[1] != 0, valid);
        }
    }
}
