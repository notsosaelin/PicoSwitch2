using System.Runtime.InteropServices;

namespace PicoSwitch.Companion.Linux.Display;

/// <summary>
/// The UI scale for Steam Game Mode (gamescope), where nothing else sets one.
/// </summary>
/// <remarks>
/// <para>
/// A desktop session tells Avalonia its scale (GNOME and KDE publish Xft.dpi;
/// the Ally's desktop runs at 1.5x). gamescope's Xwayland publishes nothing, so
/// Avalonia draws at 1x: on a 7-inch 1920x1080 handheld the whole UI came out
/// about a third too small to use (owner, 2026-11-08: "horrible").
/// </para>
/// <para>
/// The scale is PHYSICAL, from the size gamescope reports for its monitor
/// (RandR millimetres, the panel's real size: 150x90 mm on the ROG Xbox Ally X).
/// One UI unit is made the same physical size as one dp on the AYN Odin 2 Mini,
/// which the owner reviewed the Android app on and named as the reference:
/// 1920x1080 at density 369 (2.31x, 832x468 dp) on a 5-inch panel, about
/// <see cref="UnitsPerInch"/> units per inch. On the Ally that is 1.7x
/// (about 1130x635 units), so a button is the size it is on the Odin and the
/// larger panel shows more page, never bigger chrome.
/// </para>
/// <para>
/// A large display (a TV through a dock) has a low pixel density and would
/// compute below 1x. There the floor is resolution-proportional instead: never
/// more than 1080 units of height, so a 4K TV gets 2x and a 1080p one 1x.
/// </para>
/// <para>
/// Applied through Avalonia's own AVALONIA_GLOBAL_SCALE_FACTOR before the
/// platform starts, so every window, popup and flyout agrees; a value the user
/// set (or AVALONIA_SCREEN_SCALE_FACTORS) is left alone.
/// </para>
/// </remarks>
public static class GamescopeScale
{
    /// <summary>Odin 2 Mini dp per inch: 1920x1080 is 440.6 ppi at 5.0 inches, over density 2.306.</summary>
    public const double UnitsPerInch = 191;

    /// <summary>The most units of height a large display is given (see remarks).</summary>
    public const double MaxUnitsHigh = 1080;

    public const double Min = 1;

    public const double Max = 4;

    public const string GlobalScaleVariable = "AVALONIA_GLOBAL_SCALE_FACTOR";

    public const string ScreenScalesVariable = "AVALONIA_SCREEN_SCALE_FACTORS";

    /// <summary>SteamOS / Bazzite Game Mode: gamescope sets these for what it launches.</summary>
    public static bool UnderGamescope(Func<string, string?> environment) =>
        environment("GAMESCOPE_WAYLAND_DISPLAY") is { Length: > 0 } ||
        string.Equals(environment("XDG_CURRENT_DESKTOP"), "gamescope", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// The scale for a monitor of this many pixels and millimetres, in steps of
    /// 0.05. Millimetres of zero or an implausible density (no EDID, a
    /// projector) fall back to the resolution rule alone.
    /// </summary>
    public static double For(int widthPx, int heightPx, int widthMm, int heightMm)
    {
        if (widthPx <= 0 || heightPx <= 0)
        {
            return Min;
        }

        var byResolution = heightPx / MaxUnitsHigh;
        var scale = byResolution;
        if (widthMm > 0 && heightMm > 0)
        {
            var ppi = widthPx / (widthMm / 25.4);
            if (ppi is >= 50 and <= 1000)
            {
                scale = Math.Max(ppi / UnitsPerInch, byResolution);
            }
        }

        return Math.Clamp(Math.Round(scale * 20) / 20, Min, Max);
    }

    /// <summary>
    /// Sets the scale for this process when it runs under gamescope and nothing
    /// set one. Returns a line for the log, or null when it did nothing.
    /// </summary>
    public static string? ApplyIfUnderGamescope()
    {
        if (!OperatingSystem.IsLinux() || !UnderGamescope(Environment.GetEnvironmentVariable))
        {
            return null;
        }

        if (Environment.GetEnvironmentVariable(GlobalScaleVariable) is { Length: > 0 } ||
            Environment.GetEnvironmentVariable(ScreenScalesVariable) is { Length: > 0 })
        {
            return "gamescope: a scale is already set in the environment; left alone";
        }

        if (!X11Monitor.TryReadPrimary(out var monitor))
        {
            return "gamescope: no X11 monitor to measure; staying at 1x";
        }

        var scale = For(monitor.WidthPx, monitor.HeightPx, monitor.WidthMm, monitor.HeightMm);
        Environment.SetEnvironmentVariable(GlobalScaleVariable,
            scale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture));
        return $"gamescope: monitor {monitor.WidthPx}x{monitor.HeightPx} px, {monitor.WidthMm}x{monitor.HeightMm} mm -> " +
               $"scale {scale:0.##} ({monitor.WidthPx / scale:0}x{monitor.HeightPx / scale:0} units)";
    }
}

