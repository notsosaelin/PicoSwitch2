using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using Avalonia.Styling;
using Avalonia.Threading;
using PicoSwitch.Companion.Linux.App;
using PicoSwitch.Companion.Linux.App.Shell;

// Renders every page, in both themes, across a matrix of real screen
// geometries. Sizes are LOGICAL (device pixels divided by the display scale),
// because that is the space the layout actually gets: a 1920x1080 handheld at
// 1.5x lays out exactly like a 1280x720 one.
//
//   <out-dir> [--pages a,b] [--sizes a,b] [--themes dark,light] [--amiibo <dir>]
//
// The Touch Gamepad is not a page; --pages touch-pro2,touch-gamecube,
// touch-joyconl,touch-joyconr opens it in play mode with that controller's
// layout. A -menu suffix right-clicks to open its menu; -edit enters the editor
// and drags the first control 60 px right with real (headless) pointer input,
// so the capture shows the selection, the guides and the toolbar. The harness has no adapter, so the personality is SET here rather
// than confirmed; the capture reviews the drawing, not personality discovery.
//
// A page name may end in :<tab>, a zero-based tab index (kbm:1 is Keyboard &
// Mouse on its Mouse tab, adapter:0 is Adapter on Console).
//
// --amiibo seeds the (scratch) library with every .bin in a directory, such as
// tests/fixtures/amiibo, so the browser is reviewed populated as well as empty.
var sizes = new Dictionary<string, (int Width, int Height, string What)>
{
    ["720p"] = (1280, 720, "1920x1080 handheld at 1.5x (ROG Ally / Ally X desktop), 720p handhelds"),
    ["800p"] = (1280, 800, "16:10 handhelds: Steam Deck, Legion Go S at 1.5x"),
    ["1080p"] = (1920, 1080, "1080p monitor, or a handheld in Game Mode at 1x"),
    ["1440p"] = (2560, 1440, "1440p monitor at 1x, maximised"),
    ["ultrawide"] = (3440, 1440, "21:9 ultrawide, maximised"),
    ["4x3"] = (1024, 768, "4:3 panel or a small window"),
    ["portrait"] = (800, 1280, "portrait tablet / rotated handheld"),
    ["minimum"] = (600, 460, "the smallest window the app allows, rounded up"),
    ["short"] = (832, 468, "a landscape handheld under the short-window height (Android at 832x468 dp)"),
    ["tall"] = (1280, 2400, "a whole page, to review content below the fold"),
};

var output = Path.GetFullPath(args.FirstOrDefault(a => !a.StartsWith("--", StringComparison.Ordinal)) ?? "screenshots");
string[] Option(string name, IEnumerable<string> fallback)
{
    var index = Array.IndexOf(args, name);
    return index >= 0 && index + 1 < args.Length
        ? args[index + 1].Split(',', StringSplitOptions.RemoveEmptyEntries)
        : [.. fallback];
}

var pages = Option("--pages", MainWindow.AllPages);
var sizeNames = Option("--sizes", sizes.Keys);
var themeNames = Option("--themes", ["dark", "light"]);
var keys = Option("--keys", []);
var padSteps = Option("--pad", []);
var placement = Option("--placement", []).FirstOrDefault() is { } placementKey
    ? PicoSwitch.Companion.Services.Presentation.NavPlacements.FromKey(placementKey)
    : (PicoSwitch.Companion.Services.Presentation.NavPlacement?)null;

Directory.CreateDirectory(output);

// Keep the run away from the user's real companion data.
// Before anything touches AppServices: the platform reads it once.
var data = Path.Combine(output, ".data");
if (Directory.Exists(data))
{
    Directory.Delete(data, recursive: true);
}

Environment.SetEnvironmentVariable(AppServices.DataDirectoryVariable, data);

AppBuilder.Configure<App>()
    .UseSkia()
    .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
    .WithInterFont()
    .SetupWithoutStarting();

var seed = Array.IndexOf(args, "--amiibo") is var at and >= 0 && at + 1 < args.Length ? args[at + 1] : null;
if (seed is not null)
{
    var result = AppServices.AmiiboLibrary.ImportMany(
        [.. Directory.EnumerateFiles(seed, "*.bin").Select(path => new PicoSwitch.Companion.Services.AmiiboImportSource(Path.GetFileName(path), File.ReadAllBytes(path)))]);
    Console.WriteLine(result.Summary);
}

