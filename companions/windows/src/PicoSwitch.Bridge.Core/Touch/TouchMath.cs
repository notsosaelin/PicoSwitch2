using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// The one conversion between the touch engine's geometry domain and bridge wire
/// units.
///
/// The engine works in <c>[-1,+1]</c> because that is the domain the standard
/// gamepad model and every piece of stick math are written in; the bridge works
/// in <c>0..255</c> because that is the wire. Converting in exactly one place is
/// what stops a renderer, a control and a settings screen from each rounding
/// slightly differently.
///
/// The endpoint asymmetry is deliberate and is NOT a rounding bug:
/// <c>128 + n*127</c> puts full deflection at <c>1</c> and <c>255</c>, which is
/// what <c>AxisRange.Stick</c> — the physical path — has always produced. A touch
/// stick that reached <c>0</c> while the physical stick reached <c>1</c> would be
/// a second contract for the same axis, so the shared formula wins over the
/// tidier-looking one.
/// </summary>
public static class TouchAxis
{
    public const int Neutral = 128;

    /// <summary>
    /// <c>[-1,+1]</c> -> <c>0..255</c>, negative left/up.
    ///
    /// Non-finite input resolves to neutral rather than propagating: a NaN here
    /// would become a garbage axis byte on the wire, and the only honest value
    /// for "we do not know where the stick is" is centred.
    /// </summary>
    public static int ToBridge(float value)
    {
        if (!float.IsFinite(value))
        {
            return Neutral;
        }

        return Math.Clamp(
            (int)MathF.Round(Neutral + Math.Clamp(value, -1f, 1f) * 127f,
                             MidpointRounding.AwayFromZero),
            0,
            255);
    }

    /// <summary><c>[0,1]</c> -> <c>0..255</c>, rest at <c>0</c>.</summary>
    public static int TriggerToBridge(float value)
    {
        if (!float.IsFinite(value))
        {
            return 0;
        }

        return Math.Clamp(
            (int)MathF.Round(Math.Clamp(value, 0f, 1f) * 255f, MidpointRounding.AwayFromZero),
            0,
            255);
    }
}

/// <summary>A stick position in the portable domain: <c>[-1,+1]</c>, negative left and up.</summary>
public readonly record struct TouchVector(float X, float Y)
{
    public static readonly TouchVector Zero = new(0f, 0f);
}

/// <summary>
/// Analog stick geometry, shared by every stick and every future host client.
///
/// Two rules matter and both are easy to get subtly wrong:
///
/// 1. CIRCULAR clamping. Clamping X and Y independently produces a square gate,
///    so a diagonal reaches magnitude <c>sqrt(2)</c> before anything notices and
///    the stick feels faster on the diagonals than on the cardinals. The vector's
///    magnitude is what gets clamped; direction is preserved exactly.
///
/// 2. RADIAL deadzone with RESCALING. Simply discarding an inner radius also
///    discards that much of the usable range, so full deflection becomes
///    unreachable at the top. The magnitude above the threshold is remapped onto
///    the whole <c>0..1</c> range instead.
/// </summary>
public static class TouchStick
{
    /// <param name="dx">Displacement from the stick centre, in the layout's coordinate space.</param>
    /// <param name="dy">Displacement from the stick centre, positive DOWN (screen convention).</param>
    /// <param name="radius">Travel radius in the same space; the distance meaning full deflection.</param>
    /// <param name="deadzone">Inner fraction of <paramref name="radius"/> that publishes centre, <c>0..1</c>.</param>
    public static TouchVector Resolve(float dx, float dy, float radius, float deadzone)
    {
        if (!float.IsFinite(dx) || !float.IsFinite(dy) || !float.IsFinite(radius) || radius <= 0f)
        {
            return TouchVector.Zero;
        }

        var nx = dx / radius;
        var ny = dy / radius;
        var magnitude = MathF.Sqrt((nx * nx) + (ny * ny));
        if (magnitude <= 0f)
        {
            return TouchVector.Zero;
        }

        var gate = Math.Clamp(deadzone, 0f, 0.9f);
        if (magnitude <= gate)
        {
            return TouchVector.Zero;
        }

        // Rescale so the first movement past the gate is small and the rim is
        // still exactly full scale.
        var scaled = Math.Clamp((magnitude - gate) / (1f - gate), 0f, 1f);
        return new TouchVector(nx / magnitude * scaled, ny / magnitude * scaled);
    }
}

/// <summary>
/// D-pad geometry: one 2D control with eight sectors, not four unrelated buttons.
///
/// A thumb slides around the ring rather than jumping between discrete keys, so
/// the useful model is an angle plus two thresholds:
///
/// - a RADIAL pair, <see cref="TouchControlConfig.DpadEnterFraction"/> to engage
///   and a lower <see cref="TouchControlConfig.DpadExitFraction"/> to disengage,
///   so resting a thumb near the centre does not flicker on and off;
/// - an ANGULAR margin, <see cref="TouchControlConfig.DpadHysteresisDegrees"/>, so
///   a thumb parked on a sector boundary does not alternate Up / UpRight on
///   sub-pixel noise.
///
/// Opposite directions are structurally impossible from one contact: a single
/// angle selects a single sector, and only diagonals set two flags. Nothing here
/// needs a cancellation rule, and the wire hat code stays where it belongs — in
/// the protocol encoder.
/// </summary>
public static class TouchDpad
{
    /// <summary>Sector index -> the two flags it raises. Index 0 is East, counter-clockwise.</summary>
    private static readonly DpadState[] Sectors =
    [
        new(Right: true),
        new(Up: true, Right: true),
        new(Up: true),
        new(Up: true, Left: true),
        new(Left: true),
        new(Down: true, Left: true),
        new(Down: true),
        new(Down: true, Right: true),
    ];

    private const float SectorDegrees = 45f;
    private const float HalfSector = SectorDegrees / 2f;

    /// <param name="dy">
    /// Positive DOWN, as the platform reports it; converted internally so that 90
    /// degrees means Up.
    /// </param>
    /// <param name="previous">The direction this control published last, for hysteresis.</param>
    public static DpadState Resolve(
        float dx, float dy, float radius, TouchControlConfig config, DpadState previous)
    {
        if (!float.IsFinite(dx) || !float.IsFinite(dy) || !float.IsFinite(radius) || radius <= 0f)
        {
            return DpadState.None;
        }

        var magnitude = MathF.Sqrt((dx * dx) + (dy * dy)) / radius;
        var engaged = previous != DpadState.None;
        var threshold = engaged ? config.DpadExitFraction : config.DpadEnterFraction;
        if (magnitude < threshold)
        {
            return DpadState.None;
        }

        // Atan2 on -dy so the maths frame is the conventional one (90 = up) even
        // though the input frame has y growing downward.
        var degrees = (float)(Math.Atan2(-dy, dx) * 180d / Math.PI);
        if (degrees < 0f)
        {
            degrees += 360f;
        }

        var candidate = (int)((degrees + HalfSector) / SectorDegrees) % Sectors.Length;
        var held = Array.IndexOf(Sectors, previous);
        if (held >= 0)
        {
            var offset = AngularDistance(degrees, held * SectorDegrees);
            if (offset <= HalfSector + config.DpadHysteresisDegrees)
            {
                return Sectors[held];
            }
        }

        return Sectors[candidate];
    }

    private static float AngularDistance(float a, float b)
    {
        var raw = MathF.Abs(a - b) % 360f;
        return raw > 180f ? 360f - raw : raw;
    }
}
