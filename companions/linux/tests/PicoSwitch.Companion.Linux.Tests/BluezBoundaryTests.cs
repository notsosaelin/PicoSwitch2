using PicoSwitch.Companion.Linux.Bluetooth;
using PicoSwitch.Companion.Platform.Bluetooth;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>BlueZ paths, error classification, pairing and radio, as pure rules.</summary>
public sealed class BluezBoundaryTests
{
    [Fact]
    public void DevicePathsUseBlueZsOwnSpelling()
    {
        Assert.Equal(
            "/org/bluez/hci0/dev_2C_CF_67_A1_B2_C3",
            BluezPaths.DevicePath("/org/bluez/hci0", 0x2CCF67A1B2C3));
    }

    [Theory]
    [InlineData("2C:CF:67:A1:B2:C3", 0x2CCF67A1B2C3UL)]
    [InlineData("2c:cf:67:a1:b2:c3", 0x2CCF67A1B2C3UL)]
    public void AddressesRoundTrip(string text, ulong expected)
    {
        Assert.True(BluezPaths.TryParseAddress(text, out var address));
        Assert.Equal(expected, address);
        Assert.Equal(text.ToUpperInvariant(), BluetoothAddressFormat.ToText(address));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("2C:CF:67:A1:B2")]
    [InlineData("2C:CF:67:A1:B2:ZZ")]
    [InlineData("00:00:00:00:00:00")]
    public void MalformedAddressesAreRejected(string? text) =>
        Assert.False(BluezPaths.TryParseAddress(text, out _));

    [Fact]
    public void OnlyDirectChildrenAreChildren()
    {
        Assert.True(BluezPaths.IsChildOf("/org/bluez/hci0/dev_X", "/org/bluez/hci0"));
        Assert.False(BluezPaths.IsChildOf("/org/bluez/hci0/dev_X/service1", "/org/bluez/hci0"));
        Assert.False(BluezPaths.IsChildOf("/org/bluez/hci01", "/org/bluez/hci0"));
        Assert.False(BluezPaths.IsChildOf("/org/bluez/hci0", "/org/bluez/hci0"));
    }

    [Theory]
    [InlineData(BluezErrors.NotConnected, "", GattCommunicationOutcome.Unreachable)]
    [InlineData(BluezErrors.Failed, "Not connected", GattCommunicationOutcome.Unreachable)]
    [InlineData(BluezErrors.Failed, "le-connection-abort-by-local", GattCommunicationOutcome.Unreachable)]
    [InlineData(BluezErrors.NotAuthorized, "", GattCommunicationOutcome.AccessDenied)]
    [InlineData(BluezErrors.Failed, "Operation failed with ATT error: 0x05", GattCommunicationOutcome.AccessDenied)]
    [InlineData(BluezErrors.Failed, "Operation failed with ATT error: 0x0f", GattCommunicationOutcome.AccessDenied)]
    [InlineData(BluezErrors.Failed, "Operation failed with ATT error: 0x0e", GattCommunicationOutcome.ProtocolError)]
    [InlineData(BluezErrors.NotPermitted, "Write not permitted", GattCommunicationOutcome.ProtocolError)]
    [InlineData(BluezErrors.InProgress, "In Progress", GattCommunicationOutcome.ProtocolError)]
    public void FailuresAreClassifiedForTheSharedRecoveryPolicy(
        string name,
        string message,
        GattCommunicationOutcome expected) =>
        Assert.Equal(expected, BluezErrors.Outcome(new BluezException(name, message)));

    [Fact]
    public void AnUnrecognisedFailureCanNeverManufactureARepairOffer()
    {
        // AccessDenied is the outcome the bond-mismatch signature acts on, so an
        // error this mapping does not understand must not become one.
        var unknown = BluezErrors.Outcome(new BluezException("org.example.Error.Weird", "something new"));
        var trust = new TransportTrustSnapshot(WindowsPaired: true, PeerObserved: true);

        Assert.NotEqual(GattCommunicationOutcome.AccessDenied, unknown);
        Assert.False(AdapterResetSignature.IsBondMismatch(GattFailureStage.Subscribe, unknown, null, trust));
    }

    [Theory]
    [InlineData(BluezErrors.AlreadyExists, "", AdapterPairingOutcome.AlreadyPaired)]
    [InlineData(BluezErrors.AuthenticationCanceled, "", AdapterPairingOutcome.Rejected)]
    [InlineData(BluezErrors.AuthenticationRejected, "", AdapterPairingOutcome.Rejected)]
    [InlineData(BluezErrors.AuthenticationTimeout, "", AdapterPairingOutcome.TimedOut)]
    [InlineData(BluezErrors.ConnectionAttemptFailed, "Page Timeout", AdapterPairingOutcome.TimedOut)]
    [InlineData(BluezErrors.AuthenticationFailed, "", AdapterPairingOutcome.Failed)]
    public void PairingFailuresUseTheSharedVocabulary(string name, string message, AdapterPairingOutcome expected) =>
        Assert.Equal(expected, BluezErrors.PairingOutcome(new BluezException(name, message)));

