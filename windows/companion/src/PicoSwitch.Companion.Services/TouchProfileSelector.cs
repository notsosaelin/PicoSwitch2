using PicoSwitch.Bridge.Touch;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Which touch layout the adapter's CONFIRMED personality calls for.
///
/// ## Why this is a selector and not a user setting
///
/// The surface draws the controller the console is currently being shown. A user-chosen
/// personality would let the screen present a GameCube face while the adapter enumerates
/// as a Pro Controller 2, and the labels under the thumb would then be a lie — which is
/// the one thing an on-screen controller must never be. `WINDOWS_PASS.md` §15.8 states
/// the rule and the Android companion has always worked this way.
///
/// ## Why null is a real answer
///
/// `Config` and `Unknown` map to null, and null keeps the surface neutral and disabled
/// rather than defaulting to Pro Controller 2. Not knowing what the console sees is a
/// different state from knowing it is a Pro Controller 2, and only one of them is safe to
/// draw.
///
/// ## Why it lives here rather than in the portable core
///
/// <see cref="Personality"/> is a management-protocol type and
/// <see cref="TouchProfileId"/> is a bridge type; `PicoSwitch.Bridge.Core` deliberately
/// cannot see the management assembly (WINDOWS_PASS.md §10.2), so the one place both are
/// visible is this layer. The mapping is trivial, but it is the seam where a new
/// personality gets forgotten, so it is named and tested rather than inlined.
/// </summary>
public static class TouchProfileSelector
{
    /// <summary>
    /// The personalities the on-screen controller can actually play.
    /// </summary>
    /// <remarks>
    /// Derived from <see cref="Select"/> rather than listed twice: a personality
    /// that maps to no touch profile has no layout to draw, and the two answers
    /// must never be able to disagree. Ports the Android
    /// <c>TouchProfileSelector.gameplayPersonalities</c> set.
    /// </remarks>
    public static IReadOnlyList<Personality> GameplayPersonalities { get; } =
        [.. Enum.GetValues<Personality>().Where(p => Select(p) is not null)];

    public static TouchProfileId? Select(Personality personality) => personality switch
    {
        Personality.Pro2 => TouchProfileId.Pro2,
        Personality.GameCube => TouchProfileId.GameCube,
        Personality.JoyConLeft => TouchProfileId.JoyConLeft,
        Personality.JoyConRight => TouchProfileId.JoyConRight,

        // Config is the adapter's own management face, not a controller the console is
        // being shown; Unknown is the answer before anything has been confirmed.
        _ => null,
    };

    /// <summary>
    /// The live personality, or the last one this adapter was CONFIRMED to be showing.
    /// </summary>
    /// <remarks>
    /// A remembered personality is still a confirmation — it is what the adapter reported
    /// at the last verified connection (<c>AdapterRecord.LastPersonality</c>) — and using
    /// it is what lets a user lay out their controller on the train. It is not a guess,
    /// and it is not a default: with nothing ever connected there is still nothing to
    /// draw.
    ///
    /// It can be STALE, which is why the surface says so rather than presenting it as
    /// live. Everything the editor does is local, so a stale answer costs the user a
    /// wrong-looking layout and never a wrong button on a console.
    /// </remarks>
    public static TouchProfileId? SelectOrRemembered(
        Personality live, string? lastConfirmedWireName) =>
        Select(live) ?? Select(Personalities.FromWire(lastConfirmedWireName));
}
