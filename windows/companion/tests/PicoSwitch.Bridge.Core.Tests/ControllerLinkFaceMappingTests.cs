using PicoSwitch.Bridge.Core;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// Controller Link: what a press on the host's physical pad sends to the console.
///
/// Regression, 2026-08-24. The Touch Gamepad face correction moved the firmware's
/// bridge face usages onto their logical A/B/X/Y destinations, which is right for
/// the on-screen pad — but the physical path still forwarded its key codes in the
/// source device's own dialect, so every Controller Link face press came out
/// inverted on console under both layouts. Nothing failed: the two origins shared
/// one mapper and only the on-screen half had a cross-layer golden.
///
/// This reads <c>tools/fixtures/controller_link_face_mapping.csv</c> — the same
/// file the Kotlin <c>ControllerLinkFaceMappingTest</c> and the C
/// <c>tools/test_controller_link_face_goldens.c</c> consume, so the
/// console-facing expectation exists in exactly one place.
///
/// The platform key column names the button IN THE SOURCE DEVICE'S OWN DIALECT.
/// A Windows backend converts a <c>GamepadButtons.A</c> or a raw HID usage into
/// that dialect exactly as the Android backend converts a
/// <c>KEYCODE_BUTTON_A</c>; the mapping under test is what happens after that.
/// </summary>
public sealed class ControllerLinkFaceMappingTests
{
    [Fact]
    public void EveryFixtureRowMapsItsPlatformKeyToTheLogicalButtonItClaims()
    {
        var rows = FixtureRows();
        Assert.Equal(8, rows.Count);

        foreach (var row in rows)
        {
            var state = new ControllerInputState();
            state.SetRequestedLayout(row.Layout);
            state.PressButton(row.Reported, true);

            Assert.Equal(
                ControllerButtonSet.Of(row.LogicalButton),
                state.State.Value.Buttons);

            // The usage the firmware reads is the enum value plus one; the fixture
            // carries it so the C side and this side cannot drift apart on it.
            Assert.Equal(row.Usage, (int)row.LogicalButton + 1);
        }
    }

    /// <summary>
    /// The property behind the table: on both source families the button you press
    /// reaches the console face button in the SAME PLACE. A Nintendo-labelled
    /// handheld reports its printed legend and needs no correction; a positional
    /// pad names its bottom button <c>A</c> while the console's bottom button is B.
    /// </summary>
    [Fact]
    public void BothLayoutsPreserveThePhysicalPositionOfThePress()
    {
        var rows = FixtureRows();
        var nintendo = rows
            .Where(row => row.Layout == ControllerFaceLayout.Nintendo)
            .ToDictionary(row => row.Reported, row => row.LogicalButton);
        var xbox = rows
            .Where(row => row.Layout == ControllerFaceLayout.Xbox)
            .ToDictionary(row => row.Reported, row => row.LogicalButton);

        Assert.Equal(ControllerButton.A, nintendo[ControllerButton.A]);
        Assert.Equal(ControllerButton.B, xbox[ControllerButton.A]);

        foreach (var key in new[]
                 {
                     ControllerButton.A, ControllerButton.B, ControllerButton.X, ControllerButton.Y,
                 })
        {
            Assert.True(
                nintendo[key] != xbox[key],
                $"key {key} must not resolve alike under both layouts");
        }
    }

    /// <summary>
    /// I13: the physical and on-screen mappers can NEVER resolve alike for a face
    /// button.
    ///
    /// They are opposite functions of the same layout, and collapsing them into one
    /// mapper broke both origins in turn. Asserting the property, rather than the
    /// current implementation, is what stops a future "simplification" from
    /// reintroducing the shared mapper.
    /// </summary>
    [Fact]
    public void ThePhysicalAndTouchMappersCanNeverResolveAlikeForAFaceButton()
    {
        ControllerButton[] faces =
        [
            ControllerButton.A, ControllerButton.B, ControllerButton.X, ControllerButton.Y,
        ];

        foreach (var layout in new[] { ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox })
        {
            foreach (var face in faces)
            {
                Assert.True(
                    ControllerLayoutResolver.MapPhysicalFaceKey(face, layout) !=
                    ControllerLayoutResolver.MapTouchFacePosition(face, layout),
                    $"{face} under {layout} resolved alike through both mappers");
            }
        }
    }

