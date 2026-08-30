using System.Text.Json;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The embedded canonical defaults, against the firmware that generated them.
/// </summary>
/// <remarks>
/// These exist so a profile can be created and edited with NO adapter connected.
/// A local profile stores only sparse overrides, so drawing one needs the table
/// they are applied against — and fetching that table from the adapter is what
/// made the library require a connection it should never have needed.
///
/// Embedding firmware data is only safe if it cannot drift, which is what this
/// checks.
/// </remarks>
public sealed class KbmDefaultsTests
{
    [Fact]
    public void TheEmbeddedTableMatchesWhatTheFirmwareEmits()
    {
        var text = RepositoryFixtures.ReadText(
            "tools/fixtures/management/kbm-default-mappings.json");
        using var document = JsonDocument.Parse(text);
        var layouts = document.RootElement.GetProperty("layouts");

        var checkedLayouts = 0;
        foreach (var entry in layouts.EnumerateObject())
        {
            var layout = KbmLayouts.FromWire(entry.Name);
            Assert.NotNull(layout);

            var expected = entry.Value.GetProperty("bindings").EnumerateArray()
                .Select(row => (row.GetProperty("src").GetString()!,
                                row.GetProperty("dst").GetString()!))
                .ToList();

            var embedded = KbmDefaults.For(layout!.Value).Bindings
                .Select(binding => (binding.Source.Wire, binding.Destination.Wire()))
                .ToList();

            Assert.Equal(expected, embedded);
            checkedLayouts++;
        }

        Assert.Equal(2, checkedLayouts);
    }

    [Fact]
    public void BothLayoutsHaveARealTable()
    {
        // An empty table would let the app draw a blank keyboard offline and call
        // it a mapping. The resource has to actually be in the assembly.
        foreach (var layout in KbmLayouts.All)
        {
            Assert.NotEmpty(KbmDefaults.For(layout).Bindings);
            Assert.True(KbmDefaults.For(layout).Mouse.SensitivityX > 0);
        }
    }

    [Fact]
    public void TheLayoutsDifferBecauseOnlyOneOfThemHasAMouse()
    {
        var keyboard = KbmDefaults.For(KbmLayout.Keyboard).Bindings;
        var combined = KbmDefaults.For(KbmLayout.KeyboardMouse).Bindings;

        Assert.DoesNotContain(keyboard,
                              b => b.Source.Kind == KbmSourceKind.MouseButton);
        Assert.Contains(combined, b => b.Source.Kind == KbmSourceKind.MouseButton);
    }

    /* ------------------------------------------------------ effective mapping */

    [Fact]
    public void AProfileWithNoOverridesIsExactlyTheDefault()
    {
        // What "New from Default" produces, and it needs no adapter to compose.
        var effective = KbmDefaults.Effective(KbmLayout.Keyboard, []);

        Assert.Equal(KbmDefaults.For(KbmLayout.Keyboard).Bindings.Count,
                     effective.Count);
        Assert.All(effective, binding => Assert.False(binding.Custom));
    }

    [Fact]
    public void AnOverrideReplacesTheDefaultAndIsMarkedCustom()
    {
        var source = KbmDefaults.For(KbmLayout.Keyboard).Bindings[0].Source;
        var effective = KbmDefaults.Effective(
            KbmLayout.Keyboard,
            [new KbmBinding(source, KbmDestination.Capture, Custom: true)]);

        var changed = effective.Single(b => b.Source.Wire == source.Wire);
        Assert.Equal(KbmDestination.Capture, changed.Destination);
        Assert.True(changed.Custom);

        // And it replaces rather than adds.
        Assert.Equal(KbmDefaults.For(KbmLayout.Keyboard).Bindings.Count,
                     effective.Count);
    }

    [Fact]
    public void AnOverrideOnAnInputTheDefaultDoesNotBindIsAddedToTheMapping()
    {
        // A key with no canonical binding is a new row, not a discarded one.
        var unbound = new KbmSource(KbmSourceKind.Key, 0x9A);
        Assert.DoesNotContain(KbmDefaults.For(KbmLayout.Keyboard).Bindings,
                              b => b.Source.Wire == unbound.Wire);

        var effective = KbmDefaults.Effective(
            KbmLayout.Keyboard,
            [new KbmBinding(unbound, KbmDestination.Home, Custom: true)]);

        Assert.Contains(effective, b => b.Source.Wire == unbound.Wire);
        Assert.Equal(KbmDefaults.For(KbmLayout.Keyboard).Bindings.Count + 1,
                     effective.Count);
    }

    [Fact]
    public void AnExplicitlyClearedBindingIsKeptRatherThanFallingBackToTheDefault()
    {
        // "This key does nothing" is a real answer and differs from the default
        // the adapter would otherwise apply. Dropping it would silently restore
        // the very binding the user removed.
        var source = KbmDefaults.For(KbmLayout.Keyboard).Bindings[0].Source;
        var effective = KbmDefaults.Effective(
            KbmLayout.Keyboard,
            [new KbmBinding(source, KbmDestination.None, Custom: true)]);

        Assert.Equal(KbmDestination.None,
                     effective.Single(b => b.Source.Wire == source.Wire).Destination);
    }
}
