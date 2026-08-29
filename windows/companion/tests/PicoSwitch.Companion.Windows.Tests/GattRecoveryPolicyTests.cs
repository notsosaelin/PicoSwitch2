using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

public sealed class GattRecoveryPolicyTests
{
    private static GattTransportException Failure(
        GattFailureStage stage,
        GattCommunicationOutcome? outcome = null,
        int? hresult = null) =>
        new("failed", stage, outcome, protocolError: null, hresult: hresult);

    [Fact]
    public void OnlyTransientConnectStageConditionsAreWorthACleanRetry()
    {
        Assert.True(GattRecoveryPolicy.IsRetryable(
            Failure(GattFailureStage.Connect, GattCommunicationOutcome.Unreachable)));
        Assert.True(GattRecoveryPolicy.IsRetryable(
            Failure(GattFailureStage.Connect, hresult: GattStatusFormatter.EDeviceNotAvailable)));
        Assert.True(GattRecoveryPolicy.IsRetryable(
            Failure(GattFailureStage.Connect, hresult: GattStatusFormatter.RpcSServerUnavailable)));
    }

    [Fact]
    public void TheBondMismatchShapeIsNeverRetried()
    {
        // Retrying it is what produced six futile attempts over fourteen minutes on
        // the Android side before the OS dropped its own bond.
        Assert.False(GattRecoveryPolicy.IsRetryable(
            Failure(GattFailureStage.Connect, GattCommunicationOutcome.AccessDenied)));
        Assert.False(GattRecoveryPolicy.IsRetryable(Failure(
            GattFailureStage.Connect,
            hresult: GattStatusFormatter.EBluetoothAttInsufficientAuthentication)));
    }

    [Fact]
    public void AFailureAtAnyOtherStageIsNotAConnectRetry()
    {
        foreach (var stage in new[]
                 {
                     GattFailureStage.Services,
                     GattFailureStage.Subscribe,
                     GattFailureStage.Command,
                 })
        {
            Assert.False(GattRecoveryPolicy.IsRetryable(
                Failure(stage, GattCommunicationOutcome.Unreachable)));
        }
    }

    [Fact]
    public void SomethingThatIsNotATransportFailureIsNotRetryable() =>
        Assert.False(GattRecoveryPolicy.IsRetryable(new ManagementException("timed out")));

    [Fact]
    public void ATransportFailureIsFoundThroughAWrapper()
    {
        var wrapped = new ManagementException(
            "wrapped",
            Failure(GattFailureStage.Connect, GattCommunicationOutcome.Unreachable));
        Assert.True(GattRecoveryPolicy.IsRetryable(wrapped));
    }

    [Fact]
    public void TheRetryBudgetIsExactlyOne()
    {
        var retryable = Failure(GattFailureStage.Connect, GattCommunicationOutcome.Unreachable);
        Assert.True(GattRecoveryPolicy.ShouldRetry(retryable, retriesUsed: 0));
        Assert.False(GattRecoveryPolicy.ShouldRetry(retryable, retriesUsed: 1));
        Assert.Equal(1, GattRecoveryPolicy.MaxCleanRetries);
    }
}

