using PicoSwitch.Companion.Windows.Bluetooth;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// One adapter, as the connection lifecycle sees it.
///
/// Deliberately NOT the registry record: the coordinator owns one attempt at a
/// time and has no business knowing about aliases, cached firmware, or the other
/// adapters the user owns. <see cref="AdapterRelationshipExtensions.ToRelationship"/>
/// is the one bridge between the two.
/// </summary>
public sealed record AdapterRelationship(
    string Address,
    string DisplayName = AdapterRecord.DefaultProductName);

public static class AdapterRelationshipExtensions
{
    public static AdapterRelationship ToRelationship(this AdapterRecord record) =>
        new(record.Address, record.DisplayName);
}

/// <summary>
/// The lifecycle of the SELECTED registry row.
///
/// Ported from the Android <c>AdapterRelationshipPhase</c> with two deliberate
/// differences (WINDOWS_PASS.md §18.1):
///
/// - <c>Associating</c> is gone. Windows has no Companion Device association and
///   needs none — <c>DeviceInformation.Id</c> plus the pairing state already give
///   a stable handle and a trust state (§19.2). <c>Discovering</c> takes its place
///   and means what it says: watching advertisements for the management service.
/// - <c>Validating</c> is explicit. <c>Connected</c> may only be entered from it,
///   so a link that is up but has not answered <c>info</c> with
///   <c>id == "picoswitch"</c> is never reported as a working adapter.
///
/// Other remembered rows remain inert while this lifecycle runs.
/// </summary>
public enum AdapterRelationshipPhase
{
    NoRelationship,
    Idle,
    Discovering,
    Pairing,
    Connecting,
    Validating,
    Connected,
    Failed,
    RepairRequired,
}

public enum AdapterConnectReason
{
    FirstPair,
    ForegroundAuto,
    Manual,
    AfterPair,
    AfterPersonality,

    /// <summary>The activation half of a switch between two known adapters.</summary>
    AdapterSwitch,
}

public static class AdapterConnectReasons
{
    public static string DiagnosticName(this AdapterConnectReason value) => value switch
    {
        AdapterConnectReason.FirstPair => "first-pair",
        AdapterConnectReason.ForegroundAuto => "foreground-auto",
        AdapterConnectReason.Manual => "manual",
        AdapterConnectReason.AfterPair => "after-pair",
        AdapterConnectReason.AfterPersonality => "after-personality",
        _ => "adapter-switch",
    };
}

public sealed record AdapterRelationshipStatus
{
    public AdapterRelationshipPhase Phase { get; init; } = AdapterRelationshipPhase.NoRelationship;

    public long Generation { get; init; }

    public AdapterConnectReason? Reason { get; init; }

    public WindowsPairingState Pairing { get; init; } = WindowsPairingState.Unknown;

    public string? Message { get; init; }

    /// <summary>
    /// An attempt is in flight, so a reconnect request must be inert.
    ///
    /// <c>Validating</c> counts: the link is up but the adapter has not yet proved
    /// it is a PicoSwitch2, and a second attempt started underneath would race the
    /// identity check.
    /// </summary>
    public bool AttemptActive => Phase is
        AdapterRelationshipPhase.Discovering or
        AdapterRelationshipPhase.Pairing or
        AdapterRelationshipPhase.Connecting or
        AdapterRelationshipPhase.Validating;
}

public sealed record AdapterConnectionAttempt(
    long Generation,
    AdapterConnectReason Reason,
    AdapterRelationship Relationship);

public abstract record AdapterLifecycleDecision
{
    private AdapterLifecycleDecision()
    {
    }

    public sealed record Ignored : AdapterLifecycleDecision
    {
        public static readonly Ignored Instance = new();
    }

    /// <summary>
    /// Wait for the Windows pairing ceremony. <c>StartPairing</c> is false when
    /// Windows already reports the device as paired-in-progress and a second
    /// <c>PairAsync</c> would only race it.
    /// </summary>
    public sealed record AwaitPairing(AdapterConnectionAttempt Attempt, bool StartPairing)
        : AdapterLifecycleDecision;

    public sealed record Connect(AdapterConnectionAttempt Attempt) : AdapterLifecycleDecision;

