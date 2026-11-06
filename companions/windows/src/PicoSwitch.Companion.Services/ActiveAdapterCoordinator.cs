namespace PicoSwitch.Companion.Services;

/// <summary>
/// Where a switch from one adapter to another currently is.
///
/// <c>Retiring</c> and <c>Activating</c> are separate on purpose. The outgoing
/// adapter must be completely gone before the incoming one becomes authoritative,
/// and a phase that covered both would make "is this event from the adapter we
/// are leaving or the one we are joining?" unanswerable at exactly the moment it
/// matters.
/// </summary>
public enum SwitchPhase
{
    Settled,
    Retiring,
    Activating,
}

public sealed record ActiveAdapterState
{
    /// <summary>
    /// The adapter the user has chosen. Set the instant a switch begins and NOT
    /// reverted if that adapter turns out to be unreachable: the honest report of
    /// a failed switch is "the adapter you selected is not connected", never a
    /// silent return to the previous one.
    /// </summary>
    public AdapterId? ActiveId { get; init; }

    /// <summary>The adapter being torn down, non-null only during <see cref="SwitchPhase.Retiring"/>.</summary>
    public AdapterId? RetiringId { get; init; }

    public long Generation { get; init; }

    public SwitchPhase Phase { get; init; } = SwitchPhase.Settled;

    public bool Connected { get; init; }

    /// <summary>Why the last activation failed, if it did. Cleared when a new switch begins.</summary>
    public string? Failure { get; init; }

    public bool Transitioning => Phase != SwitchPhase.Settled;
}

public sealed record SwitchPlan(
    AdapterId Target,

    // Null when nothing was active, which is the only case with no retirement.
    AdapterId? Previous,
    long Generation);

public abstract record SwitchOutcome
{
    private SwitchOutcome()
    {
    }

    /// <summary>The target was already active and settled; nothing was torn down.</summary>
    public sealed record AlreadyActive : SwitchOutcome
    {
        public static readonly AlreadyActive Instance = new();
    }

    /// <summary>The target is selected and its connection has been started.</summary>
    public sealed record Activating(SwitchPlan Plan) : SwitchOutcome;

    /// <summary>A newer switch replaced this one; this one stopped without activating.</summary>
    public sealed record Superseded(long Generation) : SwitchOutcome;
}

/// <summary>
/// The single authority on which adapter is active, and on whether an event is
/// allowed to say anything about it.
///
/// ## The rule this exists to enforce
///
/// A switch from adapter A to adapter B is ONE generation-owned transition. A is
/// retired completely before B becomes authoritative, so no callback, connection
/// state or snapshot belonging to A can reach B's UI or lifecycle. If B cannot be
/// reached, the app settles into a truthful "B selected, not connected" state; it
/// never falls back to A.
///
/// That last point is deliberate and is the reason
/// <see cref="ActivationFailed"/> keeps <c>ActiveId</c> pointing at the target. A
/// fallback would be a hidden state transition — the user asked for B, and an app
/// that quietly reconnects A while displaying something else is the class of lie
/// this whole subsystem has been built to avoid.
///
/// ## Why a second coordinator
///
/// <see cref="AdapterRelationshipCoordinator"/> owns ONE attempt at ONE
/// relationship: discovery, pairing, connect progression, identity validation,
/// and the generation that makes a stale attempt inert. This owns the layer above
/// it — which adapter that coordinator is currently working on, and the ordered
/// handover between two of them. Folding the two together would put connect
/// progression and adapter selection under one generation counter, so a retry of
/// a connection would look like a change of adapter.
///
/// Ordering is executed by <see cref="AdapterSwitch"/>; this object only decides.
/// </summary>
public sealed class ActiveAdapterCoordinator(AdapterId? initialActive = null)
{
    private readonly Lock gate = new();
    private ActiveAdapterState current = new() { ActiveId = initialActive };

    public ActiveAdapterState State
    {
        get
        {
            lock (gate)
            {
                return current;
            }
        }
    }