/// <summary>
/// The Windows bond-mismatch signature.
///
/// **Confidence: Hypothesis.** Reasoned from the Windows API surface and from the
/// hardware-confirmed Android behaviour, and NOT yet observed against a reflashed
/// adapter on Windows. These tests pin the CONDITION SET so the thing under test
/// is written down; they do not establish that the condition set is right. The
/// exit criterion for that is first-attempt <c>RepairRequired</c> against a
/// genuinely reflashed adapter (WINDOWS_PASS.md §31 Phase 2).
/// </summary>
public sealed class AdapterResetSignatureTests
{
    [Fact]
    public void AllThreeFactsAreRequired()
    {
        // Without a held pairing this is simply "not paired", which is a different
        // message and a different flow.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.AccessDenied,
            hresult: null,
            windowsStillPaired: false,
            peerReachable: true));

        // Without reachability, an out-of-range adapter would be misdiagnosed as
        // reflashed.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.AccessDenied,
            hresult: null,
            windowsStillPaired: true,
            peerReachable: false));

        // With all three, it fires.
        Assert.True(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.AccessDenied,
            hresult: null,
            windowsStillPaired: true,
            peerReachable: true));
    }

    [Theory]
    [InlineData(GattStatusFormatter.EAccessDenied)]
    [InlineData(GattStatusFormatter.EBluetoothAttInsufficientAuthentication)]
    [InlineData(GattStatusFormatter.EBluetoothAttInsufficientEncryption)]
    public void TheAuthenticationHresultsAreTheSignatureToo(int hresult) =>
        Assert.True(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Command,
            outcome: null,
            hresult: hresult,
            windowsStillPaired: true,
            peerReachable: true));

    [Fact]
    public void AnOrdinaryUnreachableFailureIsNotTheSignature()
    {
        // Otherwise every out-of-range adapter would offer to unpair itself.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Connect,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            windowsStillPaired: true,
            peerReachable: true));
    }

    [Fact]
    public void AConnectFailureWithNoStatusAtAllCannotBeTheSignature()
    {
        // A bare timeout carries no evidence about keys either way.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Connect,
            outcome: null,
            hresult: null,
            windowsStillPaired: true,
            peerReachable: true));
    }

    [Fact]
    public void TheRepairMessageNamesAnActionTheAppPerforms()
    {
        // This is the one place Windows is better than Android: the Kotlin message
        // has to send the user to Bluetooth settings because removeBond() is a
        // privileged @SystemApi. Here the app can unpair itself.
        Assert.Contains("Repair pairing", AdapterResetSignature.RepairMessage);
        Assert.DoesNotContain("settings", AdapterResetSignature.RepairMessage);
    }
}

public sealed class GattStatusFormatterTests
{
    [Fact]
    public void EachStatusNamespaceIsNamedRatherThanFlattenedIntoOneNumber()
    {
        // The Kotlin formatter exists specifically to stop an HCI code being read
        // as an ATT code. Windows hides HCI, so the equivalent discipline is to
        // keep GattCommunicationStatus, the ATT error byte and the HRESULT
        // distinguishable.
        var described = GattStatusFormatter.Describe(
            GattFailureStage.Command,
            GattCommunicationOutcome.ProtocolError,
            protocolError: 0x05,
            hresult: GattStatusFormatter.EAccessDenied);

        Assert.Contains("stage=command", described);
        Assert.Contains("GattCommunicationStatus=ProtocolError", described);
        Assert.Contains("ATT=0x05 INSUFFICIENT_AUTHENTICATION", described);
        Assert.Contains("HRESULT=0x80070005 E_ACCESSDENIED", described);
    }

    [Fact]
    public void AbsentFieldsAreOmittedRatherThanRenderedAsZero()
    {
        // A zero HRESULT beside a real status would read as "S_OK", which is the
        // opposite of what happened.
        var described = GattStatusFormatter.Describe(
            GattFailureStage.Connect,
            GattCommunicationOutcome.Unreachable,
            protocolError: null,
            hresult: null);

        Assert.DoesNotContain("ATT=", described);
        Assert.DoesNotContain("HRESULT=", described);
    }
}

public sealed class GattCallbackAuthorityTests
{
    [Fact]
    public void OnlyTheCurrentGenerationOfALiveOwnerMayMutateState()
    {
        Assert.True(GattCallbackAuthority.IsAuthoritative(5, 5, callbackOwnerClosed: false));
        Assert.False(GattCallbackAuthority.IsAuthoritative(6, 5, callbackOwnerClosed: false));
        Assert.False(GattCallbackAuthority.IsAuthoritative(5, 5, callbackOwnerClosed: true));
        Assert.False(GattCallbackAuthority.IsAuthoritative(null, 5, callbackOwnerClosed: false));
    }
}

