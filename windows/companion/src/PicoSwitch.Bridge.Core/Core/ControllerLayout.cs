namespace PicoSwitch.Bridge.Core;

/*
 * Physical face-button LAYOUT, kept separate from logical button semantics.
 *
 * ## The contract this file has to satisfy
 *
 * The bridge wire contract is fixed and LOGICAL: HID usages 1..4 are Nintendo
 * A/B/X/Y. Descriptor-proven bridge input is routed straight to
 * `NS2_DST_A/B/X/Y` by `ns2_resolve_button_destination()`, bypassing the
 * positional B/A/Y/X map that directly paired physical controllers use. So
 * everything here has one job: get each ORIGIN's face input into that logical
 * contract.
 *
 * ## Why the two origins need OPPOSITE corrections
 *
 * A physical face KEY arrives as a platform key code, and what that code means
 * depends on the source device's plastic:
 *
 *  * A positional/Xbox-style source names the BOTTOM face button `A`, so its `A`
 *    is the console's B and its `X` is the console's Y. It needs the swap.
 *  * A Nintendo-labelled handheld names its keys after the PRINTED legend: the
 *    button printed `A` — the right-hand one — reports as `A`. It is already
 *    logical and must be left alone. This is not a guess: the first AYN Thor
 *    in-game pass forwarded key codes untranslated and came out inverted, which
 *    is only possible if the handheld reports by legend rather than by position
 *    (`docs/bluetooth/android-controller-bridge.md`).
 *
 * An on-screen face POSITION has no plastic at all. It has four slots and a
 * presentation preference that decides which letter is DRAWN in each, and the
 * letter drawn is the letter sent. Under a Nintendo presentation the south slot
 * is drawn `B` and must send B; under an Xbox presentation it is drawn `A` and
 * must send A.
 *
 * Those are opposite functions of the same ControllerFaceLayout. Collapsing them
 * into one mapper is what broke both origins in turn: correcting the on-screen
 * pad inverted every physical face key, because one shared mapper can only be
 * right for one origin at a time. Keep them apart.
 *
 *   physical key code  -> MapPhysicalFaceKey   -\
 *                                                >-- LOGICAL button -> HID usage 1..4
 *   on-screen position -> MapTouchFacePosition -/
 */

public enum ControllerFaceLayout
{
    Auto,
    Nintendo,
    Xbox,
}

public static class ControllerFaceLayouts
{
    public static string Key(this ControllerFaceLayout value) => value switch
    {
        ControllerFaceLayout.Nintendo => "nintendo",
        ControllerFaceLayout.Xbox => "xbox",
        _ => "auto",
    };

    public static string Title(this ControllerFaceLayout value) => value switch
    {
        ControllerFaceLayout.Nintendo => "Nintendo",
        ControllerFaceLayout.Xbox => "Xbox",
        _ => "Auto",
    };

    public static string Description(this ControllerFaceLayout value) => value switch
    {
        ControllerFaceLayout.Nintendo => "Match Nintendo-style A/B and X/Y printed labels",
        ControllerFaceLayout.Xbox => "Match the platform's positional (Xbox-style) face-button order",
        _ => "Use a known handheld profile, otherwise the platform's standard button positions",
    };

    public static ControllerFaceLayout FromKey(string? value) => value switch
    {
        "nintendo" => ControllerFaceLayout.Nintendo,
        "xbox" => ControllerFaceLayout.Xbox,
        _ => ControllerFaceLayout.Auto,
    };

    public static readonly IReadOnlyList<ControllerFaceLayout> All =
    [
        ControllerFaceLayout.Auto,
        ControllerFaceLayout.Nintendo,
        ControllerFaceLayout.Xbox,
    ];
}

/// <summary>
/// Stable identity of a host input source.
///
/// <c>Descriptor</c> is whatever opaque, stable string the platform uses to
/// recognize the same physical device across reconnects (a Windows HID instance
/// path, an Android <c>InputDevice.descriptor</c>, a Linux
/// <c>/dev/input/by-id</c> name). The bridge never parses it; it only compares
/// and persists it.
/// </summary>
public sealed record ControllerSourceIdentity(
    string Descriptor,
    string Name,
    int VendorId,
    int ProductId);

/// <summary>
/// A face-diamond slot named by WHERE it is, not by what is printed on it.
///
/// An on-screen controller has no plastic, so it has no printed legend to inherit
/// — it has four positions and a layout preference that decides what to draw in
/// them. Naming the positions is what stops a renderer from assuming "A is always
/// the bottom one", which is false across controller families.
///
/// <see cref="FaceButtonPositions.Positional"/> is that same position expressed
/// in the enum the rest of the bridge already uses, so a software press enters
/// <see cref="ControllerLayoutResolver"/> by a single documented route and cannot
/// acquire a second mapping table of its own.
/// </summary>
public enum FaceButtonPosition
{
    South,
    East,
    West,
    North,
}

public static class FaceButtonPositions
{
    public static ControllerButton Positional(this FaceButtonPosition value) => value switch
    {
        FaceButtonPosition.South => ControllerButton.A,
        FaceButtonPosition.East => ControllerButton.B,
        FaceButtonPosition.West => ControllerButton.X,
        _ => ControllerButton.Y,
    };
}

public sealed record ResolvedControllerLayout(ControllerFaceLayout Layout, string Reason);

