using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The touch face-mapping conformance row from WINDOWS_PASS.md §26.2.
///
/// Reads <c>tools/fixtures/touch_face_mapping.csv</c> — the SAME file the Kotlin
/// suite reads, never a copy — and asserts that the ported profile catalog draws
/// the same letter on the same control and sends the same bridge usage.
///
/// This is the row that catches I13, the two OPPOSITE face mappers. A touch slot
/// sends the letter it DRAWS, while a physical key has to be interpreted against
/// the plastic the handheld prints; getting the direction wrong on one of them
/// inverts every face press and nothing else notices. The fixture pins both
/// presentations for Pro Controller 2 and the fixed arrangement for the other
/// three personalities.
/// </summary>
public sealed class TouchFaceMappingConformanceTests
{
    [Fact]
    public void EveryFaceControlDrawsAndSendsWhatTheFixtureSays()
    {
        var rows = RepositoryFixtures.ReadCsv(RepositoryFixtures.TouchFaceMapping);
        Assert.NotEmpty(rows);

        foreach (var row in rows)
        {
            Assert.Equal(8, row.Length);
            var key = string.Join('/', row[0], row[2], row[3]);

            var profileId = TouchProfileIds.FromKey(row[0]);
            Assert.True(profileId is not null, $"{key}: unknown profile");

            var profile = TouchProfileCatalog.Require(profileId!.Value);
            Assert.Equal(row[1], profile.DefaultTemplate.Id);

            var control = profile.DefaultTemplate.Controls
                .FirstOrDefault(entry => entry.Id == row[3]);
            Assert.True(control is not null, $"{key}: no such control");

            var action = profile.Bindings[control!.Output];
            var presentation = row[2] switch
            {
                "nintendo" => ControllerFaceLayout.Nintendo,
                "xbox" or "fixed" => ControllerFaceLayout.Xbox,
                _ => throw new InvalidDataException($"{key}: unknown presentation"),
            };

            var button = action switch
            {
                TouchControlAction.Face face =>
                    ControllerLayoutResolver.MapTouchFacePosition(
                        face.Position.Positional(), presentation),
                TouchControlAction.Logical logical => logical.Button,
                _ => throw new InvalidDataException($"{key}: does not bind to a button"),
            };

            var label = action is TouchControlAction.Face labelled
                ? ControllerLayoutResolver.FaceLabel(labelled.Position, presentation)
                : control.Visual.Role == TouchVisualRole.JoyConDirectionButton
                    ? DirectionGlyph(control.Output, key)
                    : control.Visual.Label;

            Assert.Equal(row[4], label);

            // The Android HID usage the adapter's descriptor declares, which is the
            // enum's ordinal plus one. Pinning the USAGE rather than the enum name is
            // what makes this a wire assertion instead of a naming one.
            Assert.Equal(int.Parse(row[5]), (int)button + 1);
        }
    }

    [Fact]
    public void TheFixtureCoversEveryFaceControlInEveryProfile()
    {
        // A conformance file that silently stopped covering a personality would pass
        // every row it still had. The expected key set is DERIVED from the catalog,
        // so adding a face control without a fixture row fails here.
        var expected = TouchProfileCatalog.Profiles.Values.SelectMany(profile =>
        {
            var outputs = FaceOutputs(profile.Id);
            var presentations = profile.Id == TouchProfileId.Pro2
                ? new[] { "nintendo", "xbox" }
                : ["fixed"];

            return profile.DefaultTemplate.Controls
                .Where(control => outputs.Contains(control.Output))
                .SelectMany(control => presentations.Select(presentation =>
                    string.Join('/', profile.Id.Key(), profile.DefaultTemplate.Id,
                                presentation, control.Id)));
        }).ToHashSet(StringComparer.Ordinal);

        var actual = RepositoryFixtures.ReadCsv(RepositoryFixtures.TouchFaceMapping)
            .Select(row => string.Join('/', row[0], row[1], row[2], row[3]))
            .ToHashSet(StringComparer.Ordinal);

        Assert.Equal(expected, actual);
    }

