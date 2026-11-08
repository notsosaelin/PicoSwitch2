using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.Input;
using PicoSwitch.Companion.Platform.Input;
using SDL;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// SDL gamepad state -> bridge state.
///
/// The motion rows are pinned the way CLAUDE.md requires for any motion seam:
/// by a gravity anchor and by the SENSE of each rotation, not merely by the
/// transform having a determinant of +1.
/// </summary>
public sealed class SdlGamepadMappingTests
{
    private const float G = 9.80665f;

    private static ControllerMotion Motion(float[] accel, float[] gyro, ulong nanos = 0) =>
        SdlGamepadMapping.Motion(accel, gyro, nanos);

    [Fact]
    public void APadLyingFlatReadsOneGOutOfItsFace()
    {
        // SDL: flat on a table reads +g on Y (up, out of the face).
        var motion = Motion([0f, G, 0f], [0f, 0f, 0f]);

        Assert.Equal(0, motion.AccelX);
        Assert.Equal(0, motion.AccelY);
        Assert.Equal(8192, motion.AccelZ);
        Assert.True(motion.Valid);
    }

    [Fact]
    public void TiltingTheFarEdgeUpPutsGravityOnCanonicalMinusY()
    {
        // Stood on its near edge with the face toward the player: gravity's
        // reaction points from the near edge to the far edge (SDL -Z). In the
        // canonical frame that is "toward the top of the screen", +Y.
        var motion = Motion([0f, 0f, -G], [0f, 0f, 0f]);

        Assert.Equal(8192, motion.AccelY);
        Assert.Equal(0, motion.AccelZ);
    }

    [Fact]
    public void RightSideDownPutsGravityOnPlusX()
    {
        // Rolled so the right grip points at the floor: the reaction is toward
        // the left... SDL reports -X; canonical X is unchanged.
        var motion = Motion([-G, 0f, 0f], [0f, 0f, 0f]);
        Assert.Equal(-8192, motion.AccelX);
    }

    [Fact]
    public void YawingLeftIsPositiveRotationAboutTheFaceNormal()
    {
        // Counter-clockwise seen from above (turning left) is +SDL Y, the face
        // normal, which is canonical +Z.
        var rate = (float)(Math.PI / 2); // 90 deg/s
        var motion = Motion([0f, G, 0f], [0f, rate, 0f]);

        Assert.Equal(0, motion.GyroX);
        Assert.Equal(0, motion.GyroY);
        Assert.Equal(MotionScale.GyroCounts(rate), motion.GyroZ);
        Assert.True(motion.GyroZ > 0);
    }

    [Fact]
    public void PitchingTheFarEdgeUpIsPositiveRotationAboutX()
    {
        var rate = 1.0f;
        var motion = Motion([0f, G, 0f], [rate, 0f, 0f]);
        Assert.Equal(MotionScale.GyroCounts(rate), motion.GyroX);
    }

    [Fact]
    public void RollAboutTheLongAxisChangesSignWithTheAxis()
    {
        // SDL +Z points at the player; canonical +Y points away. The same
        // physical roll is therefore opposite in sign.
        var motion = Motion([0f, G, 0f], [0f, 0f, 1.0f]);
        Assert.Equal(-MotionScale.GyroCounts(1.0f), motion.GyroY);
    }

    [Fact]
    public void TheTransformIsAProperRotation()
    {
        // Columns of the SDL -> canonical matrix, read off the mapping itself.
        int[] Column(float[] axis)
        {
            var m = Motion(axis, axis);
            return [Math.Sign(m.AccelX), Math.Sign(m.AccelY), Math.Sign(m.AccelZ)];
        }

        var x = Column([1f, 0f, 0f]);
        var y = Column([0f, 1f, 0f]);
        var z = Column([0f, 0f, 1f]);
        var determinant =
            x[0] * (y[1] * z[2] - y[2] * z[1]) -
            y[0] * (x[1] * z[2] - x[2] * z[1]) +
            z[0] * (x[1] * y[2] - x[2] * y[1]);
        Assert.Equal(1, determinant);
    }

    [Fact]
    public void TheTimestampIsTheSensorClockIn100MicrosecondTicks()
    {
        Assert.Equal(1, Motion([0, G, 0], [0, 0, 0], nanos: 100_000).TimestampTicks);

        // Truncated to the 16-bit wire field; the firmware handles the wrap.
        Assert.Equal(0, Motion([0, G, 0], [0, 0, 0], nanos: 65_536UL * 100_000).TimestampTicks);
    }

