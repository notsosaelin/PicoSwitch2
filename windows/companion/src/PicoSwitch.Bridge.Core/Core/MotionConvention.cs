namespace PicoSwitch.Bridge.Core;

/*
 * THE canonical motion convention of the PicoSwitch Bridge.
 *
 * Motion is the one part of this contract where a platform convention can leak
 * in unnoticed and stay wrong for months, because a wrong axis still *works* --
 * it just aims sideways. So the convention is defined here, independently of any
 * host operating system, and every platform backend converts into it.
 *
 * ## Frame
 *
 * Right-handed, expressed in the frame the user is HOLDING the device, not the
 * frame the device was manufactured in:
 *
 *   +X  right, along the top edge of the screen
 *   +Y  up, toward the top of the screen
 *   +Z  out of the screen, toward the user
 *
 * Gyroscope values are angular rate about those axes, positive counter-clockwise
 * when looking from the positive end of the axis toward the origin (the standard
 * right-hand rule). Accelerometer values are proper acceleration and are
 * GRAVITY-INCLUSIVE: a device lying face up on a table reads about +1 g on Z, not
 * zero. The firmware's stillness/bias tracking depends on that.
 *
 * This is deliberately the same frame the Switch 2 carrier expects, which is why
 * the firmware's `SWITCH_MOTION_SOURCE_ANDROID` seam row is the identity. It is
 * not "Android's frame promoted to a standard": Android happens to define its
 * sensor frame the same way, in the device's NATURAL orientation, and the
 * remaining difference -- natural orientation versus held orientation -- is
 * exactly what ScreenOrientation exists to remove.
 *
 * ## Units
 *
 * Fixed-point counts, not SI, because the wire contract is fixed point and a
 * float intermediate would put rounding in two places:
 *
 *   acceleration: 8192 counts per g
 *   angular rate: 16.384 counts per degree/second
 *
 * These match what the adapter already receives from a DualSense, so a host
 * device reuses the hardware-validated motion translation instead of adding a
 * second scaling convention. MotionScale converts from SI, which is what most
 * platform sensor APIs report.
 *
 * ## Timestamps
 *
 * ControllerMotion.TimestampTicks is a free-running counter in 100 us ticks,
 * truncated to 16 bits on the wire (wraps every 6.5536 s; the firmware takes the
 * delta in the field's own modulus).
 *
 * It MUST come from the sensor sample, never from send time. The report cadence
 * is faster than a typical IMU delivers, so the same physical sample is sent more
 * than once; the adapter de-duplicates on this field and only advances its motion
 * sequence when it genuinely changes. Stamping at send time makes every repeat
 * look like a fresh IMU frame, which the console integrates as real movement.
 *
 * 100 us rather than 1 ms because the adapter integrates angular rate against
 * this clock: at a 125 Hz cadence a 1 ms quantum is 12.5% of the interval, the
 * same order as the Bluetooth arrival jitter the timestamp exists to eliminate.
 *
 * ## Where conversion happens
 *
 *   platform sensor frame + platform units
 *            v   platform backend (device transform, orientation, MotionScale)
 *   CANONICAL BRIDGE MOTION  (this file)
 *            v   protocol layer
 *   PicoSwitch wire report
 *
 * Nothing after the platform backend re-interprets axes or units.
 */

public static class MotionConvention
{
    public const double AccelCountsPerG = 8192.0;
    public const double GyroCountsPerDps = 16.384;
    public const double GravityMs2 = 9.80665;
    public const double DegreesPerRadian = 57.2957795;

    /// <summary>Wire ticks per second for <c>ControllerMotion.TimestampTicks</c>.</summary>
    public const long TimestampTicksPerSecond = 10_000L;

    /// <summary>Nanoseconds per tick, for backends whose sensor clock is in nanoseconds.</summary>
    public const long TimestampNanosPerTick = 100_000L;

    /// <summary>The field is 16 bits on the wire; wrap is expected and handled downstream.</summary>
    public const int TimestampMask = 0xFFFF;
}