    private static HashSet<TouchOutputControl> FaceOutputs(TouchProfileId id) => id switch
    {
        TouchProfileId.Pro2 =>
        [
            TouchOutputControl.FaceSouth, TouchOutputControl.FaceEast,
            TouchOutputControl.FaceWest, TouchOutputControl.FaceNorth,
        ],
        TouchProfileId.JoyConLeft =>
        [
            TouchOutputControl.DirectionUp, TouchOutputControl.DirectionLeft,
            TouchOutputControl.DirectionRight, TouchOutputControl.DirectionDown,
        ],
        _ =>
        [
            TouchOutputControl.A, TouchOutputControl.B,
            TouchOutputControl.X, TouchOutputControl.Y,
        ],
    };

    /// <summary>
    /// The sideways Joy-Con (L) direction buttons are drawn as TRIANGLES, not
    /// letters: the markings are physical and turn with the shell.
    /// </summary>
    private static string DirectionGlyph(TouchOutputControl output, string key) => output switch
    {
        TouchOutputControl.DirectionUp => "triangle-up",
        TouchOutputControl.DirectionLeft => "triangle-left",
        TouchOutputControl.DirectionRight => "triangle-right",
        TouchOutputControl.DirectionDown => "triangle-down",
        _ => throw new InvalidDataException($"{key}: direction role without a direction output"),
    };
}

/// <summary>
/// The shipped layouts resolve and audit cleanly at every window shape the surface
/// is asked to fit.
///
/// The failures this catches are exactly the ones a screenshot does not show — a
/// hidden hit region overlapping its neighbour, a target below the size a thumb can
/// find, a control that drifted outside the safe rectangle — and they appear at
/// SHAPES rather than at one rendered size, which is why the sweep matters.
/// </summary>
public sealed class TouchShippedLayoutTests
{
    /// <summary>
    /// Representative interaction rectangles, in logical units, at the density scales
    /// a Windows tablet, convertible and handheld actually report.
    /// </summary>
    public static TheoryData<float, float, float> Shapes => new()
    {
        // The Kotlin suite's validated probe shapes, at the density scales a
        // Windows tablet, convertible and handheld report. Deliberately the SAME
        // shapes: they are the ones the shipped artwork was signed off against, and
        // inventing a different sweep here would test a different claim.
        { 915f, 412f, 1f },
        { 800f, 360f, 1f },
        { 832f, 440f, 1.5f },
        { 1024f, 768f, 2f },
        { 1280f, 800f, 2.75f },
        { 640f, 360f, 1f },      // deliberately below the minimum
    };

    [Theory]
    [MemberData(nameof(Shapes))]
    public void EveryShippedLayoutResolvesWithoutABlockingFinding(
        float widthUnits, float heightUnits, float unitScale)
    {
        var region = new TouchLayoutRegion(
            0f, 0f, widthUnits * unitScale, heightUnits * unitScale, unitScale);

        var tooSmall = widthUnits < TouchLayoutResolver.MinRegionWidthUnits ||
                       heightUnits < TouchLayoutResolver.MinRegionHeightUnits;

        foreach (var profile in TouchProfileCatalog.Profiles.Values)
        {
            var composed = TouchLayoutComposer.Compose(profile);
            var resolved = TouchLayoutResolver.Resolve(
                composed.Layout, region, TouchLayoutAuditMode.ShippedTemplate);

            if (tooSmall)
            {
                // Below the minimum the layout must REFUSE rather than draw
                // overlapping targets, and say so in one sentence.
                Assert.True(resolved.RegionTooSmall, $"{profile.Id}: expected a refusal");
                Assert.False(resolved.Fits);
                Assert.False(string.IsNullOrWhiteSpace(resolved.Problem));
                continue;
            }

            var blocking = resolved.Findings.Where(finding => finding.Blocking).ToList();
            Assert.True(
                blocking.Count == 0,
                $"{profile.Id} at {widthUnits}x{heightUnits}@{unitScale}: " +
                string.Join("; ", blocking.Select(finding => finding.Message)));
            Assert.True(resolved.Fits);
        }
    }

