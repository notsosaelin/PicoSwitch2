namespace PicoSwitch.Bridge.Core;

/// <summary>
/// The PicoSwitch Bridge's logical controller buttons.
///
/// These are BRIDGE semantics, not any host platform's. A platform backend
/// translates its own identifiers (a Windows <c>GamepadButtons</c> flag, a raw
/// HID usage, an Android <c>KEYCODE_BUTTON_A</c>) into these before anything
/// shared sees them.
///
/// The VALUE IS THE WIRE BIT: <see cref="Protocol.ControllerReportEncoder"/>
/// writes <c>1 &lt;&lt; value</c> into the button field, and the firmware's
/// generic sequential profile reads usage <c>value + 1</c>. The numbers are
/// therefore written out explicitly rather than left implicit — append only, and
/// never reorder.
///
/// <c>C</c> is the Switch 2 GameChat button (usage 15). <c>GL</c> and <c>GR</c>
/// are the Pro Controller 2 GRIP buttons (usages 16/17), which bridge contract 4
/// added. Almost no host device has a physical key for any of the three, which is
/// exactly why backends are expected to offer them as virtual buttons alongside
/// Home and Capture.
/// </summary>
public enum ControllerButton
{
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    L1 = 4,
    R1 = 5,
    L2 = 6,
    R2 = 7,
    Select = 8,
    Start = 9,
    LeftStick = 10,
    RightStick = 11,
    Home = 12,
    Capture = 13,
    C = 14,
    GL = 15,
    GR = 16,
}

/// <summary>
/// An immutable set of held buttons, stored as the wire bitmask.
///
/// Kotlin's <c>Set&lt;ControllerButton&gt;</c> inside a <c>data class</c> gives
/// structural equality for free, and <c>MutableStateFlow</c> relies on it to drop
/// an unchanged snapshot. A C# record holding a <c>HashSet</c> compares by
/// reference, so a literal translation would publish a change event for every
/// input event — 125 times a second, all of them identical.
///
/// Storing the mask rather than a set also removes the one place the encoder
/// could disagree with the model about what a button's bit is: there is now
/// exactly one <c>1 &lt;&lt; value</c> in the codebase, and it is here.
/// </summary>
public readonly record struct ControllerButtonSet
{
    private ControllerButtonSet(int bits) => Bits = bits;

    /// <summary>The wire button field: bit <c>n</c> is the button whose enum value is <c>n</c>.</summary>
    public int Bits { get; }

    public static ControllerButtonSet Empty => default;

    public static ControllerButtonSet Of(params ControllerButton[] buttons)
    {
        var bits = 0;
        foreach (var button in buttons)
        {
            bits |= 1 << (int)button;
        }

        return new ControllerButtonSet(bits);
    }

    public static ControllerButtonSet Of(IEnumerable<ControllerButton> buttons)
    {
        var bits = 0;
        foreach (var button in buttons)
        {
            bits |= 1 << (int)button;
        }

        return new ControllerButtonSet(bits);
    }

    public bool Contains(ControllerButton button) => (Bits & (1 << (int)button)) != 0;

    public ControllerButtonSet With(ControllerButton button) =>
        new(Bits | (1 << (int)button));

    public ControllerButtonSet Without(ControllerButton button) =>
        new(Bits & ~(1 << (int)button));

    public ControllerButtonSet With(ControllerButton button, bool pressed) =>
        pressed ? With(button) : Without(button);

    public ControllerButtonSet Union(ControllerButtonSet other) => new(Bits | other.Bits);

    public int Count => System.Numerics.BitOperations.PopCount((uint)Bits);

    public bool IsEmpty => Bits == 0;

    public IEnumerable<ControllerButton> Values
    {
        get
        {
            for (var value = 0; value <= (int)ControllerButton.GR; value++)
            {
                if ((Bits & (1 << value)) != 0)
                {
                    yield return (ControllerButton)value;
                }
            }
        }
    }

    public override string ToString() =>
        IsEmpty ? "{}" : "{" + string.Join(", ", Values) + "}";
}

/// <summary>
/// One motion sample in the bridge's canonical convention.
///
/// Units, axes and orientation are defined once, in
/// <see cref="MotionConvention"/>. A platform backend converts its own sensor
/// frame and units into that convention before constructing this; nothing
/// downstream re-interprets it.
///
/// <c>Valid</c> is what tells the adapter whether motion is live; when the
/// sensors are idled (the console is not consuming motion) the flag clears and
/// the firmware stops publishing motion rather than latching the last sample.
/// </summary>
public readonly record struct ControllerMotion(
    int GyroX = 0,
    int GyroY = 0,
    int GyroZ = 0,
    int AccelX = 0,
    int AccelY = 0,
    int AccelZ = 0,

    // TimestampTicks is a free-running 100 us stamp; it wraps at 16 bits on the
    // wire. See MotionConvention for why it must come from the sensor sample.
    int TimestampTicks = 0,
    bool Valid = false)
{
    public static ControllerMotion None => default;
}

