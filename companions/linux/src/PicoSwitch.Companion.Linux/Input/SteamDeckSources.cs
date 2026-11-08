using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Companion.Linux.Input;

/// <summary>
/// Which hidraw nodes are read as Steam Deck-format controllers, and which SDL
/// pads are their mirrors. Pure, so the rules are unit-tested.
///
/// ## The rule
///
/// A Valve-vendor hidraw node is probed when SDL will not drive it: any product
/// except the Steam Deck's own 0x1205, which SDL's HIDAPI driver handles (with
/// back buttons and motion) because the source sets
/// <c>SDL_JOYSTICK_HIDAPI_STEAMDECK</c>. It is listed only if it actually
/// streams Steam Deck controller-state reports (<see cref="SteamDeckReport.IsState"/>),
/// so a Valve device with another report format is never misread. No rule
/// names a handheld: whatever emulates the Deck format under a Valve id is
/// covered.
///
/// ## Mirrors
///
/// An SDL pad with the same Valve vendor and product as a listed node is the
/// same physical controls seen again, in practice Steam Input's virtual pad,
/// which in Game Mode copies the underlying controller's ids (observed on the
/// Ally 2026-11-06: "Steam Virtual Gamepad" 28de:12fd). It is listed as
/// excluded, like any Steam virtual pad, and is used only for rumble and battery
/// while its node is the input.
/// </summary>
internal static class SteamDeckSources
{
    public static bool IsCandidate(ushort vendor, ushort product) =>
        vendor == SdlGamepadMapping.ValveVendor && product != SdlGamepadMapping.SteamDeckProduct;

    public static HidrawNode? MirrorOf(ushort vendor, ushort product, IReadOnlyList<HidrawNode> deckNodes) =>
        vendor == SdlGamepadMapping.ValveVendor
            ? deckNodes.FirstOrDefault(node => node.Vendor == vendor && node.Product == product)
            : null;

    /// <summary>
    /// Stable across restarts and Desktop/Game Mode switches (the hidraw index
    /// is not), so a remembered choice survives both.
    /// </summary>
    public static string Descriptor(HidrawNode node) =>
        $"hidraw:{node.Vendor:x4}:{node.Product:x4}:{node.Uniq ?? node.Name}";
}

/// <summary>
/// One open Steam Deck-format hidraw node, read on its own thread. Reports are
/// decoded and handed to the owner as they arrive; there is no polling clock.
/// </summary>
internal sealed class SteamDeckReader : IDisposable
{
    public delegate void StateHandler(SteamDeckReader reader, in SteamDeckReport.State state);

    private const int PollMillis = 50;

    private readonly Hidraw.Handle handle;
    private readonly StateHandler onState;
    private readonly Action<SteamDeckReader> onGone;
    private readonly Thread thread;
    private volatile bool stopping;
    private volatile bool alive = true;

    private SteamDeckReader(
        HidrawNode node,
        ControllerSourceIdentity identity,
        Hidraw.Handle handle,
        StateHandler onState,
        Action<SteamDeckReader> onGone)
    {
        Node = node;
        Identity = identity;
        this.handle = handle;
        this.onState = onState;
        this.onGone = onGone;
        thread = new Thread(Run) { IsBackground = true, Name = "deck-hidraw" };
    }

    public HidrawNode Node { get; }

    public ControllerSourceIdentity Identity { get; }

    public bool Alive => alive;

    // Owned by the reader thread: change detection for frame publication.
    public bool Published { get; set; }

    public ControllerButtonSet LastButtons { get; set; } = ControllerButtonSet.Empty;

    public AnalogFrame LastAnalog { get; set; } = AnalogFrame.Neutral;

    public static SteamDeckReader? Start(
        HidrawNode node,
        ControllerSourceIdentity identity,
        StateHandler onState,
        Action<SteamDeckReader> onGone,
        out string? error)
    {
        var handle = Hidraw.Open(node.DevicePath, out error);
        if (handle is null)
        {
            return null;
        }

        var reader = new SteamDeckReader(node, identity, handle, onState, onGone);
        reader.thread.Start();
        return reader;
    }

    private void Run()
    {
        Span<byte> buffer = stackalloc byte[256];
        try
        {
            while (!stopping)
            {
                var count = handle.Read(buffer, PollMillis);
                if (count < 0)
                {
                    break;
                }

                if (count > 0 && SteamDeckReport.TryParse(buffer[..count], out var state))
                {
                    onState(this, state);
                }
            }
        }
        finally
        {
            alive = false;
        }

        if (!stopping)
        {
            onGone(this);
        }
    }

    public void Dispose()
    {
        stopping = true;
        if (Thread.CurrentThread != thread)
        {
            thread.Join(TimeSpan.FromSeconds(1));
        }

        handle.Dispose();
    }
}