    /// <summary>
    /// A PRE-EXISTING defect in the shipped GameCube artwork, carried across from the
    /// Kotlin suite rather than silently "fixed" by the port.
    ///
    /// At aspect ratios near 2:1 the GameCube `c-stick` and the small `B` button
    /// genuinely overlap — their DRAWN circles, not merely their margins, by about
    /// four units at the worst point. It has nothing to do with the editor or the
    /// bean rotation; no shipped probe shape lands inside the band, so nothing ever
    /// ran the check there. The band is roughly 1.945 &lt; width/height &lt; 2.057,
    /// which includes 18:9 displays, and inside it the shipped GameCube controller
    /// refuses to draw.
    ///
    /// Fixing it means moving or shrinking approved GameCube artwork, which is not
    /// something a porting pass may decide. **This test reproducing the defect is
    /// also the strongest evidence the port is faithful**: the C# geometry lands in
    /// the same band, on the same two controls, as the Kotlin original.
    ///
    /// If this ever comes back empty the defect was fixed upstream — delete it.
    /// </summary>
    [Fact]
    public void TheKnownGameCubeCollisionNearTwoToOneIsReproducedExactly()
    {
        var profile = TouchProfileCatalog.Require(TouchProfileId.GameCube);

        var inside = Blocking(profile, 800f, 400f, 1f);
        Assert.Single(inside);
        Assert.Equal(
            ["b", "c-stick"],
            inside[0].ControlIds.OrderBy(id => id, StringComparer.Ordinal));

        // Just outside the band on either side, the shipped layout is clean.
        Assert.Empty(Blocking(profile, 800f, 420f, 1f));
        Assert.Empty(Blocking(profile, 800f, 380f, 1f));
    }

    private static IReadOnlyList<TouchLayoutFinding> Blocking(
        TouchControllerProfile profile, float width, float height, float density)
    {
        var region = new TouchLayoutRegion(0f, 0f, width * density, height * density, density);
        var composed = TouchLayoutComposer.Compose(profile);
        var resolved = TouchLayoutResolver.Resolve(
            composed.Layout, region, TouchLayoutAuditMode.ShippedTemplate);
        return resolved.Findings.Where(finding => finding.Blocking).ToList();
    }

    [Fact]
    public void TheOnlyMarginContactInTheShippedCatalogIsTheGameCubeZAndYBean()
    {
        // Recorded rather than merely tolerated. The GameCube `z` pad and the `Y`
        // bean have touching COURTESY MARGINS at the authored positions; the drawn
        // shapes clear each other. An Editor 2.0 pass moved the shoulder strip to
        // "fix" it, which broke a hardware-validated position. If this assertion ever
        // reports a different pair, the layout changed and somebody should look.
        var region = new TouchLayoutRegion(0f, 0f, 800f, 400f, 1f);

        foreach (var profile in TouchProfileCatalog.Profiles.Values)
        {
            var composed = TouchLayoutComposer.Compose(profile);
            var resolved = TouchLayoutResolver.Resolve(
                composed.Layout, region, TouchLayoutAuditMode.ShippedTemplate);

            var touching = resolved.Findings
                .Where(finding => finding.Message.Contains("touching hit margins",
                                                          StringComparison.Ordinal))
                .ToList();

            if (profile.Id == TouchProfileId.GameCube)
            {
                Assert.Single(touching);
                Assert.Equal(["y", "z"], touching[0].ControlIds.OrderBy(id => id, StringComparer.Ordinal));
            }
            else
            {
                Assert.Empty(touching);
            }
        }
    }