    /// <summary>
    /// Begin a switch to <paramref name="target"/>.
    ///
    /// Returns null only when the target is already active, settled AND connected,
    /// so a repeated tap on a healthy session cannot tear it down.
    ///
    /// Connected is part of that test on purpose. A target that is selected but
    /// disconnected — the state a failed activation deliberately leaves behind —
    /// must still be reachable by choosing it again, or the truthful failure state
    /// would also be a dead end. That switch retires nothing, because the target is
    /// already the active adapter, and goes straight to activation.
    ///
    /// The generation increments here and nowhere else. Any switch already in
    /// flight is dead from this moment: its remaining steps will find a generation
    /// that is not theirs and stop without activating.
    /// </summary>
    public SwitchPlan? Begin(AdapterId target)
    {
        lock (gate)
        {
            if (current.ActiveId == target &&
                current.Phase == SwitchPhase.Settled &&
                current.Connected)
            {
                return null;
            }

            var previous = current.ActiveId == target ? null : current.ActiveId;
            var generation = current.Generation + 1;
            current = new ActiveAdapterState
            {
                ActiveId = target,
                RetiringId = previous,
                Generation = generation,

                // With nothing to retire there is nothing to wait for, so the
                // switch starts already past the handover.
                Phase = previous is not null ? SwitchPhase.Retiring : SwitchPhase.Activating,
                Connected = false,
                Failure = null,
            };
            return new SwitchPlan(target, previous, generation);
        }
    }

    /// <summary>
    /// The outgoing adapter is gone. Returns false when a newer switch has taken
    /// over, which is the caller's signal to stop rather than activate.
    /// </summary>
    public bool RetirementComplete(long generation)
    {
        lock (gate)
        {
            if (generation != current.Generation || current.Phase != SwitchPhase.Retiring)
            {
                return false;
            }

            current = current with { Phase = SwitchPhase.Activating, RetiringId = null };
            return true;
        }
    }

    /// <summary>
    /// Report a connection outcome, guarded by identity rather than by generation.
    ///
    /// Identity is the right guard here because the connect path is shared with
    /// ordinary reconnects that never involved a switch, and because it makes the
    /// dangerous case impossible to express: a result for A cannot settle B.
    /// </summary>
    public bool ActivationSucceeded(AdapterId adapterId)
    {
        lock (gate)
        {
            if (adapterId != current.ActiveId)
            {
                return false;
            }

            current = current with
            {
                Phase = SwitchPhase.Settled,
                RetiringId = null,
                Connected = true,
                Failure = null,
            };
            return true;
        }
    }

    /// <summary>Settles at "selected, not connected". <c>ActiveId</c> is intentionally kept.</summary>
    public bool ActivationFailed(AdapterId adapterId, string message)
    {
        lock (gate)
        {
            if (adapterId != current.ActiveId)
            {
                return false;
            }

            current = current with
            {
                Phase = SwitchPhase.Settled,
                RetiringId = null,
                Connected = false,
                Failure = message,
            };
            return true;
        }
    }

    /// <summary>An accepted, settled session ended. Ignored mid-transition, where teardown is expected.</summary>
    public bool MarkDisconnected()
    {
        lock (gate)
        {
            if (current.Phase != SwitchPhase.Settled || !current.Connected)
            {
                return false;
            }

            current = current with { Connected = false };
            return true;
        }
    }

    /// <summary>
    /// The active adapter was removed from the app, or the last one was.
    ///
    /// The generation still advances: anything in flight for the adapter that no
    /// longer exists must not be able to complete.
    /// </summary>
    public void Cleared()
    {
        lock (gate)
        {
            current = new ActiveAdapterState { Generation = current.Generation + 1 };
        }
    }

    /// <summary>Adopt a selection made outside a switch, such as the registry's load or a verified first pair.</summary>
    public void Adopt(AdapterId? adapterId, bool connected = false)
    {
        lock (gate)
        {
            if (current.Transitioning)
            {
                return;
            }

            current = current with
            {
                ActiveId = adapterId,
                Connected = connected && adapterId is not null,
                Failure = null,
            };
        }
    }

    /// <summary>
    /// May an event carrying <paramref name="address"/> say anything about the
    /// active adapter?
    ///
    /// Three answers, in order:
    ///
    /// 1. **During a retirement, no.** Everything arriving then belongs to the
    ///    adapter being torn down, and the transition owns what the user sees.
    /// 2. **An event with no address is accepted outside a retirement.** The
    ///    discovery and idle-reset states carry no device, and they are genuine
    ///    progress for the adapter being activated. This is safe only because
    ///    retirement is awaited: by the time the incoming adapter is being
    ///    activated, the outgoing one has already emitted its last state.
    /// 3. **Otherwise it must be the active adapter's own address.**
    /// </summary>
    public bool Accepts(string? address)
    {
        lock (gate)
        {
            if (current.Phase == SwitchPhase.Retiring)
            {
                return false;
            }

            return address is null || AdapterId.FromAddress(address) == current.ActiveId;
        }
    }
}