foreach (var themeName in themeNames)
{
    Application.Current!.RequestedThemeVariant = themeName == "light" ? ThemeVariant.Light : ThemeVariant.Dark;
    foreach (var sizeName in sizeNames)
    {
        var (width, height, _) = sizes[sizeName];
        var window = new MainWindow { Width = width, Height = height, FillsScreen = false, Animate = false };
        if (placement is { } chosenPlacement)
        {
            window.Placement = chosenPlacement;
        }

        window.Show();
        foreach (var page in pages)
        {
            var touch = TouchPersonality(page);
            if (touch is { } personality)
            {
                window.ShowTouchGamepad();
                Dispatcher.UIThread.RunJobs();
                AppServices.TouchGamepad.SetPersonality(personality);
                Dispatcher.UIThread.RunJobs();
                if (page.EndsWith("-menu", StringComparison.Ordinal) || page.EndsWith("-edit", StringComparison.Ordinal))
                {
                    window.MouseDown(new Point(width / 2d, height / 2d), MouseButton.Right);
                    window.MouseUp(new Point(width / 2d, height / 2d), MouseButton.Right);
                    Dispatcher.UIThread.RunJobs();
                }

                if (page.EndsWith("-edit", StringComparison.Ordinal))
                {
                    window.GetVisualDescendants().OfType<Button>().First(b => b.Content as string == "Edit")
                        .RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    Dispatcher.UIThread.RunJobs();
                    var control = AppServices.TouchGamepad.State.Value.Resolved.Controls.OrderByDescending(c => c.Spec.ZIndex).First();
                    var from = new Point(control.CenterX, control.CenterY);
                    window.MouseDown(from, MouseButton.Left);
                    for (var step = 1; step <= 6; step++)
                    {
                        window.MouseMove(from + new Point(10 * step, 0));
                    }

                    window.MouseUp(from + new Point(60, 0), MouseButton.Left);
                    Dispatcher.UIThread.RunJobs();
                    Console.WriteLine($"edit: dragged {control.Id}; dirty={AppServices.TouchGamepad.State.Value.Dirty}");
                }
            }
            else
            {
                // "play+nav": the page with the navigation out.
                var withNav = page.EndsWith("+nav", StringComparison.Ordinal);
                var parts = page.Replace("+nav", "").Split(':');
                window.Navigate(parts[0]);
                window.SetNavigationShown(withNav);
                Dispatcher.UIThread.RunJobs();

                // --keys Left,Escape: drive the shell as a keyboard (or a
                // controller, whose steps arrive as these keys) before capture.
                // --pad Down,A,B: the same through the controller path, as SDL
                // frames into the input session the shell reads.
                foreach (var name in padSteps)
                {
                    var source = new PicoSwitch.Bridge.Core.ControllerSourceIdentity("harness", "Harness pad", 0x057E, 0x2069);
                    var none = PicoSwitch.Bridge.Core.ControllerButtonSet.Empty;
                    var neutral = PicoSwitch.Bridge.Core.AnalogFrame.Neutral;
                    var press = name.ToLowerInvariant() switch
                    {
                        "up" => (none, neutral with { Dpad = new PicoSwitch.Bridge.Core.DpadState(true, false, false, false) }),
                        "right" => (none, neutral with { Dpad = new PicoSwitch.Bridge.Core.DpadState(false, true, false, false) }),
                        "down" => (none, neutral with { Dpad = new PicoSwitch.Bridge.Core.DpadState(false, false, true, false) }),
                        "left" => (none, neutral with { Dpad = new PicoSwitch.Bridge.Core.DpadState(false, false, false, true) }),
                        _ => (none.With(Enum.Parse<PicoSwitch.Bridge.Core.ControllerButton>(name, ignoreCase: true)), neutral),
                    };
                    AppServices.PadInput.ApplyPhysicalFrame(source, press.Item1, press.Item2);
                    Dispatcher.UIThread.RunJobs();
                    AppServices.PadInput.ApplyPhysicalFrame(source, none, neutral);
                    for (var i = 0; i < 4; i++)
                    {
                        Dispatcher.UIThread.RunJobs();
                        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
                    }

                    var focused = window.FocusManager?.GetFocusedElement();
                    Console.WriteLine($"pad {name}: focus={(focused as Control)?.GetType().Name} '{(focused is Control c ? Avalonia.Automation.AutomationProperties.GetName(c) : null)}' nav={window.NavigationShown} page={window.CurrentPage}");
                }

                foreach (var name in keys)
                {
                    var key = Enum.Parse<Key>(name, ignoreCase: true);
                    window.KeyPress(key, RawInputModifiers.None, PhysicalKey.None, null);
                    window.KeyRelease(key, RawInputModifiers.None, PhysicalKey.None, null);
                    for (var i = 0; i < 4; i++)
                    {
                        Dispatcher.UIThread.RunJobs();
                        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
                    }

                    var focused = window.FocusManager?.GetFocusedElement();
                    Console.WriteLine($"key {key}: focus={(focused as Control)?.GetType().Name} '{(focused is Control c ? Avalonia.Automation.AutomationProperties.GetName(c) : null)}' nav={window.NavigationShown}");
                }
                if (parts.Length > 1)
                {
                    Dispatcher.UIThread.RunJobs();
                    window.GetVisualDescendants().OfType<TabBar>().First(bar => bar.IsEffectivelyVisible)
                        .Select(int.Parse(parts[1]));
                }
            }

            for (var i = 0; i < 6; i++)
            {
                Dispatcher.UIThread.RunJobs();
                AvaloniaHeadlessPlatform.ForceRenderTimerTick();
            }

            var file = Path.Combine(output, $"{page.Replace(':', '-').Replace('+', '-')}-{sizeName}-{themeName}{(placement is { } p ? "-" + PicoSwitch.Companion.Services.Presentation.NavPlacements.Key(p) : "")}.png");
            window.CaptureRenderedFrame()?.Save(file);
            Console.WriteLine(file);
            if (touch is not null)
            {
                window.HideTouchGamepad();
            }
        }

        window.Close();
    }
}

return 0;

static PicoSwitch.Management.Personality? TouchPersonality(string page) => page.Replace("-menu", "").Replace("-edit", "") switch
{
    "touch-pro2" => PicoSwitch.Management.Personality.Pro2,
    "touch-gamecube" => PicoSwitch.Management.Personality.GameCube,
    "touch-joyconl" => PicoSwitch.Management.Personality.JoyConLeft,
    "touch-joyconr" => PicoSwitch.Management.Personality.JoyConRight,
    _ => null,
};
