namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// The Controller Link binary data plane, riding the management session that is
/// already open.
///
/// Deliberately narrow. It is a byte pipe with a measured MTU and nothing else:
/// no lifecycle, no pairing, no state machine. Those belong to
/// <c>ControllerLinkService</c>, which already owns them and does not care which
/// carrier is underneath.
///
/// There is exactly ONE Bluetooth session in the product and the management
/// owner holds it. This interface hands out access to two more characteristics
/// on that session; it never opens a connection of its own. That is the whole
/// point of Path C — Windows cannot hold a second LE relationship to the
/// adapter, which is what killed the HOGP carrier
/// (<c>docs/experiments/windows-hogp-legacy-advertising-2026-09-02.md</c>).
/// </summary>
public interface IControllerLinkDataPlane : IAsyncDisposable
{
    /// <summary>
    /// The negotiated ATT MTU for the live link, read through the session
    /// rather than cached: Windows can raise the PDU after the link is up, and
    /// a value captured too early would pin every later write to the smaller
    /// size. Compared against
    /// <c>ControllerLinkDataPlane.MinimumAttMtu</c> before streaming starts.
    /// </summary>
    int AttMtu { get; }

    /// <summary>Feedback frames from the adapter, raw. Decoding is the caller's.</summary>
    event Action<byte[]>? OutputFrameReceived;

    /// <summary>
    /// Raised when the underlying management session goes away. Controller Link
    /// cannot outlive its carrier, and unlike the HOGP host there is nothing
    /// separate left running that would need stopping.
    /// </summary>
    event Action<string>? Closed;

    /// <summary>
    /// Resolve the two characteristics and subscribe to feedback. Returns false
    /// when the firmware has no data plane — an older build simply does not
    /// declare these characteristics — which the caller reports as
    /// "update the adapter", not as a failure.
    /// </summary>
    Task<bool> OpenAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Hand the newest gameplay frame to the writer.
    ///
    /// Never blocks and never queues. The implementation holds ONE latest-state
    /// mailbox with at most one write in flight: a frame arriving while a write
    /// is outstanding REPLACES the pending frame rather than joining a queue,
    /// because an older controller state has no value once a newer one exists.
    ///
    /// <c>WriteWithoutResponse</c> means the adapter sends no ATT Write
    /// Response — not that the local WinRT operation may be discarded.
    /// Discarding it would leave unbounded overlapping operations in the stack
    /// with no backpressure and no completion status, which is an unbounded
    /// queue of stale frames built out of concurrency instead of out of a list.
    ///
    /// Still write-without-response: no ATT response, no gameplay ACK, no
    /// per-frame round trip, and no retransmission of a failed historical
    /// state. A failed write is superseded, not retried.
    /// </summary>
    void PublishInput(ReadOnlySpan<byte> frame);

    /// <summary>Frames handed to the writer by the report scheduler.</summary>
    long StatesPublished { get; }

    /// <summary>
    /// Frames replaced in the mailbox before they could be sent — latest-state
    /// -wins doing its job. A large ratio means the radio is behind the
    /// scheduler, which is a measurement, not a fault.
    /// </summary>
    long StatesCoalesced { get; }

    /// <summary>GATT writes actually issued and accepted by the stack.</summary>
    long FramesWritten { get; }

    long FrameWriteFailures { get; }

    long OutputFramesReceived { get; }

    /// <summary>
    /// Must never exceed 1. Exposed rather than asserted so hardware
    /// qualification can prove the writer stayed bounded instead of assuming it.
    /// </summary>
    int MaximumInFlight { get; }

    /// <summary>Local <c>WriteValueWithResultAsync</c> completion latency.</summary>
    TimeSpan AverageWriteLatency { get; }

    TimeSpan MaximumWriteLatency { get; }
}

/// <summary>
/// Obtains a data plane on the live management session. Implemented by the BLE
/// management transport, which owns that session.
/// </summary>
public interface IControllerLinkDataPlaneProvider
{
    /// <summary>
    /// Null when there is no live trusted session to attach to. Controller Link
    /// requires trusted management, so that is a refusal, not an error.
    /// </summary>
    IControllerLinkDataPlane? TryCreateDataPlane();
}