    [Fact]
    public void FaceButtonsArePositional()
    {
        var south = SdlGamepadMapping.Buttons(b => b == SDL_GamepadButton.SDL_GAMEPAD_BUTTON_SOUTH);
        var east = SdlGamepadMapping.Buttons(b => b == SDL_GamepadButton.SDL_GAMEPAD_BUTTON_EAST);

        Assert.Equal(ControllerButtonSet.Empty.With(ControllerButton.A, true), south);
        Assert.Equal(ControllerButtonSet.Empty.With(ControllerButton.B, true), east);
    }

    [Theory]
    [InlineData(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_GUIDE, ControllerButton.Home)]
    [InlineData(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_MISC1, ControllerButton.Capture)]
    [InlineData(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, ControllerButton.GL)]
    [InlineData(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, ControllerButton.GR)]
    [InlineData(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_BACK, ControllerButton.Select)]
    public void ExtraControlsReachTheirConsoleCounterparts(SDL_GamepadButton sdl, ControllerButton expected) =>
        Assert.Equal(ControllerButtonSet.Empty.With(expected, true), SdlGamepadMapping.Buttons(b => b == sdl));

    [Fact]
    public void SticksAreNotInvertedBecauseSdlIsAlreadyYDown()
    {
        var up = SdlGamepadMapping.Analog(
            axis => axis == SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTY ? short.MinValue : (short)0,
            _ => false);

        // Full up is the shared symmetric minimum (128 - 127), not 0.
        Assert.Equal(1, up.LeftY);
        Assert.Equal(128, up.LeftX);
        Assert.Equal(0, up.LeftTrigger);
    }

    [Fact]
    public void TriggersSpanTheFullBridgeRange()
    {
        var pulled = SdlGamepadMapping.Analog(
            axis => axis == SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? short.MaxValue : (short)0,
            _ => false);
        Assert.Equal(255, pulled.RightTrigger);
    }

    [Fact]
    public void TheSteamDeckIsBuiltInAndSteamsVirtualPadIsNotADevice()
    {
        Assert.Equal(ControllerAttachment.BuiltIn, SdlGamepadMapping.Attachment(0x28DE, 0x1205));
        Assert.Equal(ControllerAttachment.External, SdlGamepadMapping.Attachment(0x054C, 0x0CE6));
        Assert.True(SdlGamepadMapping.IsSteamVirtualPad(0x28DE, 0x11FF));
        Assert.False(SdlGamepadMapping.IsSteamVirtualPad(0x28DE, 0x1205));
    }

    [Fact]
    public void OnADeckWithAPadAttachedTheBuiltInControlsWin()
    {
        ControllerSource Source(ushort vendor, ushort product, string name) => new(
            new ControllerCandidate(product, SdlGamepadMapping.Descriptor(vendor, product, null, name), name,
                vendor, product, true, true, SdlGamepadMapping.IsSteamVirtualPad(vendor, product)),
            SdlGamepadMapping.Attachment(vendor, product),
            IsGamepadClass: true);

        var deck = Source(0x28DE, 0x1205, "Steam Deck");
        var steamVirtual = Source(0x28DE, 0x11FF, "Steam Virtual Gamepad");
        var dualSense = Source(0x054C, 0x0CE6, "DualSense Wireless Controller");

        Assert.Same(deck, ControllerSourceSelection.Resolve([steamVirtual, dualSense, deck], null));

        // Steam's duplicate is never a candidate, even when it is all there is.
        Assert.Null(ControllerSourceSelection.Resolve([steamVirtual], null));
        Assert.Same(dualSense, ControllerSourceSelection.Resolve([steamVirtual, dualSense], null));
    }

    [Theory]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_ON_BATTERY, 40, 40, false, true)]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_CHARGING, 80, 80, true, true)]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_CHARGED, -1, 100, false, true)]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_ON_BATTERY, -1, 0, false, false)]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_NO_BATTERY, -1, 0, false, false)]
    [InlineData(SDL_PowerState.SDL_POWERSTATE_UNKNOWN, 50, 0, false, false)]
    public void BatteryIsOnlyValidWhenThePadActuallyReportsIt(
        SDL_PowerState state, int percent, int level, bool charging, bool valid)
    {
        var reading = SdlGamepadMapping.Battery(state, percent);
        Assert.Equal(valid, reading.Valid);
        if (valid)
        {
            Assert.Equal(level, reading.LevelPercent);
            Assert.Equal(charging, reading.Charging);
        }
    }

    [Fact]
    public void RumbleUsesTheWholeMotorRange()
    {
        Assert.Equal((ushort)0, SdlGamepadMapping.Motor(0));
        Assert.Equal(ushort.MaxValue, SdlGamepadMapping.Motor(255));
        Assert.Equal(ushort.MaxValue, SdlGamepadMapping.Motor(999));
    }
}
