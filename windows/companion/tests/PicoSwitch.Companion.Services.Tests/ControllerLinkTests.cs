using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Bluetooth;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Controller Link in its gate-failed shape (`WINDOWS_PASS.md` §14.6, §31 Phase 6
/// "If the gate fails").
///
/// The §14.5 experiment ran on 2026-08-31 and did not pass: B1 and B2 succeeded,
/// B3–B6 were never reached because the radio refuses the connectable
/// advertisement, and package identity was tested and ruled out. What ships
/// instead is a page that explains that specifically — and §33.2 accepts the
/// negative result **only** on that condition, which is why the sentences are
/// asserted here rather than written in XAML.
///
/// Every test runs without a radio.
/// </summary>
public sealed class ControllerLinkViewTests
{
    [Fact]
    public void TheMeasuredCaseNamesTheCapabilityThatIsMissing()
    {
        // The state this project's only test machine is in. "Not supported on
        // Windows" would be both unhelpful and false, since Windows published the
        // service perfectly well.
        var view = ControllerLinkView.Of(new ControllerLinkCapability(
            ControllerLinkStep.AdvertisingRefused,
            ClaimsPeripheralRole: true,
            RadioAddress: "14:18:C3:47:C4:89"));

        Assert.Contains("advertise", view.Headline, StringComparison.OrdinalIgnoreCase);

        // The two facts a user needs in order to act: it is the radio, and it is
        // not their adapter or their setup.
        Assert.Contains("radio or its driver", view.Explanation, StringComparison.Ordinal);
        Assert.Contains("not of the adapter", view.Explanation, StringComparison.Ordinal);

        // And the reason to believe it: the refusal is not specific to this
        // feature.
        Assert.Contains("no meaning at all", view.Explanation, StringComparison.Ordinal);
    }

    [Fact]
    public void TheRadioLineShowsTheClaimNextToTheBehaviourWhenTheyDisagree()
    {
        // `IsPeripheralRoleSupported` reports true on the test radio while every
        // connectable advertisement aborts. Hiding that contradiction to look
        // tidy would remove the single most useful line on the page for anyone
        // deciding whether another Bluetooth adapter would help.
        var view = ControllerLinkView.Of(new ControllerLinkCapability(
            ControllerLinkStep.AdvertisingRefused,
            ClaimsPeripheralRole: true,
            RadioAddress: "14:18:C3:47:C4:89"));

        Assert.NotNull(view.RadioLine);
        Assert.Contains("reports support", view.RadioLine, StringComparison.Ordinal);
        Assert.Contains("does not perform it", view.RadioLine, StringComparison.Ordinal);
        Assert.Contains("14:18:C3:47:C4:89", view.RadioLine, StringComparison.Ordinal);
    }

    [Fact]
    public void ARadioThatDoesNotClaimTheRoleIsNotAccusedOfContradictingItself()
    {
        var view = ControllerLinkView.Of(new ControllerLinkCapability(
            ControllerLinkStep.NoPeripheralRole, ClaimsPeripheralRole: false));

        Assert.NotNull(view.RadioLine);
        Assert.Contains("does not report support", view.RadioLine, StringComparison.Ordinal);
        Assert.DoesNotContain("does not perform it", view.RadioLine, StringComparison.Ordinal);
    }

    [Fact]
    public void AWorkingRadioIsNotPromisedAWorkingFeature()
    {
        // The revisit path, and the one most at risk of over-claiming. Advertising
        // removes the platform blocker; it does not answer B3 — whether the
        // adapter's HOGP client proceeds without the Device Information Service
        // that Windows forbids an application from publishing. No radio has ever
        // reached the point where that could be asked.
        var view = ControllerLinkView.Of(new ControllerLinkCapability(
            ControllerLinkStep.Advertising, ClaimsPeripheralRole: true));

        Assert.Contains("not built yet", view.Headline, StringComparison.Ordinal);
        Assert.Contains("does not mean controller input works", view.Explanation,
            StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Device Information Service", view.Explanation, StringComparison.Ordinal);
    }

    [Fact]
    public void EveryOutcomeSaysSomethingAndOffersARecheck()
    {
        // A page that can reach a state with no text is worse than no page. The
        // recheck matters just as much: "we can revisit this later" is only true
        // if a user who swaps in another Bluetooth adapter can ask again.
        foreach (var step in Enum.GetValues<ControllerLinkStep>())
        {
            var view = ControllerLinkView.Of(new ControllerLinkCapability(step));

            Assert.False(string.IsNullOrWhiteSpace(view.Headline), $"{step} has no headline");
            Assert.False(string.IsNullOrWhiteSpace(view.Explanation), $"{step} has no explanation");
            Assert.True(view.ShowRecheck, $"{step} cannot be re-measured");
        }
    }

    [Fact]
    public void NoOutcomeClaimsControllerLinkWorks()
    {
        // The whole page exists because the feature does not exist. Nothing on it
        // may read as though it does.
        foreach (var step in Enum.GetValues<ControllerLinkStep>())
        {
            var view = ControllerLinkView.Of(new ControllerLinkCapability(step, true, "AA:BB"));
            var text = $"{view.Headline} {view.Explanation}";

            Assert.DoesNotContain("is supported", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("is ready", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("you can now", text, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void NothingIsClaimedBeforeAnythingIsMeasured()
    {
        Assert.Equal(ControllerLinkStep.Unknown, ControllerLinkView.Idle.Step);
        Assert.Null(ControllerLinkView.Idle.RadioLine);
        Assert.False(ControllerLinkView.Idle.Measuring);
    }
}

/// <summary>The service around the probe, over a scripted measurement.</summary>
public sealed class ControllerLinkServiceTests
{
    private static ControllerLinkService Service(
        Func<CancellationToken, Task<ControllerLinkCapability>> measure) =>
        new(new DiagnosticLog(), measure);

    [Fact]
    public async Task AMeasurementReplacesTheIdleView()
    {
        var service = Service(_ => Task.FromResult(
            new ControllerLinkCapability(ControllerLinkStep.AdvertisingRefused, true, "AA:BB")));

        Assert.Equal(ControllerLinkStep.Unknown, service.View.Value.Step);

        await service.CheckAsync();

        Assert.Equal(ControllerLinkStep.AdvertisingRefused, service.View.Value.Step);
        Assert.Equal(ControllerLinkStep.AdvertisingRefused, service.Capability.Step);
        Assert.False(service.View.Value.Measuring);
    }

    [Fact]
    public async Task AProbeThatThrowsLeavesThePageSayingSomething()
    {
        // A page stuck on "measuring…" forever is the failure this prevents: the
        // user cannot act and cannot find out why.
        var service = Service(_ => throw new InvalidOperationException("radio exploded"));

        await service.CheckAsync();

        Assert.False(service.View.Value.Measuring);
        Assert.True(service.View.Value.ShowRecheck);
        Assert.False(string.IsNullOrWhiteSpace(service.View.Value.Explanation));
    }

    [Fact]
    public async Task ASecondCheckWhileOneIsRunningIsIgnored()
    {
        // Two overlapping probes would race each other's StopAdvertising and
        // could leave the machine advertising after both returned.
        var started = 0;
        var release = new TaskCompletionSource();
        var service = Service(async _ =>
        {
            Interlocked.Increment(ref started);
            await release.Task;
            return new ControllerLinkCapability(ControllerLinkStep.Advertising);
        });

        var first = service.CheckAsync();
        await service.CheckAsync();

        Assert.Equal(1, started);

        release.SetResult();
        await first;
        Assert.Equal(ControllerLinkStep.Advertising, service.View.Value.Step);
    }

    [Fact]
    public async Task TheOutcomeReachesTheDiagnosticLog()
    {
        // The line a support bundle needs. "This radio cannot" and "the feature is
        // unfinished" are indistinguishable to a user, and only this tells them
        // apart after the fact.
        var log = new DiagnosticLog();
        var service = new ControllerLinkService(log, _ => Task.FromResult(
            new ControllerLinkCapability(ControllerLinkStep.AdvertisingRefused, true, "AA:BB:CC")));

        await service.CheckAsync();

        Assert.Contains(log.Snapshot(), entry =>
            entry.Source == "gamepad" &&
            entry.Message.Contains("AdvertisingRefused", StringComparison.Ordinal) &&
            entry.Message.Contains("claimsPeripheralRole=True", StringComparison.Ordinal));
    }
}