    [Fact]
    public void TheAuthoredDefaultPlacesEveryControlTheTemplateMarksAsShipped()
    {
        foreach (var profile in TouchProfileCatalog.Profiles.Values)
        {
            var document = TouchLayoutDocument.AuthoredDefault(profile);
            var expected = profile.DefaultTemplate.Controls
                .Where(control => control.InDefaultLayout)
                .Select(control => control.Id);

            Assert.Equal(expected, document.Controls.Select(instance => instance.InstanceId));

            // …and nothing else. Pro Controller 2's grips are in the CATALOG and not
            // in the shipped layout, which is the whole mechanism behind Add Control.
            Assert.DoesNotContain(document.Controls,
                instance => instance.InstanceId is TouchLayoutV1.GripLeft or TouchLayoutV1.GripRight);
        }
    }

    [Fact]
    public void ProControllerTwoOffersTheGripsFromItsCatalogWithoutPlacingThem()
    {
        var profile = TouchProfileCatalog.Require(TouchProfileId.Pro2);

        foreach (var id in new[] { TouchLayoutV1.GripLeft, TouchLayoutV1.GripRight })
        {
            var entry = profile.CatalogEntry(id);
            Assert.NotNull(entry);
            Assert.False(entry!.InDefaultLayout);
            Assert.Equal(TouchControlCategory.Grip, entry.Category);
            Assert.Contains(entry.Output, profile.Outputs);
        }
    }
}

/// <summary>Composition, validation and the schema-1 migration.</summary>
public sealed class TouchCompositionTests
{
    private static TouchControllerProfile Pro2 =>
        TouchProfileCatalog.Require(TouchProfileId.Pro2);

    [Fact]
    public void ComposingWithNoDocumentGivesTheShippedArrangement()
    {
        var composed = TouchLayoutComposer.Compose(Pro2);

        Assert.False(composed.Customized);
        Assert.Null(composed.Warning);
        Assert.False(composed.Degraded);
        Assert.Equal(TouchProfileId.Pro2, composed.Layout.ProfileId);
    }

    [Fact]
    public void ADocumentForAnotherControllerIsRefusedRatherThanBestEfforted()
    {
        var foreign = TouchLayoutDocument.AuthoredDefault(
            TouchProfileCatalog.Require(TouchProfileId.GameCube));

        var composed = TouchLayoutComposer.Compose(Pro2, foreign);

        Assert.Equal("Stored layout belongs to another controller", composed.Warning);
        Assert.False(composed.Customized);
    }

    [Fact]
    public void ADocumentFromANewerTemplateRevisionIsRefused()
    {
        // It was written against controls this build may not have, and quietly
        // dropping them would present a layout missing pieces without saying so.
        var document = TouchLayoutDocument.AuthoredDefault(Pro2) with
        {
            BasedOnRevision = TouchLayoutV1.TemplateRevision + 1,
        };

        var composed = TouchLayoutComposer.Compose(Pro2, document);

        Assert.Equal("Stored layout was written for a newer template revision", composed.Warning);
    }

    [Fact]
    public void OneUnknownCatalogEntryCostsOneInstanceAndNotTheLayout()
    {
        var authored = TouchLayoutDocument.AuthoredDefault(Pro2);
        var document = authored with
        {
            Controls =
            [
                .. authored.Controls,
                new TouchControlInstance
                {
                    InstanceId = "ghost",
                    CatalogId = "no-such-control",
                    AnchorX = 0.5f,
                    AnchorY = 0.5f,
                },
            ],
        };

        var composed = TouchLayoutComposer.Compose(Pro2, document);

        Assert.True(composed.Degraded);
        Assert.DoesNotContain(composed.Layout.Controls, control => control.Id == "ghost");
        Assert.Equal(authored.Controls.Count, composed.Layout.Controls.Count);
    }

