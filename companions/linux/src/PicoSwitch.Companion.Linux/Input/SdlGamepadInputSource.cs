using System.Collections.Concurrent;
using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Input;
using SDL;

namespace PicoSwitch.Companion.Linux.Input;

/// <summary>
/// SDL3 gamepads for Controller Link: the Linux counterpart of the Windows
/// <c>WindowsGamepadInputSource</c>, with motion.
///
/// ## Why SDL
///
/// SDL's gamepad layer is what Steam and most Linux games use. It supplies the
/// community mapping database (so a pad Linux merely sees as a joystick still has
/// named buttons), the HIDAPI drivers that expose gyro and accelerometer for
/// DualSense, DualShock 4, Switch Pro and the Steam Deck's own controls, rumble,
/// and battery level, behind one API.
///
/// ## Threading
///
/// Every SDL call happens on ONE dedicated thread that owns initialization, the
/// event pump and the open gamepad. Other threads post commands (refresh,
/// select, rumble, sensor enable) and read immutable snapshots. That keeps the
/// realtime Controller Link publisher from ever blocking on SDL, and keeps SDL
/// from being entered concurrently.
///
/// Input is event-driven: the SDL thread publishes a frame the moment a batch
/// of axis or button events changes the selected pad's state, so there is no
/// separate sampling clock to add latency. (Windows polls because its API has
/// no change events; that is why this type implements no input sampler.)
///
/// ## Selection
///
/// The shared <see cref="ControllerSourceSelection"/> rules, unchanged: an
/// explicit choice wins, a single built-in controller (the Steam Deck's own
/// controls) is chosen automatically, and the adapter's own USB output is never
/// auto-selected. Steam Input's virtual pads are listed as excluded, because
/// they duplicate a physical controller that is listed in its own right.
///
/// ## Steam Deck-format controllers SDL does not drive
///
/// A Valve-vendor hidraw node streaming Steam Deck reports under a product id
/// SDL's Deck driver refuses (InputPlumber's emulated Deck on the ROG Xbox Ally,
/// 28de:12fd) is read directly (<see cref="SteamDeckReport"/>, rules in
/// <see cref="SteamDeckSources"/>): buttons, both back-button pairs, sticks,
/// triggers and motion. Steam's virtual pad for the same controls is excluded
/// as a mirror and kept open only for rumble and battery. Without this, Game
/// Mode offered only Steam's virtual Xbox 360 pad, which has no back buttons.
///
/// Status: the SDL path is hardware-validated for buttons and sticks in Game
/// Mode (ROG Xbox Ally, Steam's virtual pad, 2026-11-06). The deck-report path
/// is implementation complete; **hardware validation pending**.
/// </summary>
public sealed class SdlGamepadInputSource : IControllerOutputBackend, IMotionBackend, IAsyncDisposable
{
    private const int EventWaitMillis = 4;
    private const long BatteryPollMillis = 30_000;
    private const uint RumbleDurationMillis = 2_000;
    private const long RumbleRefreshMillis = 1_000;
    private const int DeckProbeMillis = 150;
    private static readonly double NanosPerStopwatchTick = 1e9 / Stopwatch.Frequency;

    private readonly Action<string, string>? log;
    private readonly Thread thread;
    private readonly ConcurrentQueue<Action> commands = new();
    private readonly CancellationTokenSource lifetime = new();
    private readonly TaskCompletionSource started = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Lock gate = new();

    // Owned by the SDL thread.
    private unsafe SDL_Gamepad* gamepad;
    private SDL_JoystickID gamepadId;
    private Dictionary<SDL_JoystickID, ControllerSource> byInstance = [];
    private bool inputDirty;
    private bool publishedForCurrent;
    private AnalogFrame lastAnalog = AnalogFrame.Neutral;
    private ControllerButtonSet lastButtons = ControllerButtonSet.Empty;
    private long lastBatteryPoll;
    private ControllerBattery lastBattery = ControllerBattery.Unknown;
    private RumbleRequest appliedRumble = RumbleRequest.None;
    private long rumbleAppliedAt;
    private bool sensorsOn;