/// <summary>One RandR monitor: its pixels and its physical size.</summary>
public readonly record struct MonitorSize(int WidthPx, int HeightPx, int WidthMm, int HeightMm);

/// <summary>
/// Reads the first active RandR monitor of $DISPLAY through libXrandr, the
/// same libraries Avalonia's X11 backend loads. gamescope's Xwayland reports
/// one monitor at the nested resolution with the real panel's millimetres; its
/// core screen size is a fixed 96 dpi fiction (508x286 mm on the Ally), so the
/// core protocol cannot be used.
/// </summary>
internal static class X11Monitor
{
    public static bool TryReadPrimary(out MonitorSize monitor)
    {
        monitor = default;
        IntPtr display;
        try
        {
            display = XOpenDisplay(IntPtr.Zero);
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }

        if (display == IntPtr.Zero)
        {
            return false;
        }

        try
        {
            var monitors = XRRGetMonitors(display, XDefaultRootWindow(display), getActive: 1, out var count);
            if (monitors == IntPtr.Zero)
            {
                return false;
            }

            try
            {
                if (count <= 0)
                {
                    return false;
                }

                // XRRMonitorInfo (64-bit): Atom name @0; Bool primary @8;
                // Bool automatic @12; int noutput @16; int x @20, y @24,
                // width @28, height @32; int mwidth @36, mheight @40;
                // RROutput* outputs @48. 56 bytes. The primary one if marked.
                const int Size = 56;
                var chosen = 0;
                for (var i = 0; i < count; i++)
                {
                    if (Marshal.ReadInt32(monitors, i * Size + 8) != 0)
                    {
                        chosen = i;
                        break;
                    }
                }

                var at = chosen * Size;
                monitor = new MonitorSize(
                    Marshal.ReadInt32(monitors, at + 28),
                    Marshal.ReadInt32(monitors, at + 32),
                    Marshal.ReadInt32(monitors, at + 36),
                    Marshal.ReadInt32(monitors, at + 40));
                return monitor.WidthPx > 0 && monitor.HeightPx > 0;
            }
            finally
            {
                XRRFreeMonitors(monitors);
            }
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
        finally
        {
            XCloseDisplay(display);
        }
    }

    [DllImport("libX11.so.6")]
    private static extern IntPtr XOpenDisplay(IntPtr name);

    [DllImport("libX11.so.6")]
    private static extern int XCloseDisplay(IntPtr display);

    [DllImport("libX11.so.6")]
    private static extern IntPtr XDefaultRootWindow(IntPtr display);

    [DllImport("libXrandr.so.2")]
    private static extern IntPtr XRRGetMonitors(IntPtr display, IntPtr window, int getActive, out int count);

    [DllImport("libXrandr.so.2")]
    private static extern void XRRFreeMonitors(IntPtr monitors);
}