    [Fact]
    public void AnImpossiblePositionIsDroppedAndAMerelyOutOfRangeOneIsClamped()
    {
        // The distinction is deliberate: a document written by a build with different
        // limits still describes something the user made, while NaN has no defensible
        // position to clamp to and drawing at it takes the whole layout down.
        var authored = TouchLayoutDocument.AuthoredDefault(Pro2);
        var document = authored with
        {
            Controls =
            [
                authored.Controls[0] with { AnchorX = float.NaN },
                authored.Controls[1] with { Scale = 99f },
            ],
        };

        var validation = TouchLayoutDocumentValidator.Validate(document, Pro2);

        Assert.True(validation.Degraded);
        Assert.Single(validation.Document.Controls);
        Assert.Equal(TouchLayoutLimits.MaxScale, validation.Document.Controls[0].Scale);
    }

    [Fact]
    public void AStoredLatchOnAControlThatCannotHoldIsDiscarded()
    {
        // Otherwise it is a setting the editor never shows and nothing ever reads.
        var authored = TouchLayoutDocument.AuthoredDefault(Pro2);
        var stick = authored.Controls.First(instance => instance.CatalogId == TouchLayoutV1.StickLeft);
        var document = authored with { Controls = [stick with { Latch = true }] };

        var validation = TouchLayoutDocumentValidator.Validate(document, Pro2);

        Assert.Null(validation.Document.Controls[0].Latch);
    }

    [Fact]
    public void AUserRotationAddsToTheAuthoredArtDirectionRatherThanReplacingIt()
    {
        var profile = TouchProfileCatalog.Require(TouchProfileId.GameCube);
        var authored = TouchLayoutDocument.AuthoredDefault(profile);
        var bean = authored.Controls.First(instance => instance.CatalogId == "x");
        var entry = profile.CatalogEntry("x")!;

        var composed = TouchLayoutComposer.Compose(profile, authored with
        {
            Controls = [bean with { RotationDegrees = 20f }],
        });

        var spec = composed.Layout.Controls.Single();
        Assert.Equal(entry.Visual.RotationDegrees + 20f, spec.VisualRotationDegrees, 3);

        // And the authored angle survives separately, so "reset orientation" means
        // the art direction rather than a blind zero.
        Assert.Equal(entry.Visual.RotationDegrees, spec.AuthoredRotationDegrees, 3);
    }
}

/// <summary>The retired schema-1 override document, and the upgrade off it.</summary>
public sealed class TouchLayoutOverrideCodecTests
{
    private static TouchControllerProfile Pro2 =>
        TouchProfileCatalog.Require(TouchProfileId.Pro2);

    private static TouchLayoutOverride Sample => new()
    {
        ProfileId = TouchProfileId.Pro2,
        TemplateId = TouchLayoutV1.Id,
        BasedOnRevision = TouchLayoutV1.TemplateRevision,
        Controls = new Dictionary<string, TouchControlOverride>(StringComparer.Ordinal)
        {
            [TouchLayoutV1.Dpad] = new() { AnchorX = 0.25f, AnchorY = 0.7f, Scale = 1.2f },
            [TouchLayoutV1.Chat] = new() { Visible = false },
            [TouchLayoutV1.Home] = new() { Latch = true },
        },
    };

    [Fact]
    public void ARoundTripThroughTheCodecIsLossless()
    {
        // The encoder exists ONLY to pin the decoder. A round trip is the single
        // check that this reader still accepts exactly what earlier builds wrote, and
        // losing it would mean discovering on somebody's device that an upgrade had
        // thrown their layout away.
        var decoded = TouchLayoutOverrideJsonCodec.Decode(
            TouchLayoutOverrideJsonCodec.Encode(Sample));

        var valid = Assert.IsType<TouchOverrideDecodeResult.Valid>(decoded);
        Assert.Equal(Sample.ProfileId, valid.Value.ProfileId);
        Assert.Equal(Sample.TemplateId, valid.Value.TemplateId);
        Assert.Equal(Sample.BasedOnRevision, valid.Value.BasedOnRevision);
        Assert.Equal(Sample.Controls.Count, valid.Value.Controls.Count);
        Assert.Equal(0.25f, valid.Value.Controls[TouchLayoutV1.Dpad].AnchorX);
        Assert.False(valid.Value.Controls[TouchLayoutV1.Chat].Visible);
        Assert.True(valid.Value.Controls[TouchLayoutV1.Home].Latch);
    }