    // The selected source's Candidate.Id: an SDL instance id (> 0), a hidraw
    // deck node (< 0, see DeckKey), or 0 for none.
    private int selectedKey;

    // True while `gamepad` is a deck controller's SDL mirror, open only for
    // rumble and battery; its input is ignored.
    private bool outputOnly;
    private SteamDeckReader? deckReader;
    private readonly Dictionary<HidrawNode, bool> deckProbes = [];

    // Shared, under `gate`.
    private IReadOnlyList<ControllerSource> sources = [];
    private ControllerSource? selectedSource;
    private ControllerSourceIdentity? selectedIdentity;
    private string? rememberedId;
    private string? adapterPersonality;
    private string? unresolvedReason;
    private string? initError;
    private RumbleRequest wantedRumble = RumbleRequest.None;
    private bool motionWanted;
    private bool motionAvailable;
    private bool haveAccel;
    private bool haveGyro;
    private readonly float[] accel = new float[3];
    private readonly float[] gyro = new float[3];
    private ulong gyroTimestamp;

    private long framesPublished;
    private long motionSamples;
    private bool disposed;

    public SdlGamepadInputSource(Action<string, string>? log = null)
    {
        this.log = log;
        thread = new Thread(Run) { IsBackground = true, Name = "sdl-gamepad" };
        thread.Start();
    }

    /// <summary>Every gamepad SDL currently offers, usable or not.</summary>
    public IReadOnlyList<ControllerSource> Sources
    {
        get { lock (gate) { return sources; } }
    }

    public ControllerSource? SelectedSource
    {
        get { lock (gate) { return selectedSource; } }
    }

    public ControllerSourceIdentity? Selected
    {
        get { lock (gate) { return selectedIdentity; } }
    }

    /// <summary>Why nothing usable is selected, or null when a readable source is held.</summary>
    public string? UnreadableReason
    {
        get
        {
            lock (gate)
            {
                return initError ?? selectedSource?.UnreadableReason ?? unresolvedReason;
            }
        }
    }

    public long FramesPublished => Interlocked.Read(ref framesPublished);

    public long MotionSamples => Interlocked.Read(ref motionSamples);

    public event Action? SourcesChanged;

    public event Action<ControllerSourceIdentity, ControllerButtonSet, AnalogFrame>? Frame;

    public event Action<ControllerSourceIdentity>? Removed;

    public event Action<ControllerBattery>? BatteryChanged;

    /* -------------------------------------------------------- selection API */

    /// <param name="personality">
    /// The connected adapter's personality wire name, so its own USB output is
    /// kept out of automatic selection.
    /// </param>
    public Task RefreshAsync(string? personality = null)
    {
        lock (gate)
        {
            adapterPersonality = personality;
        }

        return PostAsync(Enumerate);
    }

    /// <summary>Remember an explicit choice; null returns to automatic selection.</summary>
    public Task SelectAsync(string? sourceId, string? personality = null)
    {
        lock (gate)
        {
            rememberedId = sourceId;
            adapterPersonality = personality;
        }

        return PostAsync(Enumerate);
    }

    /* --------------------------------------------------------------- output */

    public void Apply(RumbleRequest request)
    {
        lock (gate)
        {
            wantedRumble = request;
        }
    }

    /* --------------------------------------------------------------- motion */

    public bool Available
    {
        get { lock (gate) { return motionAvailable; } }
    }

    public void Start()
    {
        lock (gate)
        {
            motionWanted = true;
        }
    }

    public void Stop()
    {
        lock (gate)
        {
            motionWanted = false;
            haveAccel = false;
            haveGyro = false;
        }
    }

    public ControllerMotion Sample()
    {
        lock (gate)
        {
            // Never a half-populated first frame: both sensors must have reported.
            return motionWanted && haveAccel && haveGyro
                ? SdlGamepadMapping.Motion(accel, gyro, gyroTimestamp)
                : ControllerMotion.None;
        }
    }

    public async ValueTask DisposeAsync()
    {
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
        }

