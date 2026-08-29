using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The support bundle, whose whole job is to be safe to paste in public.
///
/// The privacy assertions are the important ones. Someone pasting this into an
/// issue is publishing it, and a leak is not recoverable once it is in a tracker.
/// </summary>
public sealed class SupportBundleTests
{
    private static string Render(
        AdapterSnapshot? snapshot = null,
        AdapterRegistry? registry = null,
        BluetoothRadioCapabilities? radio = null,
        bool connected = true)
    {
        var log = new DiagnosticLog();
        log.Info("app", "started");

        return SupportBundle.Render(
            log,
            snapshot ?? new AdapterSnapshot(),
            registry ?? new AdapterRegistry(),
            new AdapterRelationshipStatus { Phase = AdapterRelationshipPhase.Connected },
            radio ?? new BluetoothRadioCapabilities
            {
                RadioPresent = true,
                RadioOn = true,
                LowEnergySupported = true,
                CentralRoleSupported = true,
                PeripheralRoleSupported = true,
                LocalAddress = "14:18:C3:47:C4:89",
            },
            new ConnectionState
            {
                Phase = connected ? ConnectionPhase.Connected : ConnectionPhase.Idle,
            });
    }

    /* ------------------------------------------------------------- privacy */

    [Fact]
    public void NoFullBluetoothAddressEverAppears()
    {
        var registry = new AdapterRegistry().With(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!);
        var bundle = Render(registry: registry);

        Assert.DoesNotContain("AA:BB:CC:DD:EE:01", bundle);
        Assert.DoesNotContain("14:18:C3:47:C4:89", bundle);

        // The suffix survives, because telling two adapters apart is the whole
        // reason an identifier appears at all.
        Assert.Contains("DDEE01", bundle);
    }

    [Fact]
    public void AdapterNicknamesNeverAppear()
    {
        // People name adapters after rooms, houses and children.
        var registry = new AdapterRegistry().With(
            AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with { UserAlias = "Emily's bedroom" });

        Assert.DoesNotContain("Emily", Render(registry: registry));
    }

    [Fact]
    public void ControllerProductNamesDoAppearBecauseThatIsThePoint()
    {
        // These identify hardware, not a person, and a compatibility report
        // without them is unusable.
        var bundle = Render(new AdapterSnapshot
        {
            Controller = new ControllerInfo("DualSense Wireless Controller", 0x054C, 0x0CE6),
        });

        Assert.Contains("DualSense Wireless Controller", bundle);
    }

    [Fact]
    public void TheHeaderStatesTheGuaranteeToThePersonPastingIt()
    {
        // A promise that lives only in documentation is one the reader never sees.
        var bundle = Render();

        Assert.Contains("short suffix", bundle);
        Assert.Contains("nicknames are never included", bundle);
    }

    /* -------------------------------------------------------------- content */

    [Fact]
    public void ThePeripheralRoleIsReportedBecauseItDecidesControllerLink()
    {
        // The first thing anyone needs when a report says "it does not work on my
        // PC". Without it every such report starts with two rounds of questions.
        Assert.Contains("peripheral    : True", Render());

        Assert.Contains(
            "peripheral    : False",
            Render(radio: new BluetoothRadioCapabilities
            {
                RadioPresent = true,
                RadioOn = true,
                LowEnergySupported = true,
                CentralRoleSupported = true,
                PeripheralRoleSupported = false,
            }));
    }

    [Fact]
    public void BothBridgeContractVersionsAndTheVerdictAreIncluded()
    {
        // A mismatch is invisible unless both numbers are present; the verdict
        // saves the reader from having to know which is which.
        var bundle = Render(new AdapterSnapshot
        {
            Firmware = new FirmwareInfo("picoswitch", "PicoSwitch2", "2.0", 1, "abc"),
        });

        Assert.Contains("bridge (fw)   : 1", bundle);
        Assert.Contains("INCOMPATIBLE", bundle);
    }

    [Fact]
    public void EveryCapabilityStateIsListedIncludingTheUnknownOnes()
    {
        // Unknown is a real answer and is exactly what a reader needs to see when
        // a feature is missing for no apparent reason.
        var bundle = Render(new AdapterSnapshot
        {
            Capabilities = new AdapterCapabilities(
                Peers: CapabilityState.Available,
                PeerForget: CapabilityState.Unsupported,
                RemotePairing: CapabilityState.Unknown),
        });

        Assert.Contains("peers         : Available", bundle);
        Assert.Contains("peer forget   : Unsupported", bundle);
        Assert.Contains("remote pairing: Unknown", bundle);
    }

    [Fact]
    public void TheDiagnosticLogIsAppendedWhole() =>
        Assert.Contains("started", Render());

    [Fact]
    public void TheHostAndRuntimeAreIdentified()
    {
        var bundle = Render();
        Assert.Contains("architecture  :", bundle);
        Assert.Contains("runtime       :", bundle);
    }

    [Fact]
    public void AnEmptyBundleStillRendersRatherThanThrowing()
    {
        // The moment a support bundle is most needed is the moment least state
        // exists. It must never be the thing that fails.
        var bundle = SupportBundle.Render(
            new DiagnosticLog(),
            new AdapterSnapshot(),
            new AdapterRegistry(),
            new AdapterRelationshipStatus(),
            new BluetoothRadioCapabilities(),
            new ConnectionState());

        Assert.Contains("PicoSwitch2 companion", bundle);
        Assert.Contains("active        : none", bundle);
    }
}