    [Fact]
    public async Task AnAbsentDeviceIsDefinitivelyNotPaired()
    {
        var bus = new FakeBluezBus { DeviceKnown = false };
        var snapshot = await new BluezPairingGateway(bus).ReadAsync(FakeBluezBus.PicoAddress);

        // BlueZ loads every stored bond as an object at startup, so absence is
        // knowledge, not uncertainty.
        Assert.Equal(HostPairingKnown.NotPaired, snapshot.State);
        Assert.True(snapshot.CanPair);
    }

    [Fact]
    public async Task NoAdapterMeansUnknownNeverAGuess()
    {
        var bus = new FakeBluezBus { Adapter = null };
        var snapshot = await new BluezPairingGateway(bus).ReadAsync(FakeBluezBus.PicoAddress);
        Assert.Equal(HostPairingKnown.Unknown, snapshot.State);
    }

    [Fact]
    public async Task PairingBondsAndTrusts()
    {
        var bus = new FakeBluezBus { Paired = false };
        var result = await new BluezPairingGateway(bus).PairAsync(FakeBluezBus.PicoAddress);

        Assert.True(result.Succeeded);
        Assert.Equal(1, bus.Pairs);
        Assert.True(bus.Trusted);
    }

    [Fact]
    public async Task AnAlreadyPairedAdapterIsNotPairedAgain()
    {
        var bus = new FakeBluezBus { Paired = true };
        var result = await new BluezPairingGateway(bus).PairAsync(FakeBluezBus.PicoAddress);

        Assert.Equal(AdapterPairingOutcome.AlreadyPaired, result.Outcome);
        Assert.Equal(0, bus.Pairs);
    }

    [Fact]
    public async Task AClosedPairingWindowReadsAsATimeout()
    {
        var bus = new FakeBluezBus
        {
            Paired = false,
            PairFailure = new BluezException(BluezErrors.AuthenticationTimeout, "Authentication Timeout"),
        };
        var result = await new BluezPairingGateway(bus).PairAsync(FakeBluezBus.PicoAddress);

        Assert.Equal(AdapterPairingOutcome.TimedOut, result.Outcome);
        Assert.Contains("pairing button", result.Message);
    }

    [Fact]
    public async Task UnpairRemovesTheBondAndReportsWhatActuallyHappened()
    {
        var bus = new FakeBluezBus { Paired = true };
        var gateway = new BluezPairingGateway(bus);

        Assert.Equal(AdapterUnpairResult.Unpaired, await gateway.UnpairAsync(FakeBluezBus.PicoAddress));
        Assert.Equal(1, bus.Removes);

        // Idempotent: nothing left to remove is still "trust removed".
        Assert.Equal(AdapterUnpairResult.AlreadyUnpaired, await gateway.UnpairAsync(FakeBluezBus.PicoAddress));
    }

    [Fact]
    public async Task ARefusedUnpairIsNeverReportedAsARemoval()
    {
        var bus = new FakeBluezBus
        {
            RemoveFailure = new BluezException(BluezErrors.NotAuthorized, "denied"),
        };
        var result = await new BluezPairingGateway(bus).UnpairAsync(FakeBluezBus.PicoAddress);

        Assert.Equal(AdapterUnpairResult.AccessDenied, result);
        Assert.False(result.TrustRemoved());
    }

    [Fact]
    public void TheRadioIsDescribedFromAdapterRoles()
    {
        var full = BluezRadio.Describe(new BluezAdapter("/org/bluez/hci0", "AA:BB:CC:DD:EE:FF", true,
            ["central", "peripheral", "central-peripheral"]));
        Assert.Null(full.ManagementBlockedReason);
        Assert.True(full.PeripheralRoleSupported);
        Assert.Equal("AA:BB:CC:DD:EE:FF", full.LocalAddress);

        var off = BluezRadio.Describe(new BluezAdapter("/org/bluez/hci0", null, false, ["central"]));
        Assert.Equal("Bluetooth is turned off.", off.ManagementBlockedReason);

        // An older BlueZ without Roles is assumed central-capable; the connect
        // itself reports the truth.
        var old = BluezRadio.Describe(new BluezAdapter("/org/bluez/hci0", null, true, null));
        Assert.Null(old.ManagementBlockedReason);
        Assert.False(old.PeripheralRoleSupported);
    }

    [Fact]
    public async Task NoBluetoothDaemonIsNoRadio()
    {
        var capabilities = await BluezRadio.ProbeAsync(new FakeBluezBus { Adapter = null });
        Assert.False(capabilities.RadioPresent);
    }
}
