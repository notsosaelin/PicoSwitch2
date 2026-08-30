using System.Text.Json;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The content fingerprint, against vectors produced by the FIRMWARE'S OWN
/// function.
/// </summary>
/// <remarks>
/// Windows and Android keep separate local libraries with separate ids, so the
/// only way either can tell whether the adapter's resident copy of a profile is
/// still the one it holds is to compare CONTENT. Three implementations of that
/// digest have to agree exactly, and agreeing with each other is not enough —
/// they must agree with the adapter.
///
/// tools/test_ns2_kbm_commands.c emits these from
/// <c>ns2_kbm_content_fingerprint()</c> into the shared corpus. This replays
/// them, so a divergence fails here rather than presenting a user with a profile
/// that is permanently, wrongly, "out of date".
/// </remarks>
public sealed class KbmFingerprintTests
{
    private static JsonElement Vectors()
    {
        var text = RepositoryFixtures.ReadText(RepositoryFixtures.KbmWireCorpus);
        var document = JsonDocument.Parse(text);
        return document.RootElement.GetProperty("fingerprints").Clone();
    }

    [Fact]
    public void EveryFirmwareVectorReproducesExactly()
    {
        var vectors = Vectors();
        var count = 0;

        foreach (var vector in vectors.EnumerateArray())
        {
            var label = vector.GetProperty("label").GetString()!;
            var layout = KbmLayouts.FromWire(vector.GetProperty("layout").GetString());
            Assert.NotNull(layout);

            var overrides = new List<KbmBinding>();
            foreach (var entry in vector.GetProperty("overrides").EnumerateArray())
            {
                var source = KbmSource.Parse(entry.GetProperty("src").GetString()!);
                var destination =
                    KbmDestinations.FromWire(entry.GetProperty("dst").GetString());
                Assert.NotNull(source);
                Assert.NotNull(destination);
                overrides.Add(new KbmBinding(source!, destination!.Value, Custom: true));
            }

            var m = vector.GetProperty("mouse");
            var mouse = new KbmMouseConfig(
                SensitivityX: m.GetProperty("sensitivityX").GetInt32(),
                SensitivityY: m.GetProperty("sensitivityY").GetInt32(),
                VelocityWindowMs: m.GetProperty("velocityWindowMs").GetInt32(),
                InvertX: m.GetProperty("invertX").GetBoolean(),
                InvertY: m.GetProperty("invertY").GetBoolean(),
                AntiDeadzone: m.GetProperty("antiDeadzone").GetInt32());

            var expected = vector.GetProperty("fingerprint").GetInt64();
            var actual = KbmFingerprint.Compute(layout!.Value, overrides, mouse);

            Assert.True(expected == actual,
                        $"vector '{label}': firmware says {expected}, this build says {actual}");
            count++;
        }

        // The corpus must actually carry vectors; an empty loop would pass
        // silently and prove nothing.
        Assert.True(count >= 6, $"expected the firmware's vectors, found {count}");
    }

    [Fact]
    public void MouseTuningMovesTheDigestJustAsARebindDoes()
    {
        // Profile-owned settings are part of the mapping's behaviour. If they did
        // not contribute, a user who changed only sensitivity would be told the
        // adapter's copy was up to date when it was not.
        var mouse = new KbmMouseConfig(SensitivityX: 512, SensitivityY: 512);
        var baseline = KbmFingerprint.Compute(KbmLayout.Keyboard, [], mouse);
        var faster = KbmFingerprint.Compute(KbmLayout.Keyboard, [],
                                            mouse with { SensitivityX = 513 });

        Assert.NotEqual(baseline, faster);
    }

    [Fact]
    public void TheLayoutIsPartOfTheDigest()
    {
        // The same overrides mean different things in different layouts, so two
        // banks' profiles must never collide.
        var mouse = new KbmMouseConfig();
        Assert.NotEqual(KbmFingerprint.Compute(KbmLayout.Keyboard, [], mouse),
                        KbmFingerprint.Compute(KbmLayout.KeyboardMouse, [], mouse));
    }

    [Fact]
    public void CanonicalOrderIsIndependentOfInsertionOrder()
    {
        // Two profiles that behave identically must fingerprint identically
        // however they were built, or "out of date" would fire on a no-op edit.
        var a = new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x20),
                               KbmDestination.A, Custom: true);
        var b = new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x05),
                               KbmDestination.B, Custom: true);
        var mouse = new KbmMouseConfig();

        var forwards = KbmFingerprint.Compute(
            KbmLayout.Keyboard, KbmFingerprint.Canonical([a, b]), mouse);
        var backwards = KbmFingerprint.Compute(
            KbmLayout.Keyboard, KbmFingerprint.Canonical([b, a]), mouse);

        Assert.Equal(forwards, backwards);
    }

    [Fact]
    public void OnlyUserOverridesContribute()
    {
        // The firmware stores content sparsely against a canonical table, so a
        // binding the user never changed is not part of the profile at all.
        var changed = new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x04),
                                     KbmDestination.Zr, Custom: true);
        var untouched = new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x16),
                                       KbmDestination.LStickDown, Custom: false);

        Assert.Equal(KbmFingerprint.Canonical([changed]),
                     KbmFingerprint.Canonical([changed, untouched]));
    }
}
