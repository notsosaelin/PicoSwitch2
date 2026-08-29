using Microsoft.UI.Xaml.Controls;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Gamepad.
///
/// Phase 0 destination: the shell, its navigation and its window chrome are
/// real; the content is not. Kept as an explicit placeholder rather than an
/// empty page so a build of this skeleton says what it is instead of looking
/// broken.
/// </summary>
public sealed partial class ControllerPage : Page
{
    public ControllerPage() => InitializeComponent();
}
