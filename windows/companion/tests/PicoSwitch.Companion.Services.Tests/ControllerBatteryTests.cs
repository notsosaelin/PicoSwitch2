using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The selected controller's battery, forwarded to the console.
///
/// Phase 6 task (e). The wire contract already reserved the field
/// (<c>ControllerReportEncoder.OffBattery</c> and <c>FlagBatteryValid</c>) but
/// nothing on Windows filled it, so every PC-driven session told the console it
/// had no idea — where Android has reported a real level since its own bridge
/// shipped.
///
/// The interesting behaviour is not the number, it is when we refuse to give one.
/// </summary>
public sealed class ControllerBatteryTests
{
    private static readonly ControllerSourceIdentity Pad =
        new("pad", "Test Pad", 0x045E, 0x02FF);

    [Fact]
    public void AReadingReachesThePublishedState()
    {
        var session = new ControllerInputSession();
        session.ApplyBattery(new ControllerBattery(64, Charging: false, Valid: true));

        var battery = session.Snapshot.Battery;
        Assert.True(battery.Valid);
        Assert.Equal(64, battery.LevelPercent);
        Assert.False(battery.Charging);
    }

    [Fact]
    public void ChargingIsCarriedSeparatelyFromLevel()
    {
        var session = new ControllerInputSession();
        session.ApplyBattery(new ControllerBattery(12, Charging: true, Valid: true));

        Assert.True(session.Snapshot.Battery.Charging);
        Assert.Equal(12, session.Snapshot.Battery.LevelPercent);
    }

    [Fact]
    public void NeutralizeDoesNotBlankTheBattery()
    {
        // Neutralize exists to stop the console acting on input nobody is giving
        // it. A charge level is not input, and clearing it on every watchdog fire
        // or authority switch would make the console's indicator flicker for
        // reasons the player cannot see or explain.
        var session = new ControllerInputSession();
        session.ApplyBattery(new ControllerBattery(80, Charging: false, Valid: true));

        session.Neutralize();

        Assert.True(session.Snapshot.Battery.Valid);
        Assert.Equal(80, session.Snapshot.Battery.LevelPercent);
    }

    [Fact]
    public void ADifferentControllerDoesNotInheritTheLastOnesCharge()
    {
        // A stale level outlives the device it described, and "80%" attached to a
        // controller that never reported is a confident lie.
        var session = new ControllerInputSession();
        session.ApplyBattery(new ControllerBattery(80, Charging: false, Valid: true));

        session.ApplyPhysicalFrame(Pad, ControllerButtonSet.Empty, AnalogFrame.Neutral);

        Assert.False(session.Snapshot.Battery.Valid);
        Assert.Equal(0, session.Snapshot.Battery.LevelPercent);
    }

    [Fact]
    public void UnknownIsNotZeroPercent()
    {
        // The distinction the Valid flag exists for. A wired pad has no battery
        // and several wireless ones decline to say; a console showing a confident
        // 0% is worse than one showing nothing, so the encoder must be able to
        // tell "empty" from "no idea".
        var session = new ControllerInputSession();

        var battery = session.Snapshot.Battery;
        Assert.False(battery.Valid);

        var payload = ControllerReportEncoder.Encode(session.Snapshot);
        Assert.Equal(0, payload[ControllerReportEncoder.OffBattery]);
        Assert.Equal(
            0,
            payload[ControllerReportEncoder.OffFlags] & ControllerReportEncoder.FlagBatteryValid);
    }

    [Fact]
    public void AKnownReadingSetsTheValidFlagOnTheWire()
    {
        var session = new ControllerInputSession();
        session.ApplyBattery(new ControllerBattery(55, Charging: false, Valid: true));

        var payload = ControllerReportEncoder.Encode(session.Snapshot);
        Assert.Equal(55, payload[ControllerReportEncoder.OffBattery]);
        Assert.Equal(
            ControllerReportEncoder.FlagBatteryValid,
            payload[ControllerReportEncoder.OffFlags] & ControllerReportEncoder.FlagBatteryValid);
    }
}
