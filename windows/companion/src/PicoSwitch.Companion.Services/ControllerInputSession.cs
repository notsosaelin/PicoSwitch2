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