/// <summary>Host battery, forwarded so the console shows a real controller battery.</summary>
public readonly record struct ControllerBattery(
    int LevelPercent = 0,
    bool Charging = false,
    bool Valid = false)
{
    public static ControllerBattery Unknown => default;
}

/// <summary>
/// The complete normalized controller state — the single value that crosses the
/// platform boundary in the input direction.
///
/// Sticks are <c>0..255</c> with <c>128</c> neutral; triggers are <c>0..255</c>
/// with <c>0</c> at rest. Those are the bridge's units, chosen to match the wire
/// contract exactly so the encoder is a copy rather than a second place where
/// scaling can be wrong. Backends normalize into them; see
/// <see cref="AxisRange"/>.
/// </summary>
public sealed record ControllerState
{
    public static readonly ControllerState Neutral = new();

    public int LeftX { get; init; } = 128;

    public int LeftY { get; init; } = 128;

    public int RightX { get; init; } = 128;

    public int RightY { get; init; } = 128;

    public int LeftTrigger { get; init; }

    public int RightTrigger { get; init; }

    public ControllerButtonSet Buttons { get; init; } = ControllerButtonSet.Empty;

    public bool DpadUp { get; init; }

    public bool DpadRight { get; init; }

    public bool DpadDown { get; init; }

    public bool DpadLeft { get; init; }

    public ControllerMotion Motion { get; init; } = ControllerMotion.None;

    public ControllerBattery Battery { get; init; } = ControllerBattery.Unknown;
}

/// <summary>
/// A host axis's reported range, and the conversion into bridge units.
///
/// Every platform reports analog axes with its own minimum/maximum/rest values,
/// and most report a manufacturer dead zone alongside. Handing those three
/// numbers to shared code keeps the normalization identical across platforms
/// instead of each backend inventing its own curve.
/// </summary>
public readonly record struct AxisRange(float Minimum, float Maximum, float Flat = 0f)
{
    public int Stick(float value, bool invert = false)
    {
        var center = (Minimum + Maximum) / 2f;
        var span = (Maximum - Minimum) / 2f;
        var radius = span > 0f ? span : 1f;
        var normalized = Math.Clamp((value - center) / radius, -1f, 1f);
        var deadZone = Math.Max(Flat / radius, 0.04f);
        if (Math.Abs(normalized) <= deadZone)
        {
            normalized = 0f;
        }
        else
        {
            var magnitude = Math.Clamp((Math.Abs(normalized) - deadZone) / (1f - deadZone), 0f, 1f);
            normalized = Math.Sign(normalized) * magnitude;
        }

        if (invert)
        {
            normalized = -normalized;
        }

        return Math.Clamp((int)MathF.Round(128f + (normalized * 127f), MidpointRounding.AwayFromZero), 0, 255);
    }

    public int Trigger(float value)
    {
        var width = Maximum - Minimum;
        var span = width > 0f ? width : 1f;
        var normalized = Math.Clamp((value - Minimum) / span, 0f, 1f);
        return (int)MathF.Round(normalized * 255f, MidpointRounding.AwayFromZero);
    }
}

/// <summary>
/// Digital D-pad state, however the host produced it.
///
/// Four retained directions rather than a hat code: opposite directions cancel,
/// and releasing one side restores the still-held side without inventing an
/// edge. The hat encoding is a wire detail and lives in the protocol layer.
/// </summary>
public readonly record struct DpadState(
    bool Up = false,
    bool Right = false,
    bool Down = false,
    bool Left = false)
{
    public static DpadState None => default;

    /// <summary>Threshold for an analog hat axis pair; <c>-1..1</c>, positive right/down.</summary>
    public static DpadState FromAxes(float x, float y) =>
        new(Up: y < -0.5f, Right: x > 0.5f, Down: y > 0.5f, Left: x < -0.5f);
}

/// <summary>
/// The analog half of one host input event, applied as a unit.
///
/// Grouped on purpose: a platform delivers sticks, triggers and hat together, and
/// publishing them as one state change keeps a single physical event from
/// becoming several observable snapshots.
///
/// <c>Dpad</c> is null when the source has no hat axes at all, which means "leave
/// the hat contribution as it is" rather than "the hat is centered".
/// </summary>
public readonly record struct AnalogFrame(
    int LeftX,
    int LeftY,
    int RightX,
    int RightY,
    int LeftTrigger,
    int RightTrigger,
    DpadState? Dpad = null)
{
    public static readonly AnalogFrame Neutral = new(128, 128, 128, 128, 0, 0);
}