public sealed class BluetoothAddressFormatTests
{
    [Fact]
    public void TheTwoRepresentationsRoundTrip()
    {
        Assert.Equal("AA:BB:CC:DD:EE:FF", BluetoothAddressFormat.ToText(0xAABBCCDDEEFF));
        Assert.True(BluetoothAddressFormat.TryParse("AA:BB:CC:DD:EE:FF", out var parsed));
        Assert.Equal(0xAABBCCDDEEFFUL, parsed);
    }

    [Fact]
    public void ByteOrderIsBigEndianAndLeadingZeroesSurvive() =>
        Assert.Equal("00:11:22:33:44:55", BluetoothAddressFormat.ToText(0x001122334455));

    [Fact]
    public void AnythingThatIsNotSixHexOctetsIsRefused()
    {
        Assert.Null(BluetoothAddressFormat.ToText(0));
        Assert.Null(BluetoothAddressFormat.ToText(0x1_0000_0000_0000));
        Assert.False(BluetoothAddressFormat.TryParse(null, out _));
        Assert.False(BluetoothAddressFormat.TryParse("AA:BB:CC:DD:EE", out _));
        Assert.False(BluetoothAddressFormat.TryParse("AABBCCDDEEFF", out _));
        Assert.False(BluetoothAddressFormat.TryParse("ZZ:BB:CC:DD:EE:FF", out _));
    }
}

public sealed class BluetoothRadioCapabilitiesTests
{
    [Fact]
    public void EachMissingCapabilityGetsItsOwnReason()
    {
        Assert.Contains("no Bluetooth radio", new BluetoothRadioCapabilities().ManagementBlockedReason);

        Assert.Contains("turned off", new BluetoothRadioCapabilities
        {
            RadioPresent = true,
        }.ManagementBlockedReason);

        Assert.Contains("Bluetooth LE", new BluetoothRadioCapabilities
        {
            RadioPresent = true, RadioOn = true,
        }.ManagementBlockedReason);

        Assert.Contains("LE central", new BluetoothRadioCapabilities
        {
            RadioPresent = true, RadioOn = true, LowEnergySupported = true,
        }.ManagementBlockedReason);
    }

    [Fact]
    public void AManagementCapableRadioWithoutThePeripheralRoleIsNormalNotBroken()
    {
        // The peripheral role is not universal across the population. A machine
        // without it is a normal machine, and the UI must never present it as a
        // fault -- but management is unaffected.
        var capabilities = new BluetoothRadioCapabilities
        {
            RadioPresent = true,
            RadioOn = true,
            LowEnergySupported = true,
            CentralRoleSupported = true,
            PeripheralRoleSupported = false,
        };

        Assert.Null(capabilities.ManagementBlockedReason);
        Assert.Contains("cannot advertise as a peripheral", capabilities.PeripheralBlockedReason);
    }

    [Fact]
    public void ARadioProblemBlocksBothHalvesAndSaysSoOnce()
    {
        var capabilities = new BluetoothRadioCapabilities { RadioPresent = true, RadioOn = false };
        Assert.Equal(capabilities.ManagementBlockedReason, capabilities.PeripheralBlockedReason);
    }

    [Fact]
    public void TheBlockingReasonIsThrownBeforeAConnectRatherThanAfterATimeout()
    {
        var error = Assert.Throws<ManagementException>(
            () => WindowsBluetoothRadio.RequireManagementCapable(new BluetoothRadioCapabilities()));
        Assert.Contains("no Bluetooth radio", error.Message);

        WindowsBluetoothRadio.RequireManagementCapable(new BluetoothRadioCapabilities
        {
            RadioPresent = true,
            RadioOn = true,
            LowEnergySupported = true,
            CentralRoleSupported = true,
        });
    }
}
