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
/// The Windows bond-mismatch signature, pinned to what the hardware did.
///
/// **Confidence: the observable shape is Confirmed.** On 2026-08-29 an adapter
/// was reflashed with the Windows pairing left in place, and four connect
/// attempts produced <c>services / Unreachable</c> with no ATT byte and no
/// <c>HRESULT</c> — never the <c>AccessDenied</c> the first version of this
/// signature was written to expect. An unpair and re-pair fixed it instantly,
/// with no firmware change.
///
/// These tests therefore do two jobs, and the second is the important one:
/// they pin the corrected condition set, and they prove it cannot fire on the
/// ordinary failures that share its status value.
/// </summary>
public sealed class AdapterResetSignatureTests
{
    /// <summary>The exact evidence the 2026-08-29 run produced, second attempt.</summary>
    private static TransportTrustSnapshot Reflashed => new(
        WindowsPaired: true,
        PeerObserved: true,
        PeerAnsweredGatt: false,
        LinkFailuresAfterResolve: 2);

    [Fact]
    public void TheHardwareShapeIsRecognised()
    {
        // 16:47:09 on 2026-08-29, reconstructed exactly: Windows reports the
        // adapter paired, the address-restricted fallback scan sees that exact
        // adapter advertising, and service discovery returns Unreachable on both
        // the direct and the scan-resolved device.
        Assert.True(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed));
    }

    [Fact]
    public void AProvenBondCannotBeDiagnosedAsAResetAdapter()
    {
        // 2026-08-31, the false positive. A live session -- services resolved,
        // characteristics read, CCC written -- stalled during a KB/M resident
        // upload, and the reconnect that followed produced the full link-refusal
        // shape against an adapter that was present and correctly bonded. The app
        // offered to destroy that pairing; a power cycle and an ordinary reconnect
        // proved it had been valid all along.
        //
        // BondProven is the disqualifier: those handles are ATT_SECURITY_ENCRYPTED,
        // so reaching the end of that sequence means the key matched.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed with { BondProven = true }));
    }

    [Fact]
    public void AnUnreachableCommandOnALiveSessionIsNotEvidenceOfAnything()
    {
        // The other half of the same defect. The transport counted an Unreachable
        // at ANY stage other than Connect, so two failing commands during an upload
        // reached the corroboration threshold with no connect failure at all. A
        // command stage is reached only after the bond has been proven, so it must
        // never be the thing that convicts it.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Command,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed with { BondProven = true }));
    }

    [Fact]
    public void TheGenuineStaleBondPathIsUntouchedByTheDisqualifier()
    {
        // A stale bond fails BELOW the attribute layer, so it can never reach the
        // CCC write and can never set BondProven. The 2026-08-29 shape must still
        // classify, unchanged.
        Assert.True(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed));
        Assert.False(Reflashed.BondProven);
    }

    [Fact]
    public void TheAttributeLayerShapeIsStillRecognised()
    {
        // Retained even though this hardware never produced it. A different radio,
        // driver or Windows build may surface the refusal at the attribute layer,
        // and it costs nothing to keep. One resolved-device failure is enough here:
        // an explicit AccessDenied is conclusive on its own.
        Assert.True(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.AccessDenied,
            hresult: null,
            new TransportTrustSnapshot(
                WindowsPaired: true,
                PeerObserved: false,
                PeerAnsweredGatt: true,
                LinkFailuresAfterResolve: 0)));
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
            new TransportTrustSnapshot(true, PeerObserved: true)));

    [Theory]
    [InlineData(GattFailureStage.Services)]
    [InlineData(GattFailureStage.Subscribe)]
    [InlineData(GattFailureStage.Command)]
    public void TheSignatureIsNotRestrictedToOneStage(GattFailureStage stage) =>
        // Android matches HCI 0x05/0x06 at the CONNECT stage. Windows exposes no
        // HCI to user mode, and the refusal surfaces wherever the stack first needs
        // the key. Pinning all three stops a future tidy-up from reintroducing
        // Android's stage restriction.
        Assert.True(AdapterResetSignature.IsBondMismatch(
            stage,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed));

    /* --------------------------------------------------------------------
     * The negative half. Unreachable is ALSO what an absent adapter produces,
     * so every one of these must stay false or the app would offer to destroy
     * a working pairing on ordinary bad luck.
     * ------------------------------------------------------------------ */

    [Fact]
    public void APoweredOffAdapterIsNotABondMismatch()
    {
        // THE EXACT EVIDENCE a powered-off adapter produced on hardware,
        // 2026-08-29 17:52. This is the single most important negative: a
        // switched-off adapter returns the same GattCommunicationStatus as a
        // reflashed one, and getting it wrong would offer to destroy a working
        // pairing to recover from a flat battery.
        //
        //   [ble] link status=Unreachable connection=Disconnected session=Closed maxPdu=23
        //   [ble] fail stage=services GattCommunicationStatus=Unreachable
        //   [connect] failed [...] [paired=True observed=False answeredGatt=False
        //                           linkFailures=1/2 -> not a bond mismatch]
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            new TransportTrustSnapshot(
                WindowsPaired: true,
                PeerObserved: false,
                PeerAnsweredGatt: false,
                LinkFailuresAfterResolve: 1)));

        // And it must stay false even if the fallback HAD produced a second
        // resolved-device failure, because the presence fact is what is missing.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            new TransportTrustSnapshot(
                WindowsPaired: true,
                PeerObserved: false,
                PeerAnsweredGatt: false,
                LinkFailuresAfterResolve: 2)));
    }

    [Fact]
    public void AnAdapterThatWasNeverSeenIsNotABondMismatch() =>
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            new TransportTrustSnapshot(WindowsPaired: true, PeerObserved: false)));

    [Fact]
    public void ASingleTransientLinkFailureIsNotEnough()
    {
        // One resolved-device failure against a present adapter is noise: a
        // momentary link collision looks identical. The corroboration is the
        // direct connect and the fresh scan-resolved connect INDEPENDENTLY
        // agreeing, which is why the ladder is allowed to run its fallback.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed with { LinkFailuresAfterResolve = 1 }));

        Assert.Equal(2, AdapterResetSignature.CorroboratingLinkFailures);
    }

    [Fact]
    public void AnUnpairedAdapterIsNotABondMismatchHoweverItFails()
    {
        // Without a held pairing this is simply "not paired": a different message,
        // a different flow, and nothing to repair. Covers the first-time pairing
        // attempt made outside the adapter's physical pairing window.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed with { WindowsPaired = false }));

        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Services,
            GattCommunicationOutcome.AccessDenied,
            hresult: null,
            new TransportTrustSnapshot(false, PeerObserved: true, PeerAnsweredGatt: true)));
    }

    [Fact]
    public void AFailureBeforeTheDeviceResolvedIsNotABondMismatch()
    {
        // A connect-stage failure means Windows never opened the device, so it
        // never got as far as needing a key. Retryable, and handled by the clean
        // retry instead.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Connect,
            GattCommunicationOutcome.Unreachable,
            hresult: null,
            Reflashed));

        // A bare timeout carries no evidence about keys in either direction.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Connect,
            outcome: null,
            hresult: null,
            Reflashed));
    }

    [Fact]
    public void AProtocolErrorThatIsNotAnAuthenticationRefusalIsNotTheSignature()
    {
        // The adapter answered at the attribute layer with an ordinary ATT error.
        // It has a key; it simply refused this request.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            GattFailureStage.Command,
            GattCommunicationOutcome.ProtocolError,
            hresult: null,
            new TransportTrustSnapshot(true, PeerObserved: true, PeerAnsweredGatt: true)));
    }

    [Fact]
    public void ASuccessfulRetryNeverReachesTheSignatureAtAll()
    {
        // Nothing to classify without a tagged transport failure -- which is what a
        // recovered attempt leaves behind.
        Assert.False(AdapterResetSignature.IsBondMismatch(
            new ManagementException("no transport detail"),
            Reflashed));
    }

    [Fact]
    public void TheExplanationNamesEveryPredicateAndTheVerdict()
    {
        // The 2026-08-29 log said only "paired=True peerAnswered=False", which was
        // not enough to tell WHICH clause rejected the classification. The next run
        // must not have that problem.
        var failure = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);

        var explained = AdapterResetSignature.Explain(failure, Reflashed);
        Assert.Equal(
            "paired=True observed=True answeredGatt=False bondProven=False " +
            "linkFailures=2/2 -> BOND MISMATCH",
            explained);

        Assert.Contains(
            "linkFailures=1/2 -> not a bond mismatch",
            AdapterResetSignature.Explain(failure, Reflashed with { LinkFailuresAfterResolve = 1 }));
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

    [Fact]
    public void TheStalePairingMessageExistsForTheCaseWithNoRowToRepair()
    {
        // Reached through the PAIR flow after the row was removed, where there is
        // no Repair button to point at. It must still name a way out.
        Assert.Contains("Repair pairing", AdapterResetSignature.StalePairingMessage);
        Assert.NotEqual(AdapterResetSignature.RepairMessage, AdapterResetSignature.StalePairingMessage);
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
