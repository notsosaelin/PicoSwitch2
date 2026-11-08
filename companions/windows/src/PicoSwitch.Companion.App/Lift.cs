using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace PicoSwitch.Companion.App;

/// <summary>
/// THE CONTROLLER CURSOR (companion-shell-layout.md, "Play"): Play's tiles and
/// actions and the navigation's items lift (scale, over the system focus
/// visual's outline) only while a keyboard or gamepad drives focus. A pointer
/// focus is FocusState.Pointer and never lifts, so a click leaves nothing
/// raised behind it.
/// </summary>
internal static class Lift
{
    private static readonly Vector3 Raised = new(1.05f, 1.05f, 1f);

    public static void Apply(Control control)
    {
        control.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(120) };
        control.GotFocus += (_, _) =>
        {
            if (control.FocusState == FocusState.Keyboard)
            {
                control.CenterPoint = new Vector3((float)control.ActualWidth / 2, (float)control.ActualHeight / 2, 0);
                control.Scale = Raised;
            }
        };
        control.LostFocus += (_, _) => control.Scale = Vector3.One;
    }
}
