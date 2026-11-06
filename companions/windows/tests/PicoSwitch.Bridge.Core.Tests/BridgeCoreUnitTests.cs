using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

public sealed class BridgeOutputCodecTests
{
    [Fact]
    public void ABareBodyDecodes()
    {
        var output = BridgeOutputCodec.Decode([0x10, 0x20, 3, 0x01]);
        Assert.NotNull(output);
        Assert.Equal(new RumbleRequest(0x10, 0x20), output!.Value.Rumble);
        Assert.Equal(3, output.Value.PlayerIndicator);
        Assert.True(output.Value.MotionRequested);
    }

    [Fact]
    public void AnEmbeddedReportIdIsToleratedBecauseStacksDisagreeAboutIt()
    {
        // HID hosts legitimately deliver an output report on the interrupt channel
        // or as a control-channel SET_REPORT, and stacks differ on whether the id
        // is in the payload. Both framings carry the same five bytes.
        var withId = BridgeOutputCodec.Decode([0x02, 0x10, 0x20, 3, 0x01]);
        var without = BridgeOutputCodec.Decode([0x10, 0x20, 3, 0x01]);
        Assert.Equal(without, withId);
    }

    [Fact]
    public void AFiveByteBodyThatDoesNotStartWithTheReportIdIsNotStripped()
    {
        // 0x01 is a plausible rumble amplitude, not a report id in this position.
        var output = BridgeOutputCodec.Decode([0x01, 0x10, 0x20, 3, 0x01]);
        Assert.Equal(new RumbleRequest(0x01, 0x10), output!.Value.Rumble);
    }

    [Fact]
    public void AnythingThatCannotBeABridgeOutputReportIsRejected()
    {
        // A stray report must never be applied as rumble.
        Assert.Null(BridgeOutputCodec.Decode(null));
        Assert.Null(BridgeOutputCodec.Decode([]));
        Assert.Null(BridgeOutputCodec.Decode([0x10, 0x20, 3]));
        Assert.Null(BridgeOutputCodec.Decode([0x10, 0x20, 3, 0x01], reportId: 1));
        Assert.Null(BridgeOutputCodec.Decode([0x02, 0x10, 0x20, 3, 0x01], reportId: 99));
    }

    [Fact]
    public void AMatchingReportIdArgumentIsAccepted()
    {
        Assert.NotNull(BridgeOutputCodec.Decode(
            [0x10, 0x20, 3, 0x01],
            reportId: ControllerReportEncoder.OutputReportId));
    }

    [Fact]
    public void OnlyTheMotionBitOfTheFlagByteIsRead()
    {
        Assert.False(BridgeOutputCodec.Decode([0, 0, 0, 0xFE])!.Value.MotionRequested);
        Assert.True(BridgeOutputCodec.Decode([0, 0, 0, 0xFF])!.Value.MotionRequested);
    }

    [Fact]
    public void ALongerPayloadStillDecodesItsLeadingBody()
    {
        // Trailing bytes a future contract adds must not make a current report
        // undecodable.
        var output = BridgeOutputCodec.Decode([0x10, 0x20, 3, 0x01, 0xAA, 0xBB]);
        Assert.Equal(new RumbleRequest(0x10, 0x20), output!.Value.Rumble);
    }
}

public sealed class RumbleShapingTests
{
    [Fact]
    public void BelowTheGateTheActuatorIsSilenced() =>
        Assert.Equal(0, RumbleShaping.Shape(RumbleShaping.GateOff, previous: 0));

    [Fact]
    public void TheGateHasHysteresisSoAValueOnTheBoundaryCannotChatter()
    {
        // Between GateOff and GateOn, hold whatever we were already doing.
        const int between = (RumbleShaping.GateOff + RumbleShaping.GateOn) / 2;
        Assert.Equal(0, RumbleShaping.Shape(between, previous: 0));
        Assert.NotEqual(0, RumbleShaping.Shape(between, previous: 32));
    }

    [Fact]
    public void FullScaleStaysFullScale() =>
        // Flooring would cap the console's hardest rumble at 240/255.
        Assert.Equal(255, RumbleShaping.Shape(255, previous: 255));

    [Fact]
    public void QuantisationRoundsToNearestRatherThanDown()
    {
        // Rounding down would systematically under-drive the actuator: 40 sits
        // exactly between two steps and must land on the higher one.
        Assert.Equal(32, RumbleShaping.Shape(39, previous: 0));
        Assert.Equal(48, RumbleShaping.Shape(40, previous: 0));
        Assert.Equal(48, RumbleShaping.Shape(47, previous: 0));
    }

    [Fact]
    public void AnAudibleButImperceptibleAmplitudeIsNeverDrivenBelowTheStartThreshold()
    {
        var shaped = RumbleShaping.Shape(RumbleShaping.GateOn, previous: 0);
        Assert.True(shaped >= RumbleShaping.GateOn);
    }

    [Fact]
    public void InputIsClampedBeforeItIsShaped()
    {
        Assert.Equal(0, RumbleShaping.Shape(-100, previous: 128));
        Assert.Equal(255, RumbleShaping.Shape(1000, previous: 0));
    }