    public sealed record RelationshipMetadataUpdated(AdapterRelationship Relationship)
        : AdapterLifecycleDecision;

    public sealed record RepairRequired(string Message) : AdapterLifecycleDecision;
}

/// <summary>
/// Single owner for discovery, pairing, connect and identity-validation
/// progression for one adapter.
///
/// Windows delivers device-watcher and pairing callbacks on pool threads and can
/// deliver the same transition more than once. Every entry point is reduced here
/// so only the current generation can advance and duplicate callbacks are inert.
///
/// Rules, all load-bearing:
///
/// - **Every transition carries a generation.** A decision for a stale generation
///   returns <c>Ignored</c>. <see cref="RequestReconnect"/> is inert while an
///   attempt is active or while <c>Connected</c>.
/// - **<c>Failed</c> is retryable; <c>RepairRequired</c> is not.** A bond mismatch
///   goes straight to <c>RepairRequired</c>, because retrying cannot succeed: the
///   adapter has no key to authenticate with.
/// - **<c>Connected</c> may only be entered from <c>Validating</c>.**
/// - **<c>Idle</c> retains the selected registry row; <c>NoRelationship</c> does
///   not.** Losing a link never deletes relationship truth; only an explicit
///   per-adapter Remove does. Controller Forget is a different firmware operation
///   and cannot remove the management relationship.
/// - **Cancellation retains the relationship.**
/// </summary>
public sealed class AdapterRelationshipCoordinator(AdapterRelationship? initialRelationship)
{
    public const string PairingFailedMessage =
        "Couldn't pair with the adapter. Make sure its pairing mode is active, then try again.";

    public const string PairingLostMessage =
        "Windows no longer has a Bluetooth pairing for this saved adapter.";

    public const string IdentityRejectedMessage =
        "That device answered, but it is not a PicoSwitch2 adapter.";

    private readonly Lock gate = new();

    private long generation;
    private AdapterRelationship? savedRelationship = initialRelationship;
    private AdapterRelationship? candidate;
    private AdapterConnectionAttempt? activeAttempt;

    private AdapterRelationshipStatus status = new()
    {
        Phase = initialRelationship is null
            ? AdapterRelationshipPhase.NoRelationship
            : AdapterRelationshipPhase.Idle,
    };

    public AdapterRelationshipStatus Status
    {
        get
        {
            lock (gate)
            {
                return status;
            }
        }
    }

    /// <summary>
    /// Start looking for an adapter to pair with.
    ///
    /// The message names the physical requirement, because a new
    /// management-client bond is admitted only while the adapter's double-tap
    /// pairing window is open. An app that omits that turns a firmware admission
    /// rule into a mysterious timeout.
    /// </summary>
    public long BeginDiscovery()
    {
        lock (gate)
        {
            generation += 1;
            candidate = null;
            activeAttempt = null;
            status = new AdapterRelationshipStatus
            {
                Phase = AdapterRelationshipPhase.Discovering,
                Generation = generation,
                Pairing = status.Pairing,
                Message = "Looking for PicoSwitch2. Double-tap the adapter's pairing button.",
            };
            return generation;
        }
    }