/// <summary>SI sensor values -> canonical bridge counts. See <see cref="MotionConvention"/>.</summary>
public static class MotionScale
{
    /// <summary>Proper acceleration in m/s^2, gravity-inclusive.</summary>
    public static int AccelCounts(float metersPerSecondSquared) =>
        Clamp16(metersPerSecondSquared *
            (MotionConvention.AccelCountsPerG / MotionConvention.GravityMs2));

    /// <summary>Angular rate in rad/s.</summary>
    public static int GyroCounts(float radiansPerSecond) =>
        Clamp16(radiansPerSecond *
            MotionConvention.DegreesPerRadian * MotionConvention.GyroCountsPerDps);

    /// <summary>Truncate a free-running nanosecond sensor clock to the wire tick field.</summary>
    public static int TimestampTicks(long sensorTimestampNanos) =>
        (int)((sensorTimestampNanos / MotionConvention.TimestampNanosPerTick) &
            MotionConvention.TimestampMask);

    /// <summary>
    /// Round-half-UP then clamp to the signed 16-bit wire field.
    ///
    /// Half-up rather than .NET's default half-to-even or away-from-zero, because
    /// the Kotlin implementation uses <c>roundToInt()</c>, which is
    /// <c>floor(x + 0.5)</c>. The three disagree only on exact midpoints, and only
    /// negative midpoints separate half-up from away-from-zero — but "only on
    /// midpoints" is exactly the kind of one-count divergence that a cross-language
    /// golden would catch six months later on a console.
    /// </summary>
    public static int Clamp16(double value) =>
        (int)Math.Clamp(Math.Floor(value + 0.5), -32768, 32767);
}

/// <summary>
/// Device natural frame -> held (screen) frame.
///
/// Every platform that can rotate its display has this problem: sensors are
/// reported in the device's manufactured orientation, which is not the
/// orientation the user is holding. A tablet held sideways to play, or a handheld
/// whose natural orientation is already landscape, would otherwise send pitch as
/// roll and aim would be rotated 90 degrees.
///
/// Rotation is expressed in DEGREES, which is a physical quantity rather than any
/// platform's rotation constant. A backend converts its own constant (a Win32
/// <c>DMDO_*</c>, an Android <c>Surface.ROTATION_90</c>, a Wayland transform) to
/// degrees and calls this. That keeps the shared layer from ever needing to know
/// how a given OS enumerates display rotation.
///
/// Pure and integer-only so it is exhaustively unit-testable, and applied to
/// counts rather than SI values so it is an exact sign/axis permutation with no
/// second rounding step.
/// </summary>
public static class ScreenOrientation
{
    public static int RemapX(int x, int y, int rotationDegrees) => Normalize(rotationDegrees) switch
    {
        90 => -y,
        180 => -x,
        270 => y,
        _ => x,
    };

    public static int RemapY(int x, int y, int rotationDegrees) => Normalize(rotationDegrees) switch
    {
        90 => x,
        180 => -y,
        270 => -x,
        _ => y,
    };

    /// <summary>Z is the screen normal and is unchanged by any screen rotation.</summary>
    public static int RemapZ(int z) => z;

    /// <summary>Apply the full remap to one canonical sample.</summary>
    public static ControllerMotion Apply(ControllerMotion sample, int rotationDegrees) => sample with
    {
        GyroX = RemapX(sample.GyroX, sample.GyroY, rotationDegrees),
        GyroY = RemapY(sample.GyroX, sample.GyroY, rotationDegrees),
        GyroZ = RemapZ(sample.GyroZ),
        AccelX = RemapX(sample.AccelX, sample.AccelY, rotationDegrees),
        AccelY = RemapY(sample.AccelX, sample.AccelY, rotationDegrees),
        AccelZ = RemapZ(sample.AccelZ),
    };

    private static int Normalize(int degrees)
    {
        var wrapped = ((degrees % 360) + 360) % 360;
        return wrapped is 90 or 180 or 270 ? wrapped : 0;
    }
}