    [Fact]
    public void RumbleRequestCollapsesToTheStrongerMotorForSingleActuatorHosts()
    {
        Assert.Equal(200, new RumbleRequest(50, 200).Strongest);
        Assert.True(RumbleRequest.None.Silent);
        Assert.False(new RumbleRequest(0, 1).Silent);
    }
}

public sealed class MotionConventionTests
{
    [Fact]
    public void AccelerationScalesAtTheDocumentedCountsPerG()
    {
        // 1 g of proper acceleration is 8192 counts, gravity-inclusive.
        Assert.Equal(8192, MotionScale.AccelCounts((float)MotionConvention.GravityMs2));
        Assert.Equal(0, MotionScale.AccelCounts(0f));
        Assert.Equal(-8192, MotionScale.AccelCounts((float)-MotionConvention.GravityMs2));
    }

    [Fact]
    public void AngularRateScalesAtTheDocumentedCountsPerDegreePerSecond()
    {
        // 1 rad/s is 57.29578 deg/s, and 16.384 counts per deg/s.
        var expected = (int)Math.Floor((57.2957795 * 16.384) + 0.5);
        Assert.Equal(expected, MotionScale.GyroCounts(1f));
    }

    [Fact]
    public void ScalingClampsToTheSignedSixteenBitWireField()
    {
        Assert.Equal(32767, MotionScale.AccelCounts(1_000_000f));
        Assert.Equal(-32768, MotionScale.AccelCounts(-1_000_000f));
        Assert.Equal(32767, MotionScale.GyroCounts(1_000_000f));
        Assert.Equal(-32768, MotionScale.GyroCounts(-1_000_000f));
    }

    [Fact]
    public void RoundingIsHalfUpToMatchTheKotlinImplementation()
    {
        // Kotlin's roundToInt() is floor(x + 0.5). .NET's default midpoint rule is
        // not, and the two disagree on negative midpoints -- a one-count divergence
        // that only shows up as slightly different aim.
        Assert.Equal(1, MotionScale.Clamp16(0.5));
        Assert.Equal(0, MotionScale.Clamp16(-0.5));
        Assert.Equal(-1, MotionScale.Clamp16(-1.5));
        Assert.Equal(2, MotionScale.Clamp16(1.5));
    }

    [Fact]
    public void TimestampsAreTruncatedFromANanosecondSensorClock()
    {
        Assert.Equal(0, MotionScale.TimestampTicks(0));
        Assert.Equal(1, MotionScale.TimestampTicks(MotionConvention.TimestampNanosPerTick));

        // The field is 16 bits and wraps every 6.5536 s; the firmware takes the
        // delta in the field's own modulus.
        Assert.Equal(
            0,
            MotionScale.TimestampTicks(65536L * MotionConvention.TimestampNanosPerTick));
        Assert.Equal(
            1,
            MotionScale.TimestampTicks(65537L * MotionConvention.TimestampNanosPerTick));
    }

    [Fact]
    public void TheTickRateIsOneHundredMicroseconds()
    {
        Assert.Equal(10_000L, MotionConvention.TimestampTicksPerSecond);
        Assert.Equal(100_000L, MotionConvention.TimestampNanosPerTick);
    }
}

public sealed class ScreenOrientationTests
{
    private static readonly ControllerMotion Sample =
        new(GyroX: 10, GyroY: 20, GyroZ: 30, AccelX: 40, AccelY: 50, AccelZ: 60, Valid: true);

    [Fact]
    public void ZeroDegreesIsTheIdentity() =>
        Assert.Equal(Sample, ScreenOrientation.Apply(Sample, 0));

    [Fact]
    public void NinetyDegreesRotatesTheScreenPlaneAndLeavesTheNormalAlone()
    {
        var rotated = ScreenOrientation.Apply(Sample, 90);
        Assert.Equal(-20, rotated.GyroX);
        Assert.Equal(10, rotated.GyroY);
        Assert.Equal(30, rotated.GyroZ);
        Assert.Equal(-50, rotated.AccelX);
        Assert.Equal(40, rotated.AccelY);
        Assert.Equal(60, rotated.AccelZ);
    }

    [Fact]
    public void OneHundredEightyDegreesInvertsThePlane()
    {
        var rotated = ScreenOrientation.Apply(Sample, 180);
        Assert.Equal(-10, rotated.GyroX);
        Assert.Equal(-20, rotated.GyroY);
        Assert.Equal(30, rotated.GyroZ);
    }

    [Fact]
    public void FourRotationsReturnToTheOriginalSample()
    {
        var cycled = Sample;
        for (var step = 0; step < 4; step++)
        {
            cycled = ScreenOrientation.Apply(cycled, 90);
        }

        Assert.Equal(Sample, cycled);
    }