    /// <summary>
    /// Feed the exact device obtained from the management-service advertisement
    /// scan into pairing progression.
    ///
    /// UUID matching is the discovery authority; a friendly name is not
    /// authentication.
    /// </summary>
    public AdapterLifecycleDecision DeviceDiscovered(
        long discoveryGeneration,
        AdapterRelationship relationship,
        WindowsPairingState pairing)
    {
        lock (gate)
        {
            if (discoveryGeneration != generation ||
                status.Phase != AdapterRelationshipPhase.Discovering)
            {
                // A watcher can report the same advertisement again while the
                // first result is still being acted on. It may improve the
                // display name, but it must never start a second pairing or
                // connection.
                if (discoveryGeneration == generation &&
                    SameAddress(candidate?.Address, relationship.Address) &&
                    candidate is not null)
                {
                    candidate = Merge(candidate, relationship);
                    if (status.Phase == AdapterRelationshipPhase.Connected)
                    {
                        savedRelationship = candidate;
                        return new AdapterLifecycleDecision.RelationshipMetadataUpdated(candidate);
                    }
                }

                return AdapterLifecycleDecision.Ignored.Instance;
            }

            candidate = relationship;
            var attempt = new AdapterConnectionAttempt(
                generation,
                AdapterConnectReason.FirstPair,
                relationship);
            activeAttempt = attempt;

            switch (pairing)
            {
                case WindowsPairingState.Paired:
                    status = status with
                    {
                        Phase = AdapterRelationshipPhase.Connecting,
                        Reason = attempt.Reason,
                        Pairing = pairing,
                        Message = "Secure pairing complete; connecting management.",
                    };
                    return new AdapterLifecycleDecision.Connect(attempt);

                case WindowsPairingState.NotPaired:
                    status = status with
                    {
                        Phase = AdapterRelationshipPhase.Pairing,
                        Reason = attempt.Reason,
                        Pairing = pairing,
                        Message = "Confirm the Windows pairing request to finish setting up the adapter.",
                    };
                    return new AdapterLifecycleDecision.AwaitPairing(attempt, StartPairing: true);

                default:
                    const string message = "Windows could not report this device's pairing state.";
                    status = status with
                    {
                        Phase = AdapterRelationshipPhase.Failed,
                        Pairing = pairing,
                        Message = message,
                    };
                    activeAttempt = null;
                    return new AdapterLifecycleDecision.RepairRequired(message);
            }
        }
    }

    /// <summary>The Windows pairing ceremony finished, one way or the other.</summary>
    public AdapterLifecycleDecision PairingCompleted(
        long attemptGeneration,
        WindowsPairingState pairing)
    {
        lock (gate)
        {
            if (activeAttempt is not { } attempt ||
                attempt.Generation != attemptGeneration ||
                generation != attemptGeneration ||
                status.Phase != AdapterRelationshipPhase.Pairing)
            {
                return AdapterLifecycleDecision.Ignored.Instance;
            }

            status = status with { Pairing = pairing };
            switch (pairing)
            {
                case WindowsPairingState.Paired:
                    status = status with
                    {
                        Phase = AdapterRelationshipPhase.Connecting,
                        Message = "Secure pairing complete; connecting management.",
                    };
                    return new AdapterLifecycleDecision.Connect(attempt);

                default:
                    status = status with
                    {
                        Phase = AdapterRelationshipPhase.Failed,
                        Message = PairingFailedMessage,
                    };
                    activeAttempt = null;

                    // Deliberately Failed and not RepairRequired: nothing was ever
                    // paired, so there is no stale trust to tear down. Repair would
                    // offer to unpair something that does not exist.
                    return AdapterLifecycleDecision.Ignored.Instance;
            }
        }
    }

    /// <summary>
    /// The link is up: services resolved and the TX CCC written. The adapter has
    /// not yet proved what it is.
    /// </summary>
    public bool CarrierReady(long connectionGeneration)
    {
        lock (gate)
        {
            if (activeAttempt is not { } attempt ||
                attempt.Generation != connectionGeneration ||
                status.Phase != AdapterRelationshipPhase.Connecting)
            {
                return false;
            }

            status = status with
            {
                Phase = AdapterRelationshipPhase.Validating,
                Message = "Checking the adapter's identity.",
            };
            return true;
        }
    }

    /// <summary>
    /// <c>info</c> answered with <c>id == "picoswitch"</c>.
    ///
    /// The ONLY route into <c>Connected</c>. Returns the verified relationship, or
    /// null when the result belongs to a retired attempt.
    /// </summary>
    public AdapterRelationship? IdentityValidated(long connectionGeneration)
    {
        lock (gate)
        {
            if (activeAttempt is not { } attempt ||
                attempt.Generation != connectionGeneration ||
                status.Phase != AdapterRelationshipPhase.Validating)
            {
                return null;
            }

            var verified = candidate ?? attempt.Relationship;
            savedRelationship = verified;
            activeAttempt = null;
            status = status with { Phase = AdapterRelationshipPhase.Connected, Message = null };
            return verified;
        }
    }

