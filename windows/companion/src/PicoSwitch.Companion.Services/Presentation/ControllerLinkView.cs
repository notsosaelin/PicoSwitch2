using PicoSwitch.Companion.Windows.Bluetooth;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// What the Gamepad page says about Controller Link on this PC.
///
/// ## The rule this page is built around
///
/// **Name the specific thing that is missing.** `WINDOWS_PASS.md` §31 Phase 6
/// "If the gate fails" requires the page to name "the platform limitation and the
/// radio capability that was missing", and §33.2 accepts a negative §14.5 result
/// only if the UI "explains the platform limitation *specifically*". "Not
/// supported on Windows" would satisfy neither, and it would be false: two of the
/// six open questions were answered positively, and the blocker is measured per
/// machine rather than being a property of the operating system.
///
/// ## Why the text is here and not in XAML
///
/// The same reason as <see cref="SectionAvailability"/>: a sentence in markup
/// cannot be asserted, and this is a sentence the project has committed to
/// getting right. Every branch below is covered by a test that runs without a
/// radio.
/// </summary>
public sealed record ControllerLinkView(
    ControllerLinkStep Step,
    string Headline,
    string Explanation,
    string? RadioLine,
    bool ShowRecheck,
    bool Measuring = false)
{
    /// <summary>Before anything has been measured.</summary>
    public static ControllerLinkView Idle => new(
        ControllerLinkStep.Unknown,
        "Using this PC as a controller",
        "PicoSwitch2 can accept a controller from a phone over Bluetooth. Whether a Windows PC " +
        "can do the same depends on the Bluetooth radio, so it has to be measured rather than " +
        "assumed. Check to find out what this PC's radio can do.",
        RadioLine: null,
        ShowRecheck: true);

    public static ControllerLinkView Busy => Idle with
    {
        Explanation = "Asking Windows to advertise as a Bluetooth peripheral…",
        ShowRecheck = false,
        Measuring = true,
    };

    public static ControllerLinkView Of(ControllerLinkCapability capability)
    {
        var radio = DescribeRadio(capability);

        return capability.Step switch
        {
            ControllerLinkStep.NoRadio => new ControllerLinkView(
                capability.Step,
                "No Bluetooth radio",
                "Windows did not report a Bluetooth radio on this PC. Everything the companion " +
                "does needs one, including managing the adapter.",
                radio,
                ShowRecheck: true),

            ControllerLinkStep.NoPeripheralRole => new ControllerLinkView(
                capability.Step,
                "This radio cannot act as a Bluetooth peripheral",
                "Sending controller input to the adapter needs this PC to advertise itself as a " +
                "Bluetooth Low Energy peripheral, and this radio reports that it does not support " +
                "that role. A different Bluetooth adapter would be needed. Nothing else in the " +
                "companion is affected — managing your PicoSwitch2 uses the ordinary central " +
                "role, which this radio does support.",
                radio,
                ShowRecheck: true),

            ControllerLinkStep.ServiceRefused => new ControllerLinkView(
                capability.Step,
                "Windows would not publish the Bluetooth service",
                "This PC could not publish the Bluetooth service that carries controller input. " +
                "That is unusual — on the hardware this was developed against, Windows publishes " +
                "it without complaint — so it is worth reporting along with your Bluetooth " +
                "adapter model and driver version.",
                radio,
                ShowRecheck: true),

            // THE MEASURED CASE. Everything here is a fact from 2026-08-31, and
            // it is deliberately specific: a user who reads this should be able to
            // tell it apart from "the feature is unfinished".
            ControllerLinkStep.AdvertisingRefused => new ControllerLinkView(
                capability.Step,
                "This PC's Bluetooth radio will not advertise as a controller",
                "The companion can publish the Bluetooth service that carries controller input — " +
                "that part works. What this radio refuses is the connectable advertisement the " +
                "adapter would have to find, and it refuses it without giving a reason. It is not " +
                "specific to controller input: a service with no meaning at all is refused the " +
                "same way, while ordinary non-connectable Bluetooth advertising works normally.\n\n" +
                "This is a limitation of the Bluetooth radio or its driver, not of the adapter " +
                "and not of your PicoSwitch2 setup. Everything else in the companion works " +
                "normally. If you try another Bluetooth adapter, check again here — the answer is " +
                "measured per radio, not assumed.",
                radio,
                ShowRecheck: true),

            ControllerLinkStep.Advertising => new ControllerLinkView(
                capability.Step,
                "This radio can advertise — but Controller Link is not built yet",
                "This PC's Bluetooth radio does what the hardware the companion was developed " +
                "against would not: it advertises as a connectable Bluetooth peripheral. That " +
                "removes the blocker that stopped this feature being built.\n\n" +
                "It does not mean controller input works. One question was never answered, " +
                "because no radio had got this far: whether the adapter accepts a controller that " +
                "cannot publish the Device Information Service, which Windows does not allow an " +
                "application to publish. If you are seeing this, the experiment is worth resuming " +
                "— please report it with your Bluetooth adapter model and driver version.",
                radio,
                ShowRecheck: true),

            _ => Idle,
        };
    }

    /// <summary>
    /// The radio's own claim, shown next to what it actually did.
    ///
    /// Shown even — especially — when the two disagree. "Reports support for the
    /// peripheral role" beside "will not advertise" is the single most useful
    /// line on this page for anyone trying to work out whether a different
    /// adapter would help, and hiding the contradiction to look tidy would
    /// destroy exactly that.
    /// </summary>
    private static string? DescribeRadio(ControllerLinkCapability capability)
    {
        if (capability.Step == ControllerLinkStep.Unknown)
        {
            return null;
        }

        var claim = capability.ClaimsPeripheralRole
            ? "reports support for the Bluetooth peripheral role"
            : "does not report support for the Bluetooth peripheral role";

        var contradiction = capability is
        {
            ClaimsPeripheralRole: true,
            Step: ControllerLinkStep.AdvertisingRefused or ControllerLinkStep.ServiceRefused,
        }
            ? ", but does not perform it"
            : string.Empty;

        var address = capability.RadioAddress is { Length: > 0 } value ? $" ({value})" : string.Empty;
        return $"This PC's Bluetooth radio{address} {claim}{contradiction}.";
    }
}
