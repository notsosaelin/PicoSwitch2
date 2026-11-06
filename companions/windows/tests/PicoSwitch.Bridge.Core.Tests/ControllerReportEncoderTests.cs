using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

public sealed class ControllerReportEncoderTests
{
    [Fact]
    public void PayloadLengthsMatchTheDeclaredContract()
    {
        Assert.Equal(9, ControllerReportEncoder.EncodeV1(ControllerState.Neutral).Length);
        Assert.Equal(26, ControllerReportEncoder.Encode(ControllerState.Neutral).Length);
        Assert.Equal(ControllerReportEncoder.PayloadSize, 9);
        Assert.Equal(ControllerReportEncoder.PayloadSizeV2, 26);
    }

    [Fact]
    public void NeutralIsCenteredSticksRestingTriggersAndANeutralHat()
    {
        var report = ControllerReportEncoder.Encode(ControllerState.Neutral);
        Assert.Equal([0x80, 0x80, 0x80, 0x80, 0x00, 0x00], report[..6]);
        Assert.Equal(0x00, report[6]);
        Assert.Equal(0x00, report[7]);
        Assert.Equal(0x00, report[8]);
        Assert.Equal(8, report[9]);
    }

    [Fact]
    public void EveryButtonSetsExactlyItsOwnBit()
    {
        foreach (var button in Enum.GetValues<ControllerButton>())
        {
            var report = ControllerReportEncoder.Encode(
                ControllerState.Neutral with { Buttons = ControllerButtonSet.Of(button) });
            var bits = report[6] | (report[7] << 8) | (report[8] << 16);
            Assert.Equal(1 << (int)button, bits);
        }
    }

    [Fact]
    public void TheThirdButtonByteCarriesOnlyGrAndKeepsItsPaddingClear()
    {
        // Bit 16 is GR, the seventeenth and last button; the remaining seven bits
        // of that byte are the descriptor's padding.
        var report = ControllerReportEncoder.Encode(
            ControllerState.Neutral with
            {
                Buttons = ControllerButtonSet.Of(Enum.GetValues<ControllerButton>()),
            });
        Assert.Equal(0x01, report[8]);
    }

    [Fact]
    public void V1KeepsCGameChatButHasNoRoomForTheGripButtons()
    {
        var all = ControllerState.Neutral with
        {
            Buttons = ControllerButtonSet.Of(Enum.GetValues<ControllerButton>()),
        };
        var report = ControllerReportEncoder.EncodeV1(all);

        // 0x7F, not 0x3F: bit 14 is C / GameChat, the fifteenth button. GL and GR
        // simply do not exist in the v1 layout.
        Assert.Equal(0xFF, report[6]);
        Assert.Equal(0x7F, report[7]);
    }

    [Theory]
    [InlineData(false, false, false, false, 8)]
    [InlineData(true, false, false, false, 0)]
    [InlineData(true, true, false, false, 1)]
    [InlineData(false, true, false, false, 2)]
    [InlineData(false, true, true, false, 3)]
    [InlineData(false, false, true, false, 4)]
    [InlineData(false, false, true, true, 5)]
    [InlineData(false, false, false, true, 6)]
    [InlineData(true, false, false, true, 7)]
    public void EveryHatDirectionEncodesToItsCode(
        bool up,
        bool right,
        bool down,
        bool left,
        int expected)
    {
        var state = ControllerState.Neutral with
        {
            DpadUp = up, DpadRight = right, DpadDown = down, DpadLeft = left,
        };
        Assert.Equal(expected, ControllerReportEncoder.Hat(state));
    }