    /// <summary>
    /// The device answered but is not a PicoSwitch2.
    ///
    /// Discovering *a* device is not permission to adopt it: the caller must not
    /// create or change a registry row on this path.
    /// </summary>
    public AdapterLifecycleDecision IdentityRejected(long connectionGeneration)
    {
        lock (gate)
        {
            if (activeAttempt is not { } attempt || attempt.Generation != connectionGeneration)
            {
                return AdapterLifecycleDecision.Ignored.Instance;
            }

            activeAttempt = null;
            candidate = null;
            status = status with
            {
                Phase = AdapterRelationshipPhase.Failed,
                Message = IdentityRejectedMessage,
            };
            return AdapterLifecycleDecision.Ignored.Instance;
        }
    }

    public AdapterLifecycleDecision RequestReconnect(
        AdapterRelationship relationship,
        AdapterConnectReason reason,
        WindowsPairingState pairing)
    {
        lock (gate)
        {
            if (status.AttemptActive || status.Phase == AdapterRelationshipPhase.Connected)
            {
                return AdapterLifecycleDecision.Ignored.Instance;
            }

            generation += 1;
            var attempt = new AdapterConnectionAttempt(generation, reason, relationship);
            activeAttempt = attempt;
            candidate = relationship;

            switch (pairing)
            {
                case WindowsPairingState.Paired:
                    status = new AdapterRelationshipStatus
                    {
                        Phase = AdapterRelationshipPhase.Connecting,
                        Generation = generation,
                        Reason = reason,
                        Pairing = pairing,
                        Message = $"Connecting to {relationship.DisplayName}.",
                    };
                    return new AdapterLifecycleDecision.Connect(attempt);

                case WindowsPairingState.NotPaired:
                    status = new AdapterRelationshipStatus
                    {
                        Phase = AdapterRelationshipPhase.RepairRequired,
                        Generation = generation,
                        Reason = reason,
                        Pairing = pairing,
                        Message = PairingLostMessage,
                    };
                    activeAttempt = null;
                    return new AdapterLifecycleDecision.RepairRequired(PairingLostMessage);

                default:
                    // Unknown means the PROBE did not establish an answer -- not
                    // that the pairing is gone. Demanding a repair here would offer
                    // to destroy a working trust relationship on the strength of a
                    // status read that failed, and an adapter that is merely out of
                    // range reads exactly like this.
                    //
                    // Attempt the connection instead. The connect is the authority:
                    // if the pairing really has gone, the attempt fails and
                    // AdapterResetSignature classifies it from evidence.
                    status = new AdapterRelationshipStatus
                    {
                        Phase = AdapterRelationshipPhase.Connecting,
                        Generation = generation,
                        Reason = reason,
                        Pairing = pairing,
                        Message = $"Connecting to {relationship.DisplayName}.",
                    };
                    return new AdapterLifecycleDecision.Connect(attempt);
            }
        }
    }

    /// <param name="bondMismatch">
    /// The peer rejected or lacked our key while Windows still holds a pairing —
    /// see <see cref="AdapterResetSignature"/>. This is what a firmware install
    /// looks like from the host, and it is terminal for the saved relationship:
    /// retrying cannot succeed, because the adapter has no key to authenticate
    /// with. Escalate straight to repair rather than leaving the attempt in
    /// <c>Failed</c>, where an automatic reconnect will simply try again. Six such
    /// attempts across fourteen minutes were observed on the Android side before
    /// the OS dropped its own bond and repair finally triggered.
    /// </param>
    /// <param name="repairMessage">
    /// What to tell the user when <paramref name="bondMismatch"/> holds. Defaults
    /// to the remembered-adapter wording, which names the Repair action. The pair
    /// flow overrides it, because a stale bond met there has no remembered row and
    /// therefore no Repair button to point at.
    /// </param>
    public AdapterLifecycleDecision ConnectionFailed(
        long connectionGeneration,
        string message,
        bool bondMismatch = false,
        string? repairMessage = null)
    {
        lock (gate)
        {
            if (activeAttempt is not { } attempt || attempt.Generation != connectionGeneration)
            {
                return AdapterLifecycleDecision.Ignored.Instance;
            }

            activeAttempt = null;
            if (bondMismatch)
            {
                var repair = repairMessage ?? AdapterResetSignature.RepairMessage;
                status = status with
                {
                    Phase = AdapterRelationshipPhase.RepairRequired,
                    Message = repair,
                };
                return new AdapterLifecycleDecision.RepairRequired(repair);
            }

            status = status with { Phase = AdapterRelationshipPhase.Failed, Message = message };
            return AdapterLifecycleDecision.Ignored.Instance;
        }
    }

