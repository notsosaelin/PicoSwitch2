using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The one serialized owner of Windows Controller Link input state.
///
/// WinUI touch contacts and Windows.Gaming.Input readings arrive on different
/// threads. <see cref="ControllerInputState"/> deliberately expects one caller,
/// so this boundary serializes both without moving platform APIs into Bridge.Core.
/// The report scheduler takes immutable snapshots through the same lock.
/// </summary>
public sealed class ControllerInputSession
{
    private readonly object gate = new();
    private readonly ControllerInputState input = new();
    private readonly TouchGamepad touch;
    private ControllerSourceIdentity? physicalSource;
    private IControllerInputSampler? sampler;

    public ControllerInputSession() => touch = new TouchGamepad(input);

    public ControllerState Snapshot
    {
        get
        {
            lock (gate)
            {
                return input.State.Value;
            }
        }
    }

    /// <summary>
    /// Which printed legend the selected controller carries.
    ///
    /// Auto resolves from the device identity: a Nintendo-labelled handheld
    /// reports its printed letters and passes through, everything else reports
    /// POSITIONALLY and is swapped so the bottom button stays the bottom button.
    /// That is the right default and the wrong answer for anyone who reads the
    /// on-screen prompt instead of the position — pressing the button marked A on
    /// an Xbox pad then produces B — so the choice is the user's to make.
    /// </summary>
    public ControllerFaceLayout FaceLayout
    {
        get { lock (gate) { return input.RequestedLayout; } }
    }

    public string FaceLayoutReason
    {
        get { lock (gate) { return input.ResolvedLayout.Reason; } }
    }

    public ControllerFaceLayout ResolvedFaceLayout
    {
        get { lock (gate) { return input.ResolvedLayout.Layout; } }
    }

    public void SetFaceLayout(ControllerFaceLayout layout)
    {
        lock (gate)
        {
            input.SetRequestedLayout(layout);
        }
    }

    /// <summary>
    /// Press and release a console button, as if the player had tapped it.
    ///
    /// Injected as a VIRTUAL button, which merges with whatever the live source
    /// is doing rather than replacing it: pressing Home from the app must not
    /// disturb a stick the player is holding, and must not require the app to
    /// own input authority.
    /// </summary>
    /// <remarks>
    /// The hold is real time, not one frame. A console samples this over its own
    /// polling interval and treats a press that appears and vanishes between two
    /// polls as no press at all — the classic "the button does nothing sometimes"
    /// bug. Long enough to be seen, short enough not to read as a hold (Home is
    /// held to open the power menu).
    ///
    /// Released in a finally so a cancelled or faulted wait can never leave a
    /// console button stuck down.
    /// </remarks>
    public async Task TapAsync(ControllerButton button, CancellationToken cancellationToken = default)
    {
        lock (gate)
        {
            input.SetVirtualButton(button, true);
        }

        try
        {
            await Task.Delay(ConsoleButtonHold, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            lock (gate)
            {
                input.SetVirtualButton(button, false);
            }
        }
    }

    /// <summary>
    /// How long an app-injected console button is held.
    ///
    /// 120 ms comfortably exceeds one poll of any console this targets while
    /// staying well under the ~1 s that turns Home into a power-menu hold.
    /// </summary>
    private static readonly TimeSpan ConsoleButtonHold = TimeSpan.FromMilliseconds(120);

    /// <summary>Attach a source that can be read on demand. Null detaches.</summary>
    public void AttachSampler(IControllerInputSampler? source)
    {
        lock (gate)
        {
            sampler = source;
        }
    }

    /// <summary>
    /// Take the freshest possible state: ask the active source to read itself,
    /// then snapshot.
    ///
    /// This is the realtime path's single cadence boundary. Reading a snapshot
    /// that some OTHER timer refreshes adds that timer's whole period as sample
    /// age before the state is even encoded — see
    /// <see cref="IControllerInputSampler"/>.
    ///
    /// An event-driven source needs no sampler and is unaffected: the Touch
    /// Gamepad has already written its current state by the time this runs.
    /// </summary>
    public ControllerState SampleAndSnapshot()
    {
        IControllerInputSampler? source;
        lock (gate)
        {
            source = sampler;
        }

        // Deliberately OUTSIDE the lock: Sample() publishes through
        // ApplyPhysicalFrame, which takes this same lock. Holding it across the
        // call would deadlock the one path that must never stall.
        source?.Sample();

        return Snapshot;
    }

    public InputAuthority Authority
    {
        get
        {
            lock (gate)
            {
                return input.Authority;
            }
        }
    }

    public void ApplyPhysicalFrame(
        ControllerSourceIdentity source,
        ControllerButtonSet buttons,
        AnalogFrame analog)
    {
        lock (gate)
        {
            physicalSource = source;
            if (touch.Active)
            {
                return;
            }

            if (input.Source != source)
            {
                input.SetSource(source);
            }

            input.ApplyPhysicalFrame(buttons, analog);
        }
    }

    public void RemovePhysicalSource(ControllerSourceIdentity source)
    {
        lock (gate)
        {
            if (physicalSource != source)
            {
                return;
            }

            physicalSource = null;
            if (!touch.Active)
            {
                input.SetSource(null);
            }
        }
    }

    public void SetTouchLayout(ResolvedTouchLayout layout)
    {
        lock (gate)
        {
            touch.SetLayout(layout);
        }
    }

    public void ActivateTouch()
    {
        lock (gate)
        {
            touch.Activate();
        }
    }

    public void DeactivateTouch()
    {
        lock (gate)
        {
            touch.Deactivate();
            input.SetSource(physicalSource);
        }
    }

    public void DispatchTouchContacts(IReadOnlyList<TouchContact> contacts)
    {
        lock (gate)
        {
            if (touch.Active)
            {
                touch.Contacts.Dispatch(contacts);
            }
        }
    }

    public void TickTouch(long nowNanos)
    {
        lock (gate)
        {
            if (touch.Active)
            {
                touch.Tick(nowNanos);
            }
        }
    }

    public void ReleaseTouch(TouchReleaseReason reason)
    {
        lock (gate)
        {
            touch.Release(reason);
        }
    }

    public void Neutralize()
    {
        lock (gate)
        {
            touch.Release(TouchReleaseReason.LinkEnded);
            input.Neutralize();
        }
    }

    public TouchDiagnosticsSnapshot TouchDiagnostics()
    {
        lock (gate)
        {
            return touch.Diagnostics();
        }
    }
}
