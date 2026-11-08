using PicoSwitch.Companion.Linux.Display;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// Game Mode's UI scale: one unit the physical size of an Odin 2 Mini dp, never
/// more than 1080 units high on a large display.
/// </summary>
public sealed class GamescopeScaleTests
{
    /// <summary>ROG Xbox Ally X as gamescope reports it (2026-11-08): 1920x1080, 150x90 mm.</summary>
    [Fact]
    public void TheAllyGetsTheOdinsPhysicalSize() =>
        Assert.Equal(1.7, GamescopeScale.For(1920, 1080, 150, 90));

    /// <summary>The reference itself: 1920x1080 on a 5-inch 16:9 panel is density 2.3.</summary>
    [Fact]
    public void TheOdinsOwnPanelComesOutAtItsAndroidDensity() =>
        Assert.Equal(2.3, GamescopeScale.For(1920, 1080, 111, 62));

    /// <summary>A lower game resolution on the same panel keeps the same physical size.</summary>
    [Fact]
    public void ALowerGameResolutionScalesDownInProportion() =>
        Assert.Equal(1.15, GamescopeScale.For(1280, 720, 150, 90));

    [Theory]
    [InlineData(3840, 2160, 1210, 680, 2.0)]   // 55-inch 4K TV: resolution floor
    [InlineData(1920, 1080, 1210, 680, 1.0)]   // 55-inch 1080p TV
    [InlineData(1920, 1080, 0, 0, 1.0)]        // no physical size: resolution rule alone
    [InlineData(1920, 1080, 16, 9, 1.0)]       // implausible millimetres (aspect ratio, not size)
    public void LargeOrUnmeasuredDisplaysUseTheResolutionFloor(int w, int h, int mmW, int mmH, double expected) =>
        Assert.Equal(expected, GamescopeScale.For(w, h, mmW, mmH));

    [Theory]
    [InlineData("gamescope-0", null, true)]
    [InlineData(null, "gamescope", true)]
    [InlineData(null, "GNOME", false)]
    [InlineData(null, null, false)]
    public void GamescopeIsRecognisedFromItsEnvironment(string? wayland, string? desktop, bool expected) =>
        Assert.Equal(expected, GamescopeScale.UnderGamescope(name => name switch
        {
            "GAMESCOPE_WAYLAND_DISPLAY" => wayland,
            "XDG_CURRENT_DESKTOP" => desktop,
            _ => null,
        }));
}