    [Fact]
    public void OppositeDirectionsCancelRatherThanPickingOne()
    {
        // Four retained directions exist precisely so that releasing one side
        // restores the still-held side without inventing an edge.
        var vertical = ControllerState.Neutral with { DpadUp = true, DpadDown = true };
        Assert.Equal(8, ControllerReportEncoder.Hat(vertical));

        var horizontal = ControllerState.Neutral with { DpadLeft = true, DpadRight = true };
        Assert.Equal(8, ControllerReportEncoder.Hat(horizontal));

        var all = ControllerState.Neutral with
        {
            DpadUp = true, DpadDown = true, DpadLeft = true, DpadRight = true,
        };
        Assert.Equal(8, ControllerReportEncoder.Hat(all));

        // A cancelled horizontal pair still leaves the vertical press readable.
        var upWithCancel = ControllerState.Neutral with
        {
            DpadUp = true, DpadLeft = true, DpadRight = true,
        };
        Assert.Equal(0, ControllerReportEncoder.Hat(upWithCancel));
    }

    [Fact]
    public void AxesAreClampedIntoTheWireRange()
    {
        var report = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            LeftX = -5, LeftY = 300, RightX = int.MinValue, RightY = int.MaxValue,
            LeftTrigger = -1, RightTrigger = 999,
        });
        Assert.Equal([0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF], report[..6]);
    }

    [Fact]
    public void MotionIsWrittenOnlyWhenTheSampleIsValid()
    {
        var sample = new ControllerMotion(1, 2, 3, 4, 5, 6, 0x1234, Valid: true);
        var valid = ControllerReportEncoder.Encode(ControllerState.Neutral with { Motion = sample });
        Assert.Equal(0x01, valid[10]);
        Assert.Equal(0x34, valid[24]);
        Assert.Equal(0x12, valid[25]);
        Assert.Equal(ControllerReportEncoder.FlagMotionValid, valid[23]);

        // An invalid sample must not latch the last values: when the console is not
        // consuming motion the firmware stops publishing it entirely.
        var invalid = ControllerReportEncoder.Encode(
            ControllerState.Neutral with { Motion = sample with { Valid = false } });
        Assert.All(invalid[10..22], value => Assert.Equal(0, value));
        Assert.Equal(0, invalid[23]);
        Assert.Equal([0, 0], invalid[24..26]);
    }

    [Fact]
    public void MotionValuesAreLittleEndianAndSigned()
    {
        var report = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Motion = new ControllerMotion(-1, short.MinValue, short.MaxValue, 0, 0, 0, 0, Valid: true),
        });
        Assert.Equal([0xFF, 0xFF], report[10..12]);
        Assert.Equal([0x00, 0x80], report[12..14]);
        Assert.Equal([0xFF, 0x7F], report[14..16]);
    }

    [Fact]
    public void TheMotionTimestampIsTruncatedToItsSixteenBitField()
    {
        var report = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Motion = new ControllerMotion(TimestampTicks: 0x1_2345, Valid: true),
        });
        Assert.Equal([0x45, 0x23], report[24..26]);
    }

    [Fact]
    public void BatteryLevelIsAlwaysWrittenButOnlyFlaggedWhenValid()
    {
        var invalid = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Battery = new ControllerBattery(77, Charging: true, Valid: false),
        });
        Assert.Equal(77, invalid[22]);

        // Charging is meaningless without a valid reading, so it is gated on it:
        // the console must not show a charging icon for a level nobody vouches for.
        Assert.Equal(0, invalid[23]);

        var valid = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Battery = new ControllerBattery(77, Charging: true, Valid: true),
        });
        Assert.Equal(
            ControllerReportEncoder.FlagBatteryValid | ControllerReportEncoder.FlagCharging,
            valid[23]);
    }

    [Fact]
    public void BatteryPercentIsClampedToARealPercentage()
    {
        var high = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Battery = new ControllerBattery(150, Valid: true),
        });
        Assert.Equal(100, high[22]);

        var low = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Battery = new ControllerBattery(-20, Valid: true),
        });
        Assert.Equal(0, low[22]);
    }

    [Fact]
    public void MotionAndBatteryFlagsAreIndependent()
    {
        var both = ControllerReportEncoder.Encode(ControllerState.Neutral with
        {
            Motion = new ControllerMotion(Valid: true),
            Battery = new ControllerBattery(50, Valid: true),
        });
        Assert.Equal(
            ControllerReportEncoder.FlagMotionValid | ControllerReportEncoder.FlagBatteryValid,
            both[23]);
    }
}
