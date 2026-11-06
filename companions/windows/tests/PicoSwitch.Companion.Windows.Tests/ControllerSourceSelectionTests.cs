using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Windows.Input;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// Which Windows controller Controller Link uses, and why.
///
/// The ambiguity rule is shared with Android and lives in
/// <see cref="ControllerCandidates"/>; these cover the Windows-specific parts:
/// a handheld's built-in controls, a device Windows will not give us named
/// input for, and the adapter's own USB output echoing back.
/// </summary>
public sealed class ControllerSourceSelectionTests
{
    private static WindowsControllerSource Source(
        string id,
        string name = "Controller",
        ControllerAttachment attachment = ControllerAttachment.External,
        bool gamepadClass = true,
        bool mayBeAdapter = false,
        int vendor = 0x045E,
        int product = 0x02FF,
        bool buttons = true,
        bool axes = true) =>
        new(new ControllerCandidate(
                Id: 0,
                Descriptor: id,
                Name: name,
                VendorId: vendor,
                ProductId: product,
                HasMotionAxes: axes,
                HasGamepadButtons: buttons),
            attachment,
            gamepadClass,
            mayBeAdapter);

    [Fact]
    public void SelectsTheOnlyController()
    {
        var only = Source("a");
        Assert.Same(only, ControllerSourceSelection.Resolve([only], null));
    }

    [Fact]
    public void RefusesToGuessBetweenTwoExternalControllers()
    {
        // The shared rule: two or more usable devices and the app must not
        // choose. Guessing here means someone's input silently goes nowhere.
        var sources = new[] { Source("a"), Source("b") };
        Assert.Null(ControllerSourceSelection.Resolve(sources, null));
        Assert.Contains("More than one", ControllerSourceSelection.UnresolvedReason(sources, null));
    }

    [Fact]
    public void PrefersTheHandheldsBuiltInControls()
    {
        // The one case where "more than one" still has an obvious answer. On a
        // ROG Ally or Legion Go the machine's own sticks are the controller in
        // the user's hands; an external pad they also plugged in is a
        // deliberate act they can express by choosing it.
        var builtIn = Source("built-in", "Built-in controller", ControllerAttachment.BuiltIn);
        var external = Source("external", "Xbox Wireless Controller");

        Assert.Same(builtIn, ControllerSourceSelection.Resolve([external, builtIn], null));
    }

    [Fact]
    public void DoesNotGuessBetweenTwoBuiltInControllers()
    {
        // A Legion Go with both detachable halves enumerated separately is two
        // built-in devices, and "prefer built-in" no longer disambiguates.
        var left = Source("l", "Left controller", ControllerAttachment.BuiltIn);
        var right = Source("r", "Right controller", ControllerAttachment.BuiltIn);

        Assert.Null(ControllerSourceSelection.Resolve([left, right], null));
    }

    [Fact]
    public void NeverAutoSelectsTheAdaptersOwnOutput()
    {
        // A PicoSwitch plugged into this PC enumerates as whatever it is
        // pretending to be. Selecting it feeds the adapter's own output back in
        // as its input -- a silent loop that looks like "the sticks drift".
        var echo = Source("echo", "Switch 2 Pro Controller",
                          vendor: 0x057E, product: 0x2069, mayBeAdapter: true);

        Assert.Null(ControllerSourceSelection.Resolve([echo], null));
        Assert.Contains("adapter's own output",
                        ControllerSourceSelection.UnresolvedReason([echo], null));
    }

    [Fact]
    public void PicksTheRealControllerOverTheAdaptersEcho()
    {
        // With the adapter on USB and a real pad connected, this is not
        // ambiguous at all -- one of the two is known-bad.
        var echo = Source("echo", "Switch 2 Pro Controller",
                          vendor: 0x057E, product: 0x2069, mayBeAdapter: true);
        var real = Source("real", "Xbox Wireless Controller");

        Assert.Same(real, ControllerSourceSelection.Resolve([echo, real], null));
    }

    [Fact]
    public void AnExplicitChoiceOverridesEveryHeuristic()
    {
        // Including the adapter itself: a user who selects it has made a
        // decision the app has no business overriding, and someone may well be
        // testing exactly that loop deliberately.
        var echo = Source("echo", "Switch 2 Pro Controller",
                          vendor: 0x057E, product: 0x2069, mayBeAdapter: true);
        var builtIn = Source("built-in", "Built-in controller", ControllerAttachment.BuiltIn);

        Assert.Same(echo, ControllerSourceSelection.Resolve([echo, builtIn], "echo"));
    }

    [Fact]
    public void FallsBackWhenTheRememberedControllerIsGone()
    {
        var only = Source("a");
        Assert.Same(only, ControllerSourceSelection.Resolve([only], "unplugged"));
    }