        lifetime.Cancel();
        await Task.Run(() => thread.Join(TimeSpan.FromSeconds(2))).ConfigureAwait(false);
        lifetime.Dispose();
    }

    /* ----------------------------------------------------------- SDL thread */

    private Task PostAsync(Action command)
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        commands.Enqueue(() =>
        {
            try
            {
                command();
                done.TrySetResult();
            }
            catch (Exception error)
            {
                done.TrySetException(error);
            }
        });

        // A source that failed to start still answers, with its reason.
        return started.Task.IsCompleted && initError is not null ? Task.CompletedTask : done.Task;
    }

    private unsafe void Run()
    {
        try
        {
            // No signal handlers: SDL must not take SIGINT/SIGTERM from the .NET
            // runtime. Background events: the window that has focus is Avalonia's,
            // not SDL's, and gamepad input must flow regardless.
            SDL3.SDL_SetHint("SDL_NO_SIGNAL_HANDLERS", "1");
            SDL3.SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            SDL3.SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1");

            if (!SDL3.SDL_Init(SDL_InitFlags.SDL_INIT_GAMEPAD))
            {
                Fail($"SDL could not start its gamepad subsystem: {SDL3.SDL_GetError()}");
                return;
            }
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            Fail($"The SDL3 library could not be loaded: {error.Message}");
            return;
        }

        started.TrySetResult();
        Log("init", "SDL gamepad subsystem started");
        try
        {
            Enumerate();
            SDL_Event e;
            while (!lifetime.IsCancellationRequested)
            {
                while (commands.TryDequeue(out var command))
                {
                    command();
                }

                if (SDL3.SDL_WaitEventTimeout(&e, EventWaitMillis))
                {
                    do
                    {
                        Handle(&e);
                    }
                    while (SDL3.SDL_PollEvent(&e));
                }

                if (inputDirty)
                {
                    inputDirty = false;
                    PublishFrame();
                }

                ServiceOutput();
                ServiceSensors();
                ServiceBattery();
            }
        }
        catch (Exception error)
        {
            Log("fail", $"SDL thread stopped: {error}");
            lock (gate)
            {
                initError = $"Controller input stopped: {error.Message}";
            }
        }
        finally
        {
            CloseSelected(raiseRemoved: false);
            SDL3.SDL_Quit();
        }
    }

    private unsafe void Handle(SDL_Event* e)
    {
        switch (e->Type)
        {
            case SDL_EventType.SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EventType.SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EventType.SDL_EVENT_GAMEPAD_REMAPPED:
                // Arrival re-resolves through the shared rules rather than taking
                // whatever appeared: "first gamepad wins" is how an adapter's own
                // USB echo becomes selectable.
                Enumerate();
                break;

            case SDL_EventType.SDL_EVENT_GAMEPAD_AXIS_MOTION:
            case SDL_EventType.SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EventType.SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (e->gdevice.which == gamepadId)
                {
                    inputDirty = true;
                }

                break;

            case SDL_EventType.SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                if (e->gsensor.which == gamepadId)
                {
                    OnSensor(&e->gsensor);
                }

                break;

            case SDL_EventType.SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
                if (e->jbattery.which == gamepadId)
                {
                    lastBatteryPoll = 0;
                }

                break;
        }
    }

    private unsafe void OnSensor(SDL_GamepadSensorEvent* sensor)
    {
        var data = &sensor->data.e0;

        // Some drivers report no device timestamp; SDL's own event timestamp is
        // then the closest thing to a sample time (both are nanoseconds).
        var stamp = sensor->sensor_timestamp != 0 ? sensor->sensor_timestamp : sensor->timestamp;
        lock (gate)
        {
            if (!motionWanted)
            {
                return;
            }

            switch ((SDL_SensorType)sensor->sensor)
            {
                case SDL_SensorType.SDL_SENSOR_ACCEL:
                    accel[0] = data[0];
                    accel[1] = data[1];
                    accel[2] = data[2];
                    haveAccel = true;
                    break;

                case SDL_SensorType.SDL_SENSOR_GYRO:
                    gyro[0] = data[0];
                    gyro[1] = data[1];
                    gyro[2] = data[2];
                    gyroTimestamp = stamp;
                    haveGyro = true;
                    Interlocked.Increment(ref motionSamples);
                    break;
            }
        }
    }

    private unsafe void Enumerate()
    {
        var found = new List<ControllerSource>();
        var sdlPads = new Dictionary<SDL_JoystickID, ControllerSource>();
        string? personality;
        string? remembered;
        lock (gate)
        {
            personality = adapterPersonality;
            remembered = rememberedId;
        }

        // Deck-format hidraw controllers first: an SDL pad carrying the same
        // Valve identity is a mirror of one of them (see SteamDeckSources).
        var deckNodes = DiscoverDeckNodes();
        var deckSources = new Dictionary<int, (ControllerSource Source, HidrawNode Node)>();
        foreach (var node in deckNodes)
        {
            var source = new ControllerSource(
                new ControllerCandidate(
                    Id: DeckKey(node),
                    Descriptor: SteamDeckSources.Descriptor(node),
                    Name: node.Name,
                    VendorId: node.Vendor,
                    ProductId: node.Product,
                    HasMotionAxes: true,
                    HasGamepadButtons: true),
                ControllerAttachment.Unknown,
                IsGamepadClass: true,
                MayBeThisAdapter: AdapterEcho.MayBeThisAdapter(node.Vendor, node.Product, personality));
            found.Add(source);
            deckSources[DeckKey(node)] = (source, node);
        }

        var mirrors = new Dictionary<int, SDL_JoystickID>();
        using (var ids = SDL3.SDL_GetGamepads())
        {
            if (ids is not null)
            {
                var seen = new Dictionary<string, int>(StringComparer.Ordinal);
                for (var i = 0; i < ids.Count; i++)
                {
                    var id = ids[i];
                    var vendor = SDL3.SDL_GetGamepadVendorForID(id);
                    var product = SDL3.SDL_GetGamepadProductForID(id);
                    var name = SDL3.SDL_GetGamepadNameForID(id) ?? "Controller";
                    var descriptor = SdlGamepadMapping.Descriptor(vendor, product, serial: null, name);

                    // Two identical pads share every SDL-visible identity; keep
                    // their descriptors distinct so the list can still tell them
                    // apart within one session.
                    var count = seen.TryGetValue(descriptor, out var n) ? n + 1 : 1;
                    seen[descriptor] = count;
                    if (count > 1)
                    {
                        descriptor += $"#{count}";
                    }

                    var mirrorOf = SteamDeckSources.MirrorOf(vendor, product, deckNodes);
                    if (mirrorOf is not null)
                    {
                        mirrors.TryAdd(DeckKey(mirrorOf), id);
                    }

                    var source = new ControllerSource(
                        new ControllerCandidate(
                            Id: (int)id,
                            Descriptor: descriptor,
                            Name: name,
                            VendorId: vendor,
                            ProductId: product,
                            HasMotionAxes: true,
                            HasGamepadButtons: true,
                            IsVirtual: SdlGamepadMapping.IsSteamVirtualPad(vendor, product) || mirrorOf is not null),
                        SdlGamepadMapping.Attachment(vendor, product),
                        IsGamepadClass: true,
                        MayBeThisAdapter: AdapterEcho.MayBeThisAdapter(vendor, product, personality),
                        Connection: id == gamepadId && gamepad is not null && !outputOnly
                            ? SdlGamepadMapping.Connection(SDL3.SDL_GetGamepadConnectionState(gamepad))
                            : ControllerConnection.Unknown);
                    found.Add(source);
                    sdlPads[id] = source;
                }
            }
        }

        var resolved = ControllerSourceSelection.Resolve(found, remembered);
        var reason = ControllerSourceSelection.UnresolvedReason(found, resolved);
        var resolvedKey = resolved?.Candidate.Id ?? 0;

        if (resolvedKey < 0 && deckSources.TryGetValue(resolvedKey, out var deck))
        {
            if (selectedKey != resolvedKey || deckReader is null || !deckReader.Alive)
            {
                CloseSelected(raiseRemoved: true);
                OpenDeck(deck.Node, deck.Source);
            }

            // The SDL mirror carries rumble and battery for the deck controller;
            // it may appear or vanish independently (Steam starts its virtual pad
            // after the app opens), so it is reconciled on every enumeration.
            SetOutputMirror(mirrors.TryGetValue(resolvedKey, out var mirror) ? mirror : (SDL_JoystickID)0);
        }
        else
        {
            var resolvedId = resolved is null ? (SDL_JoystickID)0 : (SDL_JoystickID)(uint)resolvedKey;
            if (selectedKey != resolvedKey || deckReader is not null || outputOnly || gamepad is null)
            {
                CloseSelected(raiseRemoved: true);
                if (resolved is not null)
                {
                    Open(resolvedId, resolved);
                }
            }
        }

        lock (gate)
        {
            sources = found;
            selectedSource = resolved is null ? null : CurrentSourceFor(resolved);
            unresolvedReason = reason;
        }

        SourcesChanged?.Invoke();
    }

    private static int DeckKey(HidrawNode node) => -(node.Index + 1);

    /// <summary>
    /// Valve hidraw nodes that SDL leaves undriven and that stream Steam Deck
    /// controller-state reports. Each node is opened and probed once per
    /// identity; the answer is cached, so re-enumeration costs only sysfs reads.
    /// </summary>
    private List<HidrawNode> DiscoverDeckNodes()
    {
        var result = new List<HidrawNode>();
        IReadOnlyList<HidrawNode> nodes;
        try
        {
            nodes = Hidraw.Enumerate();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return result;
        }

        foreach (var node in nodes)
        {
            if (!SteamDeckSources.IsCandidate(node.Vendor, node.Product))
            {
                continue;
            }

            if (deckReader is { Alive: true } reader && reader.Node == node)
            {
                result.Add(node);
                continue;
            }

            if (!deckProbes.TryGetValue(node, out var isDeck))
            {
                using var handle = Hidraw.Open(node.DevicePath, out var error);
                isDeck = handle is not null && handle.Probe(SteamDeckReport.IsState, DeckProbeMillis);
                deckProbes[node] = isDeck;
                Log("hidraw", handle is null
                    ? $"{node.DevicePath} {node.Vendor:x4}:{node.Product:x4} '{node.Name}' not readable: {error}"
                    : $"{node.DevicePath} {node.Vendor:x4}:{node.Product:x4} '{node.Name}' steam-deck-format={isDeck}");
            }

            if (isDeck)
            {
                result.Add(node);
            }
        }

        return result;
    }

    private unsafe ControllerSource CurrentSourceFor(ControllerSource source) =>
        gamepad is null || outputOnly
            ? source
            : source with { Connection = SdlGamepadMapping.Connection(SDL3.SDL_GetGamepadConnectionState(gamepad)) };

    private unsafe void Open(SDL_JoystickID id, ControllerSource source)
    {
        var opened = SDL3.SDL_OpenGamepad(id);
        if (opened is null)
        {
            Log("open", $"could not open {source.Name}: {SDL3.SDL_GetError()}");
            return;
        }

        gamepad = opened;
        gamepadId = id;
        outputOnly = false;
        selectedKey = source.Candidate.Id;
        sensorsOn = false;
        lastBattery = ControllerBattery.Unknown;
        lastBatteryPoll = 0;
        var motion = SDL3.SDL_GamepadHasSensor(opened, SDL_SensorType.SDL_SENSOR_ACCEL) &&
                     SDL3.SDL_GamepadHasSensor(opened, SDL_SensorType.SDL_SENSOR_GYRO);
        lock (gate)
        {
            selectedIdentity = new ControllerSourceIdentity(
                source.Id, source.Name, source.Candidate.VendorId, source.Candidate.ProductId);
            motionAvailable = motion;
            haveAccel = false;
            haveGyro = false;
        }

        publishedForCurrent = false;
        Log("open",
            $"{source.Name} vid={source.Candidate.VendorId:x4} pid={source.Candidate.ProductId:x4} " +
            $"type={SDL3.SDL_GetGamepadType(opened)} motion={motion}");
        inputDirty = true;
    }

    private void OpenDeck(HidrawNode node, ControllerSource source)
    {
        var identity = new ControllerSourceIdentity(
            source.Id, source.Name, source.Candidate.VendorId, source.Candidate.ProductId);
        var reader = SteamDeckReader.Start(node, identity, OnDeckState, OnDeckGone, out var error);
        if (reader is null)
        {
            Log("open", $"could not open {node.DevicePath} ({source.Name}): {error}");
            deckProbes.Remove(node);
            return;
        }

        deckReader = reader;
        selectedKey = source.Candidate.Id;
        lock (gate)
        {
            selectedIdentity = identity;
            motionAvailable = true;
            haveAccel = false;
            haveGyro = false;
        }

        Log("open",
            $"{source.Name} vid={node.Vendor:x4} pid={node.Product:x4} via {node.DevicePath} " +
            "(steam-deck report, motion=True)");
    }

    /// <summary>
    /// Open, replace or drop the SDL pad used only for rumble and battery while
    /// a deck controller is the input. Its own input is never published.
    /// </summary>
    private unsafe void SetOutputMirror(SDL_JoystickID id)
    {
        if (outputOnly && gamepad is not null && gamepadId == id)
        {
            return;
        }

        CloseSdl();
        if (id == 0)
        {
            return;
        }

        var opened = SDL3.SDL_OpenGamepad(id);
        if (opened is null)
        {
            Log("open", $"could not open the rumble mirror: {SDL3.SDL_GetError()}");
            return;
        }

        gamepad = opened;
        gamepadId = id;
        outputOnly = true;
        lastBattery = ControllerBattery.Unknown;
        lastBatteryPoll = 0;
        Log("open", $"rumble and battery through {SDL3.SDL_GetGamepadNameForID(id)} (input ignored)");
    }

    /// <summary>Reader thread: one decoded report.</summary>
    private void OnDeckState(SteamDeckReader reader, in SteamDeckReport.State state)
    {
        Span<float> a = stackalloc float[3];
        Span<float> g = stackalloc float[3];
        SteamDeckReport.Sensors(state, a, g);
        var stamp = (ulong)(Stopwatch.GetTimestamp() * NanosPerStopwatchTick);
        lock (gate)
        {
            if (motionWanted && ReferenceEquals(reader, deckReader))
            {
                a.CopyTo(accel);
                g.CopyTo(gyro);
                gyroTimestamp = stamp;
                haveAccel = true;
                haveGyro = true;
                Interlocked.Increment(ref motionSamples);
            }
        }

        var buttons = SteamDeckReport.Buttons(state);
        var analog = SteamDeckReport.Analog(state);
        if (reader.Published && buttons.Equals(reader.LastButtons) && analog == reader.LastAnalog)
        {
            return;
        }

        reader.Published = true;
        reader.LastButtons = buttons;
        reader.LastAnalog = analog;
        Frame?.Invoke(reader.Identity, buttons, analog);
        Interlocked.Increment(ref framesPublished);
    }

    /// <summary>Reader thread: the node went away. Re-resolve on the SDL thread.</summary>
    private void OnDeckGone(SteamDeckReader reader)
    {
        Log("hidraw", $"{reader.Node.DevicePath} closed");
        commands.Enqueue(() =>
        {
            deckProbes.Remove(reader.Node);
            Enumerate();
        });
    }

    private unsafe void CloseSdl()
    {
        if (gamepad is null)
        {
            return;
        }

        SDL3.SDL_RumbleGamepad(gamepad, 0, 0, 0);
        SDL3.SDL_CloseGamepad(gamepad);
        gamepad = null;
        gamepadId = 0;
        outputOnly = false;
        sensorsOn = false;
        appliedRumble = RumbleRequest.None;
    }

    private void CloseSelected(bool raiseRemoved)
    {
        // The reader is joined before Removed is raised, so no frame can follow
        // the removal of the controller it is attributed to.
        deckReader?.Dispose();
        deckReader = null;
        CloseSdl();
        selectedKey = 0;

        ControllerSourceIdentity? removed;
        lock (gate)
        {
            removed = selectedIdentity;
            selectedIdentity = null;
            motionAvailable = false;
            haveAccel = false;
            haveGyro = false;
        }

        // Tell downstream before anything re-resolves, so no frame is published
        // attributed to a controller that has already gone.
        if (raiseRemoved && removed is not null)
        {
            Removed?.Invoke(removed);
        }
    }

    private unsafe void PublishFrame()
    {
        var pad = gamepad;
        ControllerSourceIdentity? identity;
        lock (gate)
        {
            identity = selectedIdentity;
        }

        if (pad is null || identity is null || outputOnly)
        {
            return;
        }

        bool Pressed(SDL_GamepadButton button) => SDL3.SDL_GetGamepadButton(pad, button);
        short Axis(SDL_GamepadAxis axis) => SDL3.SDL_GetGamepadAxis(pad, axis);

        var buttons = SdlGamepadMapping.Buttons(Pressed);
        var analog = SdlGamepadMapping.Analog(Axis, Pressed);
        // The first frame of a newly opened pad always goes out: it carries a
        // new identity even when its state happens to equal the old pad's.
        if (publishedForCurrent && buttons.Equals(lastButtons) && analog == lastAnalog)
        {
            return;
        }

        publishedForCurrent = true;
        lastButtons = buttons;
        lastAnalog = analog;
        Frame?.Invoke(identity, buttons, analog);
        Interlocked.Increment(ref framesPublished);
    }

    private unsafe void ServiceOutput()
    {
        if (gamepad is null)
        {
            return;
        }

        RumbleRequest wanted;
        lock (gate)
        {
            wanted = wantedRumble;
        }

        var now = Environment.TickCount64;

        // SDL rumble is a timed effect; a held request is refreshed well inside
        // its duration so a long rumble does not lapse between feedback frames,
        // and a lost app cannot leave a motor running for more than two seconds.
        var stale = !wanted.Silent && now - rumbleAppliedAt >= RumbleRefreshMillis;
        if (wanted == appliedRumble && !stale)
        {
            return;
        }

        SDL3.SDL_RumbleGamepad(
            gamepad,
            SdlGamepadMapping.Motor(wanted.Left),
            SdlGamepadMapping.Motor(wanted.Right),
            wanted.Silent ? 0 : RumbleDurationMillis);
        appliedRumble = wanted;
        rumbleAppliedAt = now;
    }

    private unsafe void ServiceSensors()
    {
        if (gamepad is null || outputOnly)
        {
            return;
        }

        bool wanted;
        bool available;
        lock (gate)
        {
            wanted = motionWanted;
            available = motionAvailable;
        }

        var on = wanted && available;
        if (on == sensorsOn)
        {
            return;
        }

        // Only while the console consumes motion: an IMU streaming for nobody
        // costs Bluetooth bandwidth and battery on a wireless pad.
        SDL3.SDL_SetGamepadSensorEnabled(gamepad, SDL_SensorType.SDL_SENSOR_ACCEL, on);
        SDL3.SDL_SetGamepadSensorEnabled(gamepad, SDL_SensorType.SDL_SENSOR_GYRO, on);
        sensorsOn = on;
        Log("motion", on
            ? $"sensors on, gyro {SDL3.SDL_GetGamepadSensorDataRate(gamepad, SDL_SensorType.SDL_SENSOR_GYRO):F0} Hz"
            : "sensors off");
    }

    private unsafe void ServiceBattery()
    {
        if (gamepad is null)
        {
            return;
        }

        var now = Environment.TickCount64;
        if (lastBatteryPoll != 0 && now - lastBatteryPoll < BatteryPollMillis)
        {
            return;
        }

        lastBatteryPoll = now;
        int percent;
        var state = SDL3.SDL_GetGamepadPowerInfo(gamepad, &percent);
        var reading = SdlGamepadMapping.Battery(state, percent);
        if (reading == lastBattery)
        {
            return;
        }

        lastBattery = reading;
        BatteryChanged?.Invoke(reading);
    }

    private void Fail(string reason)
    {
        Log("fail", reason);
        lock (gate)
        {
            initError = reason;
        }

        started.TrySetResult();
        while (commands.TryDequeue(out var command))
        {
            command();
        }

        SourcesChanged?.Invoke();
    }

    private void Log(string stage, string detail) => log?.Invoke("input", $"{stage} {detail}");
}
