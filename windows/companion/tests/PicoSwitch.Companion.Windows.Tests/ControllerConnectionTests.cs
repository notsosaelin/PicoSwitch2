using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Windows.Input;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// How the controller reaches this PC, and why the app says something about it.
///
/// Root-caused on hardware 2026-09-03. With the controller on Bluetooth the
/// input takes two radio hops on one adapter — controller to PC, then PC to
/// PicoSwitch — and under sustained stick motion the controller's own link loses
/// airtime. Windows then hands the app stale readings, the app forwards them
/// faithfully, and the player sees the stick still moving after they stopped.
///
/// The cruel part, and the reason this is worth detecting: every counter on the
/// Controller Link side stays perfect throughout. Frames generated equals frames
/// written equals frames received, nothing coalesces, nothing goes stale. The
/// transport is behaving exactly as designed while the experience is unusable,
/// so no amount of looking at our own diagnostics finds it. The same controller
/// on USB has none of it.
/// </summary>
public sealed class ControllerConnectionTests
{
    private static WindowsControllerSource Source(ControllerConnection connection) =>
        new(new ControllerCandidate(
                Id: 0,
                Descriptor: "pad",
                Name: "Xbox Wireless Controller",
                VendorId: 0x045E,
                ProductId: 0x0B22,
                HasMotionAxes: true,
                HasGamepadButtons: true),
            ControllerAttachment.External,
            IsGamepadClass: true,
            MayBeThisAdapter: false,
            Connection: connection);

    [Fact]
    public void ABluetoothControllerIsCalledOut()
    {
        var advice = Source(ControllerConnection.Bluetooth).ConnectionAdvice;

        Assert.NotNull(advice);
        Assert.Contains("share", advice);
        Assert.Contains("USB", advice);
    }

    [Fact]
    public void AWiredControllerSaysNothing()
    {
        Assert.Null(Source(ControllerConnection.Usb).ConnectionAdvice);
    }

    [Fact]
    public void AnUnknownConnectionSaysNothing()
    {
        // Warning about a radio hop that may not exist is worse than silence.
        Assert.Null(Source(ControllerConnection.Unknown).ConnectionAdvice);
    }

    [Fact]
    public void TheAdviceReachesTheViewForTheSelectedSourceOnly()
    {
        var bluetooth = Source(ControllerConnection.Bluetooth);
        var wired = Source(ControllerConnection.Usb) with
        {
            Candidate = new ControllerCandidate(
                Id: 1, Descriptor: "wired", Name: "Wired Pad",
                VendorId: 0x045E, ProductId: 0x02FF,
                HasMotionAxes: true, HasGamepadButtons: true),
        };

        Assert.NotNull(ControllerSourceView.Of([bluetooth, wired], bluetooth, null).Advice);

        // Selected the wired one: the other controller's advice is irrelevant.
        Assert.Null(ControllerSourceView.Of([bluetooth, wired], wired, null).Advice);

        // Nothing selected: nothing to advise about.
        Assert.Null(ControllerSourceView.Of([bluetooth, wired], null, "choose one").Advice);
    }
}