    [Fact]
    public void ANonGamepadClassDeviceIsOfferedButNotChosenSilently()
    {
        // Measured 2026-09-02: a Switch 2 Pro Controller gives
        // RawGameController count 1 and Gamepad count 0. It is a real device and
        // stays visible, but Windows will not hand us its buttons in a standard
        // layout, so selecting it automatically would produce a source that
        // looks connected and sends nothing.
        var raw = Source("raw", "Switch 2 Pro Controller", gamepadClass: false);

        Assert.False(raw.CanDrive);
        Assert.Contains("standard layout", raw.UnreadableReason);
        Assert.Null(ControllerSourceSelection.Resolve([raw], null));

        // ...but an explicit choice still stands, and the reason is shown.
        Assert.Same(raw, ControllerSourceSelection.Resolve([raw], "raw"));
    }

    [Fact]
    public void SaysSoWhenNothingIsConnected()
    {
        Assert.Null(ControllerSourceSelection.Resolve([], null));
        Assert.Contains("No controller", ControllerSourceSelection.UnresolvedReason([], null));
    }

    [Fact]
    public void PassesTheSharedExclusionRuleThrough()
    {
        // A device with no identity and no capability is the virtual-keyboard
        // shape the shared rule exists to exclude. Windows must not re-decide
        // that, only report it.
        var virtualish = Source("v", "Virtual", vendor: 0, product: 0,
                                buttons: false, axes: false);

        Assert.False(virtualish.IsUsable);
        Assert.Null(ControllerSourceSelection.Resolve([virtualish], null));
        Assert.Equal(virtualish.Candidate.ExclusionReason, virtualish.UnreadableReason);
    }

    [Fact]
    public void AttachmentLabelsAreProductLanguage()
    {
        Assert.Equal("Built-in",
            Source("a", attachment: ControllerAttachment.BuiltIn).AttachmentLabel);
        Assert.Equal("Connected",
            Source("b", attachment: ControllerAttachment.External).AttachmentLabel);

        // Windows not saying is not a third thing a user needs to reason about.
        Assert.Equal("Connected",
            Source("c", attachment: ControllerAttachment.Unknown).AttachmentLabel);
    }
}

/// <summary>
/// Device instance id parsing, against the real strings Windows produced on this
/// machine.
/// </summary>
public sealed class ControllerIdentityParsingTests
{
    [Theory]
    // USB, captured from this bench.
    [InlineData(@"HID\VID_057E&PID_2069&MI_00\8&2ca74762&0&0000", 0x057E, 0x2069)]
    [InlineData(@"HID\VID_045E&PID_028E&IG_00\8&2E061D8F&0&0000", 0x045E, 0x028E)]
    [InlineData(@"HID\VID_054C&PID_0CE6&MI_03\8&1BE75E48&0&0000", 0x054C, 0x0CE6)]
    // Bluetooth LE spells the vendor with an eight-digit namespaced form.
    [InlineData(@"BTHLEDEVICE\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B22_REV&0523_14CB6583B604\B&15FBDCB7&1&0016", 0x045E, 0x0B22)]
    // Bluetooth Classic HID.
    [InlineData(@"HID\{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&05C4\B&3088AE1&0&0000", 0x054C, 0x05C4)]
    public void ParsesRealDeviceInstanceIds(string instanceId, int vendor, int product)
    {
        Assert.True(WindowsControllerSourceCatalog.TryParseIdentity(instanceId, out var identity));
        Assert.Equal(vendor, identity.Vendor);
        Assert.Equal(product, identity.Product);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(@"USB\ROOT_HUB30\4&2f9bb2e5&0")]
    [InlineData(@"HID\VID_05&PID_20\x")]   // too few digits to be an identity
    public void RejectsWhatIsNotAnIdentity(string? instanceId)
    {
        Assert.False(WindowsControllerSourceCatalog.TryParseIdentity(instanceId, out _));
    }

    [Fact]
    public void AdapterPersonalityKeysAreTheManagementWireNames()
    {
        // The adapter reports "jcl", not "joycon2l". A guessed key would never
        // match, and the failure mode is the worst kind: the adapter-echo guard
        // would look present in the code and quietly do nothing, so the loop it
        // exists to prevent would happen anyway.
        //
        // Every controller personality must be covered; Config is not a
        // controller and correctly is not.
        foreach (var personality in Enum.GetValues<Personality>())
        {
            if (personality is Personality.Config or Personality.Unknown)
            {
                continue;
            }

            var wire = Personalities.WireName(personality);
            Assert.True(
                WindowsControllerSourceCatalog.PersonalityProductId(wire) is not null,
                $"personality '{wire}' has no USB product id in the adapter-echo guard");
        }

        Assert.Null(WindowsControllerSourceCatalog.PersonalityProductId("joycon2l"));
        Assert.Equal((ushort)0x2069, WindowsControllerSourceCatalog.PersonalityProductId("pro2"));
    }
}
