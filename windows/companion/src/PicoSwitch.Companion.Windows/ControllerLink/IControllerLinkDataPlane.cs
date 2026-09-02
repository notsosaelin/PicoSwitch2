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
    /// Send one gameplay frame, write-without-response.
    ///
    /// Returns false rather than throwing on a failed write: at 125 Hz a
    /// dropped frame is superseded by the next one, and tearing down a healthy
    /// link over one lost packet would be worse than the packet. Persistent
    /// failure surfaces through the counters and, eventually, <see cref="Closed"/>.
    /// </summary>
    bool TryWriteInput(ReadOnlySpan<byte> frame);

    long FramesWritten { get; }

    long FrameWriteFailures { get; }

    long OutputFramesReceived { get; }
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
