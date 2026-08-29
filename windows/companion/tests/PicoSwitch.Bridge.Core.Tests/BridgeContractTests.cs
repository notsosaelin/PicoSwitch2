using System.Security.Cryptography;
using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The descriptor and the contract version, cross-checked against the C fixture.
///
/// This is the C# third of the anti-drift guard described in WINDOWS_PASS.md
/// §26.2. The firmware recognises this bridge by an EXACT descriptor match, and
/// when the two ends disagree the failure is silent: v1 input keeps working while
/// battery, motion and rumble all vanish at once. A byte comparison plus a
/// registered digest turns that into a red test.
/// </summary>
public sealed class BridgeContractTests
{
    [Fact]
    public void DescriptorIsByteIdenticalToTheCFixture()
    {
        var expected = RepositoryFixtures.CDescriptorBytes();
        var actual = BridgeHidDescriptor.Bytes.ToArray();

        Assert.Equal(expected.Length, actual.Length);
        for (var index = 0; index < expected.Length; index++)
        {
            Assert.True(
                expected[index] == actual[index],
                $"first difference at index {index}: C=0x{expected[index]:X2} C#=0x{actual[index]:X2}");
        }
    }

    [Fact]
    public void DescriptorMatchesTheDigestRegisteredForThisContract()
    {
        var digest = Convert.ToHexString(SHA256.HashData(BridgeHidDescriptor.Bytes)).ToLowerInvariant();
        Assert.Equal(
            "f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054",
            digest);
        Assert.Equal(digest, BridgeContract.ExpectedDescriptorDigest);
    }

    [Fact]
    public void ContractVersionMatchesTheCFixture()
    {
        var fromFixture = RepositoryFixtures.CContractVersion();
        Assert.Equal(BridgeContract.Version, fromFixture);
    }

    [Fact]
    public void HistoricalDigestsAreRetainedSoOldCapturesStayIdentifiable()
    {
        // They cost nothing and they let a capture or a bug report from an older
        // build be identified precisely.
        Assert.Contains(3, BridgeContract.DescriptorDigests.Keys);
        Assert.Contains(4, BridgeContract.DescriptorDigests.Keys);
        Assert.All(
            BridgeContract.DescriptorDigests.Values,
            value => Assert.Matches("^[0-9a-f]{64}$", value));
    }

    /* ------------------------------------------------- the decision table */

    [Fact]
    public void NothingConnectedIsNotACompatibilityClaim()
    {
        var verdict = BridgeContract.Evaluate(4, connected: false);
        Assert.IsType<BridgeContract.Compatibility.NotConnected>(verdict);
        Assert.False(BridgeContract.IsProvenCompatible(verdict));
        Assert.False(BridgeContract.WarrantsWarning(verdict));
    }

    [Fact]
    public void NotYetAskedIsPendingAndIsSILENT()
    {
        // Pending happens on every healthy connection for the second or two before
        // the info reply lands. Warning during it trains people to ignore the
        // warning that matters.
        var verdict = BridgeContract.Evaluate(null, connected: true, firmwareInfoAvailable: false);
        var pending = Assert.IsType<BridgeContract.Compatibility.Pending>(verdict);
        Assert.Equal(BridgeContract.Version, pending.Companion);
        Assert.False(BridgeContract.IsProvenCompatible(verdict));
        Assert.False(BridgeContract.WarrantsWarning(verdict));
    }

    [Fact]
    public void PendingWinsOverAReportedContractBecauseTheReplyHasNotArrived()
    {
        // firmwareInfoAvailable is the gate, not the presence of a number: a stale
        // contract from the previous adapter must not describe this one.
        Assert.IsType<BridgeContract.Compatibility.Pending>(
            BridgeContract.Evaluate(4, connected: true, firmwareInfoAvailable: false));
    }

    [Theory]
    [InlineData(null)]
    [InlineData(0)]
    [InlineData(-1)]
    public void AnAdapterThatReportsNoContractIsUnknownAndWarns(int? reported)
    {
        var verdict = BridgeContract.Evaluate(reported, connected: true);
        var unknown = Assert.IsType<BridgeContract.Compatibility.Unknown>(verdict);
        Assert.Equal(BridgeContract.Version, unknown.Companion);

        // Unknown is deliberately NOT folded into Compatible: firmware that does
        // not report a contract predates the mechanism, so it IS older.
        Assert.False(BridgeContract.IsProvenCompatible(verdict));
        Assert.True(BridgeContract.WarrantsWarning(verdict));
    }

    [Fact]
    public void AgreementIsTheOnlyProvenCompatibleOutcome()
    {
        var verdict = BridgeContract.Evaluate(BridgeContract.Version, connected: true);
        Assert.IsType<BridgeContract.Compatibility.Compatible>(verdict);
        Assert.True(BridgeContract.IsProvenCompatible(verdict));
        Assert.False(BridgeContract.WarrantsWarning(verdict));
    }

    [Fact]
    public void AnOlderFirmwareIsToldToFlashAndANewerOneToUpdateTheApp()
    {
        var older = Assert.IsType<BridgeContract.Compatibility.Mismatch>(
            BridgeContract.Evaluate(BridgeContract.Version - 1, connected: true));
        Assert.True(older.FirmwareIsOlder);
        Assert.Contains("Flash current firmware", older.Summary);

        var newer = Assert.IsType<BridgeContract.Compatibility.Mismatch>(
            BridgeContract.Evaluate(BridgeContract.Version + 1, connected: true));
        Assert.False(newer.FirmwareIsOlder);
        Assert.Contains("Install the matching companion build", newer.Summary);

        Assert.True(BridgeContract.WarrantsWarning(older));
        Assert.True(BridgeContract.WarrantsWarning(newer));
        Assert.False(BridgeContract.IsProvenCompatible(older));
    }

    [Fact]
    public void EveryVerdictCarriesASummaryAUserCanActOn()
    {
        BridgeContract.Compatibility[] verdicts =
        [
            BridgeContract.Evaluate(4, connected: false),
            BridgeContract.Evaluate(null, connected: true, firmwareInfoAvailable: false),
            BridgeContract.Evaluate(null, connected: true),
            BridgeContract.Evaluate(BridgeContract.Version, connected: true),
            BridgeContract.Evaluate(BridgeContract.Version - 1, connected: true),
        ];

        Assert.All(verdicts, verdict => Assert.False(string.IsNullOrWhiteSpace(verdict.Summary)));
    }

    [Fact]
    public void ServiceRecordNamesTheBridgeRatherThanAnyOnePlatform()
    {
        // The firmware matches the descriptor bytes and never the service name,
        // so this is presentation -- but it appears in UART captures, and calling
        // it "Android" from a Windows host would misidentify every capture.
        Assert.Equal("PicoSwitch Bridge Controller", BridgeHidDescriptor.SdpName);
        Assert.DoesNotContain("Android", BridgeHidDescriptor.SdpName);
    }
}
