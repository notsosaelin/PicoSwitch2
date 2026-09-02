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