    /// <summary>Non-face controls mean the same thing on every source.</summary>
    [Fact]
    public void ShouldersAndMenuKeysAreNeverFaceSwapped()
    {
        ControllerButton[] unchanged =
        [
            ControllerButton.L1, ControllerButton.R1, ControllerButton.L2, ControllerButton.R2,
            ControllerButton.Select, ControllerButton.Start,
            ControllerButton.LeftStick, ControllerButton.RightStick,
            ControllerButton.Home, ControllerButton.Capture, ControllerButton.C,
            ControllerButton.GL, ControllerButton.GR,
        ];

        foreach (var layout in ControllerFaceLayouts.All)
        {
            foreach (var button in unchanged)
            {
                var state = new ControllerInputState();
                state.SetRequestedLayout(layout);
                state.PressButton(button, true);
                Assert.Equal(ControllerButtonSet.Of(button), state.State.Value.Buttons);
            }
        }
    }

    /// <summary>
    /// The drawn legend is derived from the mapper, never from a second table.
    ///
    /// A label that disagrees with the bit that gets sent is invisible until
    /// someone presses the button on a console.
    /// </summary>
    [Fact]
    public void TheDrawnFaceLabelAlwaysMatchesTheBitThatGetsSent()
    {
        foreach (var layout in new[] { ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox })
        {
            foreach (var position in Enum.GetValues<FaceButtonPosition>())
            {
                var sent = ControllerLayoutResolver.MapTouchFacePosition(
                    position.Positional(),
                    layout);
                Assert.Equal(sent.ToString(), ControllerLayoutResolver.FaceLabel(position, layout));
            }
        }

        // The concrete claim, so a silent inversion of the whole table still fails:
        // under a Nintendo presentation the south slot is drawn B.
        Assert.Equal("B", ControllerLayoutResolver.FaceLabel(
            FaceButtonPosition.South, ControllerFaceLayout.Nintendo));
        Assert.Equal("A", ControllerLayoutResolver.FaceLabel(
            FaceButtonPosition.South, ControllerFaceLayout.Xbox));
    }

    private static IReadOnlyList<Row> FixtureRows() =>
        RepositoryFixtures.ReadCsv(RepositoryFixtures.ControllerLinkFaceMapping)
            .Select(fields =>
            {
                Assert.Equal(6, fields.Length);
                return new Row(
                    ControllerFaceLayouts.FromKey(fields[0]),
                    fields[1],
                    ReportedFor(fields[1]),
                    Enum.Parse<ControllerButton>(fields[2]),
                    int.Parse(fields[3]));
            })
            .ToList();

    /// <summary>
    /// The fixture's platform-key column, in bridge vocabulary.
    ///
    /// <c>BUTTON_A</c> is "whatever the source device calls its A button", which is
    /// precisely what <see cref="ControllerInputState.PressButton"/> takes.
    /// </summary>
    private static ControllerButton ReportedFor(string platformKey) => platformKey switch
    {
        "BUTTON_A" => ControllerButton.A,
        "BUTTON_B" => ControllerButton.B,
        "BUTTON_X" => ControllerButton.X,
        "BUTTON_Y" => ControllerButton.Y,
        _ => throw new Xunit.Sdk.XunitException(
            $"Fixture names an unknown platform key: {platformKey}"),
    };

    private sealed record Row(
        ControllerFaceLayout Layout,
        string PlatformKey,
        ControllerButton Reported,
        ControllerButton LogicalButton,
        int Usage);
}