    [Theory]
    [InlineData("not json at all", "not valid JSON")]
    [InlineData("""{"profileId":"pro2"}""", "no schema version")]
    [InlineData("""{"schemaVersion":99,"profileId":"pro2"}""", "newer app")]
    [InlineData("""{"schemaVersion":1,"profileId":"nope"}""", "unknown profile")]
    public void ADamagedDocumentIsReportedAndNeverGuessedAt(string raw, string expected)
    {
        var invalid = Assert.IsType<TouchOverrideDecodeResult.Invalid>(
            TouchLayoutOverrideJsonCodec.Decode(raw));

        Assert.Contains(expected, invalid.Problem, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AnOutOfRangeStoredValueIsRefusedRatherThanClamped()
    {
        // The codec is the same gate an editor operation passes, so a hand-edited
        // file cannot be a way to construct geometry the editor refuses to make.
        var raw = """
            {"schemaVersion":1,"profileId":"pro2","templateId":"picoswitch.touch.v1",
             "basedOnRevision":2,"controls":{"dpad":{"scale":9.0}}}
            """;

        var invalid = Assert.IsType<TouchOverrideDecodeResult.Invalid>(
            TouchLayoutOverrideJsonCodec.Decode(raw));

        Assert.Contains("out-of-range scale", invalid.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void APresentButUnreadableFieldIsNotTreatedAsAbsent()
    {
        // Absent means "this control has no opinion"; a key holding a string means the
        // document is damaged. Collapsing the second into the first would import
        // geometry nobody wrote.
        var raw = """
            {"schemaVersion":1,"profileId":"pro2","templateId":"picoswitch.touch.v1",
             "basedOnRevision":2,"controls":{"dpad":{"anchorX":"left"}}}
            """;

        var invalid = Assert.IsType<TouchOverrideDecodeResult.Invalid>(
            TouchLayoutOverrideJsonCodec.Decode(raw));

        Assert.Contains("invalid anchorX", invalid.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void MigrationTurnsAHiddenControlIntoNoInstanceAtAll()
    {
        var document = TouchLayoutMigration.FromOverride(Pro2, Sample);

        Assert.DoesNotContain(document.Controls,
            instance => instance.InstanceId == TouchLayoutV1.Chat);
        Assert.Equal(TouchLayoutDocument.CurrentSchemaVersion, document.SchemaVersion);

        var dpad = document.Controls.First(instance => instance.InstanceId == TouchLayoutV1.Dpad);
        Assert.Equal(0.25f, dpad.AnchorX);
        Assert.Equal(1.2f, dpad.Scale);

        var home = document.Controls.First(instance => instance.InstanceId == TouchLayoutV1.Home);
        Assert.True(home.Latch);
    }

    [Fact]
    public void MigrationIsDeterministic()
    {
        // What makes the golden migration fixtures worth having: the same template and
        // the same stored override always produce the same document, including ids.
        var first = TouchLayoutMigration.FromOverride(Pro2, Sample);
        var second = TouchLayoutMigration.FromOverride(Pro2, Sample);

        Assert.Equal(
            first.Controls.Select(instance => instance.InstanceId),
            second.Controls.Select(instance => instance.InstanceId));
        Assert.Equal(first, second);
    }

    [Fact]
    public void AnOverrideForAnotherTemplateFallsBackToTheAuthoredDefault()
    {
        var document = TouchLayoutMigration.FromOverride(
            Pro2, Sample with { TemplateId = "somebody-elses-template" });

        Assert.Equal(TouchLayoutDocument.AuthoredDefault(Pro2), document);
    }
}
