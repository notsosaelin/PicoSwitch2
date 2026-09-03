using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// What is worth putting on the wire, and how often.
///
/// Pure and separate from <see cref="ControllerLinkService"/> because this is the
/// decision that determines whether a stick can bury a button press, and a
/// decision buried inside a timer loop is one nobody can test.
///
/// ## Why a threshold and two rates
///
/// Nothing below this app can collapse a frame once it has been handed over. The
/// bounded writer coalesces only what it has not yet submitted; a GATT write
/// "completes" when the Windows driver ACCEPTS the buffer, not when the radio
/// sends it, so anything beyond the link's real capacity queues inside the
/// driver and is replayed IN ORDER. That is what makes an over-sending stick
/// feel like a backlog rather than like latency, and why button presses sit
/// behind it.
///
/// The traffic is not local either: on this bench the PC reaches the adapter over
/// the same radio the controller itself is paired to, so every needless frame
/// competes for airtime with the controller's own reports.
///
/// So two rules. A digital edge is discrete, rare and felt immediately, and is
/// never filtered or delayed. Analog motion is continuous and would otherwise set
/// the rate on its own, so it must both clear a movement threshold and respect a
/// ceiling.
/// </summary>
public static class ControllerLinkSendPolicy
{
    /// <summary>
    /// How far an axis must move before it is worth a frame, out of 0..255.
    ///
    /// An analog stick is never still — its low bits flicker from sensor noise
    /// even untouched, so byte-exact comparison reports a change on very nearly
    /// every sample and an idle stick sets the send rate.
    ///
    /// 2/255 is under 1% of full travel: smaller than the deadzone of any stick
    /// this will meet and far below what a player can see. Genuine movement
    /// crosses it at once, and because the comparison is against the LAST SENT
    /// state rather than the previous sample, a slow drift still trips as soon as
    /// it has actually moved instead of creeping the whole range unreported.
    /// </summary>
    public const int AnalogEpsilon = 2;

    /// <summary>
    /// Did a discrete control change? Never filtered, never rate-limited by the
    /// analog ceiling.
    /// </summary>
    public static bool DigitalChanged(ControllerState current, ControllerState previous) =>
        !current.Buttons.Equals(previous.Buttons) ||
        current.DpadUp != previous.DpadUp ||
        current.DpadRight != previous.DpadRight ||
        current.DpadDown != previous.DpadDown ||
        current.DpadLeft != previous.DpadLeft;

    /// <summary>
    /// Did any axis move enough to be worth a frame?
    ///
    /// Compares the ENCODED 0..255 values the wire carries, so it asks exactly
    /// the question that matters: would the adapter see a different byte, by
    /// enough for anyone to notice?
    /// </summary>
    public static bool AnalogMoved(ControllerState current, ControllerState previous) =>
        Moved(current.LeftX, previous.LeftX) ||
        Moved(current.LeftY, previous.LeftY) ||
        Moved(current.RightX, previous.RightX) ||
        Moved(current.RightY, previous.RightY) ||
        Moved(current.LeftTrigger, previous.LeftTrigger) ||
        Moved(current.RightTrigger, previous.RightTrigger);

    private static bool Moved(int current, int previous) =>
        Math.Abs(current - previous) >= AnalogEpsilon;
}