public static class ControllerLayoutResolver
{
    public static ResolvedControllerLayout Resolve(
        ControllerFaceLayout requested,
        ControllerSourceIdentity? source)
    {
        if (requested != ControllerFaceLayout.Auto)
        {
            return new ResolvedControllerLayout(requested, "Selected manually");
        }

        if (source is null)
        {
            return new ResolvedControllerLayout(
                ControllerFaceLayout.Xbox,
                "No input source selected; using positional order");
        }

        // No portable platform property exposes the printed legend. These are the
        // bounded, hardware-audited built-in controller identities; they select
        // labels only and never gate whether the device is usable. The manual
        // override remains authoritative.
        //
        // AYN's handhelds carry a button-layout toggle that changes the DEVICE
        // IDENTITY, and with it which key code each physical button sends. Both
        // modes were read off a live Odin 2 Mini on 2026-08-24:
        //
        //   0x2020/0x0111  "Odin Controller"          reports the PRINTED legend
        //   0x2020/0x0112  "Xbox Wireless Controller" reports POSITIONALLY
        //
        // So the two PIDs must NOT resolve alike, however similar the hardware is:
        // Xbox mode is an Xbox-style source that happens to live behind
        // Nintendo-printed plastic, and treating it as Nintendo inverts every face
        // button. That is a real field failure, not a hypothetical.
        var reportsPrintedLegend =
            (source.VendorId == 0x2020 && source.ProductId == 0x0111) ||
            (source.VendorId == 0x2022 && source.ProductId == 0x3001) ||
            source.Name.Contains("Odin Controller", StringComparison.OrdinalIgnoreCase) ||
            source.Name.Contains("Retroid Pocket Controller", StringComparison.OrdinalIgnoreCase);

        return reportsPrintedLegend
            ? new ResolvedControllerLayout(
                ControllerFaceLayout.Nintendo,
                "Known Nintendo-labeled handheld controller")
            : new ResolvedControllerLayout(
                ControllerFaceLayout.Xbox,
                "Platforms expose positions, not printed labels");
    }

    /// <summary>
    /// The letter a face POSITION should be drawn with under <paramref name="resolved"/>.
    ///
    /// Derived from <see cref="MapTouchFacePosition"/> rather than from a second
    /// table on purpose: a drawn label that disagrees with the bit that gets sent
    /// is the exact failure a renderer's own layout switch produces, and it is
    /// invisible until someone presses the button on a console.
    /// </summary>
    public static string FaceLabel(FaceButtonPosition position, ControllerFaceLayout resolved) =>
        MapTouchFacePosition(position.Positional(), resolved).ToString();

    /// <summary>
    /// ON-SCREEN face position -> logical bridge button.
    ///
    /// The on-screen pad draws the letter its presentation calls for and sends
    /// exactly that letter: a Nintendo presentation (south drawn <c>B</c>) swaps
    /// the positional slot into its printed letter, an Xbox presentation (south
    /// drawn <c>A</c>) leaves it alone. <see cref="FaceLabel"/> is derived from
    /// this, which is what keeps the legend and the wire bit from drifting apart.
    ///
    /// Do NOT route physical key codes through here — see
    /// <see cref="MapPhysicalFaceKey"/>.
    /// </summary>
    public static ControllerButton MapTouchFacePosition(
        ControllerButton position,
        ControllerFaceLayout resolved) =>
        resolved == ControllerFaceLayout.Nintendo ? SwapFaces(position) : position;

    /// <summary>
    /// PHYSICAL face key, as the source device reported it -> logical bridge button.
    ///
    /// A Nintendo-labelled handheld already reports its printed letters, so it
    /// passes through untouched. Every other source reports positionally, where
    /// the bottom button is <c>A</c> while the console's bottom button is B — so
    /// those are swapped into logical order here, once, at the only boundary that
    /// knows which kind of device is attached.
    ///
    /// Non-face buttons are returned unchanged; shoulders, Start/Select and the
    /// stick clicks mean the same thing under either layout.
    ///
    /// Do NOT route on-screen positions through here — see
    /// <see cref="MapTouchFacePosition"/>.
    /// </summary>
    public static ControllerButton MapPhysicalFaceKey(
        ControllerButton reported,
        ControllerFaceLayout resolved) =>
        resolved == ControllerFaceLayout.Nintendo ? reported : SwapFaces(reported);

    /// <summary>A↔B and X↔Y; everything else is identity.</summary>
    private static ControllerButton SwapFaces(ControllerButton button) => button switch
    {
        ControllerButton.A => ControllerButton.B,
        ControllerButton.B => ControllerButton.A,
        ControllerButton.X => ControllerButton.Y,
        ControllerButton.Y => ControllerButton.X,
        _ => button,
    };
}

/// <summary>
/// Per-source layout persistence, implemented by the platform (a settings file,
/// the registry, ApplicationData). The bridge only needs a stable string keyed by
/// descriptor.
/// </summary>
public interface IControllerLayoutStore
{
    ControllerFaceLayout Load(string? descriptor);

    void Save(string descriptor, ControllerFaceLayout layout);

    /// <summary>For hosts with no persistence, and for tests.</summary>
    public sealed class None : IControllerLayoutStore
    {
        public static readonly None Instance = new();

        public ControllerFaceLayout Load(string? descriptor) => ControllerFaceLayout.Auto;

        public void Save(string descriptor, ControllerFaceLayout layout)
        {
        }
    }
}