    [Fact]
    public void UnrecognisedAnglesNormalizeToTheNearestSupportedQuarterTurn()
    {
        // A backend converts its own platform rotation constant to degrees; a value
        // that is not a quarter turn is not a rotation this convention describes.
        Assert.Equal(ScreenOrientation.Apply(Sample, 90), ScreenOrientation.Apply(Sample, 450));
        Assert.Equal(ScreenOrientation.Apply(Sample, 270), ScreenOrientation.Apply(Sample, -90));
        Assert.Equal(Sample, ScreenOrientation.Apply(Sample, 45));
    }

    [Fact]
    public void ValidityAndTimestampSurviveTheRemap()
    {
        var rotated = ScreenOrientation.Apply(Sample with { TimestampTicks = 1234 }, 270);
        Assert.True(rotated.Valid);
        Assert.Equal(1234, rotated.TimestampTicks);
    }
}

public sealed class AxisRangeTests
{
    private static readonly AxisRange Range = new(-1f, 1f);

    [Fact]
    public void ARestingAxisIsNeutralAndAFullDeflectionIsFullScale()
    {
        Assert.Equal(128, Range.Stick(0f));
        Assert.Equal(255, Range.Stick(1f));
        Assert.Equal(1, Range.Stick(-1f));
    }

    [Fact]
    public void AMinimumDeadZoneIsAppliedEvenWhenThePlatformReportsNone()
    {
        // Platforms that report flat = 0 still have noisy centres.
        Assert.Equal(128, Range.Stick(0.03f));
        Assert.NotEqual(128, Range.Stick(0.2f));
    }

    [Fact]
    public void TheReportedManufacturerDeadZoneIsHonoured()
    {
        var wide = new AxisRange(-1f, 1f, Flat: 0.5f);
        Assert.Equal(128, wide.Stick(0.4f));
        Assert.NotEqual(128, wide.Stick(0.9f));
    }

    [Fact]
    public void InversionHappensAfterNormalizationNotBefore()
    {
        Assert.Equal(Range.Stick(1f), Range.Stick(-1f, invert: true));
        Assert.Equal(128, Range.Stick(0f, invert: true));
    }

    [Fact]
    public void OutOfRangeInputIsClampedRatherThanWrapped()
    {
        Assert.Equal(255, Range.Stick(5f));
        Assert.Equal(1, Range.Stick(-5f));
    }

    [Fact]
    public void TriggersRestAtZeroAndSaturateAtFullScale()
    {
        var trigger = new AxisRange(0f, 1f);
        Assert.Equal(0, trigger.Trigger(0f));
        Assert.Equal(255, trigger.Trigger(1f));
        Assert.Equal(128, trigger.Trigger(0.5f));
        Assert.Equal(0, trigger.Trigger(-1f));
        Assert.Equal(255, trigger.Trigger(2f));
    }

    [Fact]
    public void ADegenerateRangeDoesNotDivideByZero()
    {
        var degenerate = new AxisRange(0f, 0f);
        Assert.Equal(128, degenerate.Stick(0f));
        Assert.Equal(0, degenerate.Trigger(0f));
    }
}

public sealed class BridgeCountersTests
{
    [Fact]
    public void NoTrafficAtAllIsNotADivergence() =>
        Assert.Null(new BridgeCounters().FirstDivergence());

    [Fact]
    public void AnAdapterThatNeverSendsFeedbackIsNamedAsSuch()
    {
        var counters = new BridgeCounters();
        counters.ReportsSent.Set(500);
        var divergence = counters.FirstDivergence();
        Assert.NotNull(divergence);
        Assert.Contains("no HID output callbacks at all", divergence);
        Assert.Contains("did not recognize this bridge", divergence);
    }

    [Fact]
    public void TheFirstStageThatProducedNothingIsTheOneReported()
    {
        var counters = new BridgeCounters();
        counters.TransportOutputCallbacks.Set(10);
        Assert.Contains("output frames decoded is 0", counters.FirstDivergence());

        counters.OutputFramesDecoded.Set(10);
        Assert.Contains("session applied output is 0", counters.FirstDivergence());

        counters.SessionOutputApplied.Set(10);
        Assert.Null(counters.FirstDivergence());
    }

    [Fact]
    public void TheSnapshotRendersEveryBoundaryInReadingOrder()
    {
        var counters = new BridgeCounters();
        counters.ReportsSent.Increment();
        var snapshot = counters.Snapshot();
        foreach (var label in new[]
                 {
                     "cb=", "dec=", "rej=", "applied=", "motionGate=", "rumble=",
                     "sent=", "motionBlk=", "battBlk=", "imuSamples=", "battSamples=",
                 })
        {
            Assert.Contains(label, snapshot);
        }

        Assert.Contains("sent=1", snapshot);
    }

    [Fact]
    public void ResetClearsEveryCounter()
    {
        var counters = new BridgeCounters();
        counters.TransportOutputCallbacks.Increment();
        counters.ReportsSent.Increment();
        counters.Reset();
        Assert.Equal(0, counters.TransportOutputCallbacks.Value);
        Assert.Equal(0, counters.ReportsSent.Value);
        Assert.Null(counters.FirstDivergence());
    }
}
