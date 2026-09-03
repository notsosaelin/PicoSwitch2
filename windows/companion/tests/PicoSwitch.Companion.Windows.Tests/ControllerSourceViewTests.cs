using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Windows.Input;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// What the Gamepad page says about which controller on this PC feeds the
/// adapter.
///
/// These are wording tests on purpose. The page paints
/// <see cref="ControllerSourceView"/> and decides nothing, so a sentence that
/// misleads — "ready" about a source that sends nothing, or silence about the
/// adapter feeding itself — is a defect that only a test at this layer can
/// catch.
/// </summary>
public sealed class ControllerSourceViewTests
{
    private static WindowsControllerSource Source(
        string id,
        string name = "Controller",
        ControllerAttachment attachment = ControllerAttachment.External,
        bool gamepadClass = true,
        bool mayBeAdapter = false,
        int vendor = 0x045E,
        int product = 0x02FF) =>
        new(new ControllerCandidate(
                Id: 0,
                Descriptor: id,
                Name: name,
                VendorId: vendor,
                ProductId: product,
                HasMotionAxes: true,
                HasGamepadButtons: true),
            attachment,
            gamepadClass,
            mayBeAdapter);

    [Fact]
    public void NamesTheSelectedControllerAndSaysItIsReady()
    {
        var pad = Source("a", "Xbox Wireless Controller");
        var view = ControllerSourceView.Of([pad], pad, null);

        Assert.Equal("Xbox Wireless Controller", view.Headline);
        Assert.Equal("Connected and ready.", view.Detail);
        Assert.Equal("a", view.SelectedId);
    }

    [Fact]
    public void LabelsWhereTheControllerIs()
    {
        var builtIn = Source("a", "Built-in controller", ControllerAttachment.BuiltIn);
        var view = ControllerSourceView.Of([builtIn], builtIn, null);

        Assert.Equal("Built-in controller (Built-in)", Assert.Single(view.Rows).Label);
        Assert.StartsWith("Built-in", view.Detail);
    }

    [Fact]
    public void SaysWhyNothingIsSelected()
    {
        var view = ControllerSourceView.Of([], null, "No controller is connected to this PC.");

        Assert.Equal("No controller selected", view.Headline);
        Assert.Equal("No controller is connected to this PC.", view.Detail);
        Assert.Null(view.SelectedId);
        Assert.Empty(view.Rows);
    }

    [Fact]
    public void OffersAnUnreadableControllerButMarksTheRow()
    {
        // The Switch 2 Pro case measured on this bench: real device, real name,
        // and Windows will not hand us its buttons. Hiding it would leave a user
        // staring at a list missing the controller in their hands.
        var raw = Source("raw", "Switch 2 Pro Controller", gamepadClass: false);
        var view = ControllerSourceView.Of([raw], null, raw.UnreadableReason);

        var row = Assert.Single(view.Rows);
        Assert.Contains("cannot be read", row.Label);
        Assert.False(row.CanDrive);
        Assert.True(view.CanChoose);
    }

    [Fact]
    public void AnExplicitlyChosenUnreadableControllerSaysItWillSendNothing()
    {
        // The worst possible screen is one that looks healthy and sends nothing.
        var raw = Source("raw", "Switch 2 Pro Controller", gamepadClass: false);
        var view = ControllerSourceView.Of([raw], raw, null);

        Assert.Equal("Switch 2 Pro Controller", view.Headline);
        Assert.Contains("standard layout", view.Detail);
        Assert.Contains("will not receive input", view.Detail);
    }

    [Fact]
    public void WarnsWhenTheChosenControllerIsTheAdaptersOwnOutput()
    {
        // Only reachable by an explicit choice, and someone may be testing that
        // loop on purpose -- but they should still be told it is a loop, because
        // the symptom is "the sticks drift", not an error.
        var echo = Source("echo", "Switch 2 Pro Controller",
                          vendor: 0x057E, product: 0x2069, mayBeAdapter: true);
        var view = ControllerSourceView.Of([echo], echo, null);

        Assert.Contains("adapter's own output", view.Detail);
        Assert.Contains("feeds the adapter back into itself", view.Detail);
    }

    [Fact]
    public void HidesTheChooserWhenThereIsNothingToChoose()
    {
        // A chooser offering one already-selected option is a decision the user
        // cannot make.
        var only = Source("a");
        Assert.False(ControllerSourceView.Of([only], only, null).CanChoose);
    }

    [Fact]
    public void ShowsTheChooserWhenTheUserMustDecide()
    {
        var a = Source("a", "Xbox Wireless Controller");
        var b = Source("b", "DualSense Wireless Controller");
        var view = ControllerSourceView.Of([a, b], null, "More than one controller is connected.");

        Assert.True(view.CanChoose);
        Assert.Equal(2, view.Rows.Count);
        Assert.Null(view.SelectedId);
    }

    [Fact]
    public void ShowsTheChooserForASingleSourceTheAppWouldNotTakeByItself()
    {
        // One candidate, deliberately not auto-selected: without a chooser the
        // user would have no way to override the guard at all.
        var echo = Source("echo", "Switch 2 Pro Controller",
                          vendor: 0x057E, product: 0x2069, mayBeAdapter: true);
        var view = ControllerSourceView.Of([echo], null, "…looks like this adapter's own output.");

        Assert.True(view.CanChoose);
    }
}