    /// <summary>A verified session ended without deleting any relationship truth.</summary>
    public bool ConnectionEnded(string? message = null)
    {
        lock (gate)
        {
            if (status.Phase != AdapterRelationshipPhase.Connected)
            {
                return false;
            }

            generation += 1;
            activeAttempt = null;
            candidate = null;
            status = status with
            {
                Phase = savedRelationship is null
                    ? AdapterRelationshipPhase.NoRelationship
                    : AdapterRelationshipPhase.Idle,
                Generation = generation,
                Reason = null,
                Message = message,
            };
            return true;
        }
    }

    public void DiscoveryFailed(long discoveryGeneration, string message)
    {
        lock (gate)
        {
            if (discoveryGeneration != generation ||
                status.Phase != AdapterRelationshipPhase.Discovering)
            {
                return;
            }

            activeAttempt = null;
            candidate = null;
            status = status with
            {
                Phase = savedRelationship is null
                    ? AdapterRelationshipPhase.NoRelationship
                    : AdapterRelationshipPhase.Idle,
                Message = message,
            };
        }
    }

    /// <summary>
    /// Windows dropped its pairing for a saved adapter while nothing was in flight.
    ///
    /// The saved relationship is retained: what is broken is the Windows-side
    /// trust, and repair replaces exactly that.
    /// </summary>
    public AdapterLifecycleDecision PairingLost(string address)
    {
        lock (gate)
        {
            if (activeAttempt is not null ||
                !SameAddress(savedRelationship?.Address, address))
            {
                return AdapterLifecycleDecision.Ignored.Instance;
            }

            status = status with
            {
                Phase = AdapterRelationshipPhase.RepairRequired,
                Pairing = WindowsPairingState.NotPaired,
                Message = PairingLostMessage,
            };
            return new AdapterLifecycleDecision.RepairRequired(PairingLostMessage);
        }
    }

    public long CancelAndRetainRelationship(string? message = null)
    {
        lock (gate)
        {
            generation += 1;
            activeAttempt = null;
            candidate = null;
            status = new AdapterRelationshipStatus
            {
                Phase = savedRelationship is null
                    ? AdapterRelationshipPhase.NoRelationship
                    : AdapterRelationshipPhase.Idle,
                Generation = generation,
                Pairing = status.Pairing,
                Message = message,
            };
            return generation;
        }
    }

    public void Forget()
    {
        lock (gate)
        {
            generation += 1;
            savedRelationship = null;
            candidate = null;
            activeAttempt = null;
            status = new AdapterRelationshipStatus
            {
                Phase = AdapterRelationshipPhase.NoRelationship,
                Generation = generation,
                Pairing = WindowsPairingState.Unknown,
            };
        }
    }

    public void Restore(AdapterRelationship? relationship, WindowsPairingState pairing)
    {
        lock (gate)
        {
            savedRelationship = relationship;
            status = status with
            {
                Phase =
                    status.AttemptActive ? status.Phase
                    : status.Phase == AdapterRelationshipPhase.Connected && relationship is not null
                        ? AdapterRelationshipPhase.Connected
                    : relationship is null ? AdapterRelationshipPhase.NoRelationship
                    : AdapterRelationshipPhase.Idle,
                Pairing = pairing,
                Message = status.Phase == AdapterRelationshipPhase.Connected ? status.Message : null,
            };
        }
    }

    private static bool SameAddress(string? left, string? right) =>
        left is not null && right is not null &&
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private static AdapterRelationship Merge(AdapterRelationship old, AdapterRelationship fresh) => old with
    {
        DisplayName = string.IsNullOrWhiteSpace(fresh.DisplayName) ? old.DisplayName : fresh.DisplayName,
    };
}
