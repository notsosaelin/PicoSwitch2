using PicoSwitch.Companion.Linux.Bluetooth;
using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// The companion audio planes over BlueZ: AUDIO-DOWN notifications and MIC-UP
/// write-without-response, on the management link the transport already owns.
/// The service above them is shared with Windows and tested there; these pin
/// the BlueZ edge (which characteristic, which write type, what a missing plane
/// or a failed write reports).
/// </summary>
public sealed class BluezCompanionAudioTests
{
    private static readonly string Address = BluetoothAddressFormat.ToText(FakeBluezBus.PicoAddress)!;

    private static async Task<BluezManagementTransport> ConnectedAsync(FakeBluezBus bus)
    {
        var transport = new BluezManagementTransport(bus, () => Task.FromResult(BluezRadio.Describe(bus.Adapter!)));
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });
        await transport.ConnectKnownAsync(Address);
        return transport;
    }

    private static async Task<bool> WaitForAsync(Func<bool> condition)
    {
        for (var i = 0; i < 200 && !condition(); i++)
        {
            await Task.Delay(10);
        }

        return condition();
    }

    [Fact]
    public async Task NoPlaneIsOfferedWithoutALiveLink()
    {
        var bus = new FakeBluezBus();
        await using var transport = new BluezManagementTransport(bus, () => Task.FromResult(BluezRadio.Describe(bus.Adapter!)));

        Assert.Null(transport.TryCreateAudioDownlink());
        Assert.Null(transport.TryCreateMicUplink());
    }

    [Fact]
    public async Task TheDownlinkSubscribesAudioDownAndDeliversEachNotification()
    {
        var bus = new FakeBluezBus();
        await using var transport = await ConnectedAsync(bus);
        var downlink = transport.TryCreateAudioDownlink();
        Assert.NotNull(downlink);

        var frames = new List<byte[]>();
        downlink.FrameReceived += frames.Add;
        Assert.True(await downlink.OpenAsync());
        Assert.True(bus.AudioDownSubscribed);
        Assert.Equal(247, downlink.AttMtu);

        bus.RaiseAudioDown([1, 0x90, 0, 0]);
        bus.RaiseAudioDown([1, 0x90, 1, 0]);

        Assert.Equal(2, frames.Count);
        Assert.Equal(2, downlink.NotificationsReceived);

        // Closing ends the subscription; a late notification is not delivered.
        await downlink.DisposeAsync();
        Assert.False(bus.AudioDownSubscribed);
        bus.RaiseAudioDown([1, 0x90, 2, 0]);
        Assert.Equal(2, frames.Count);
    }

    [Fact]
    public async Task AnAdapterWithoutTheAudioPlanesReportsThemMissing()
    {
        // An older build, or BlueZ still holding the GATT database it cached
        // before the adapter was flashed.
        var bus = new FakeBluezBus { HasCompanionAudio = false };
        await using var transport = await ConnectedAsync(bus);

        await using var downlink = transport.TryCreateAudioDownlink()!;
        await using var uplink = transport.TryCreateMicUplink()!;

        Assert.False(await downlink.OpenAsync());
        Assert.False(await uplink.OpenAsync());
    }

    [Fact]
    public async Task TheMicUplinkWritesWithoutResponseInOrder()
    {
        var bus = new FakeBluezBus();
        await using var transport = await ConnectedAsync(bus);
        await using var uplink = transport.TryCreateMicUplink()!;
        Assert.True(await uplink.OpenAsync());

        for (byte i = 0; i < 4; i++)
        {
            uplink.Send([1, 0x91, i, 0]);
        }

        // The fake throws on a write WITH response to MIC-UP, so four landed
        // writes prove the write type as well as the order.
        Assert.True(await WaitForAsync(() => uplink.Stats.Written == 4));
        Assert.Equal([0, 1, 2, 3], bus.MicWrites.Select(frame => (int)frame[2]));

        Assert.Equal(0, uplink.Stats.Failed);
    }

    [Fact]
    public async Task AFailedMicWriteIsCountedAndNeverRetried()
    {
        var bus = new FakeBluezBus { MicWriteFailure = new BluezException(BluezErrors.Failed, "radio busy") };
        await using var transport = await ConnectedAsync(bus);
        await using var uplink = transport.TryCreateMicUplink()!;
        Assert.True(await uplink.OpenAsync());

        uplink.Send([1, 0x91, 0, 0]);

        Assert.True(await WaitForAsync(() => uplink.Stats.Failed == 1));
        await Task.Delay(50);
        Assert.Equal(1, uplink.Stats.Failed);
        Assert.Equal(0, uplink.Stats.Written);
        Assert.Empty(bus.MicWrites);
    }

    [Fact]
    public async Task AClosedMicUplinkSendsNothing()
    {
        var bus = new FakeBluezBus();
        await using var transport = await ConnectedAsync(bus);
        var uplink = transport.TryCreateMicUplink()!;
        Assert.True(await uplink.OpenAsync());
        await uplink.DisposeAsync();

        uplink.Send([1, 0x91, 0, 0]);
        await Task.Delay(50);

        Assert.Empty(bus.MicWrites);
    }
}
