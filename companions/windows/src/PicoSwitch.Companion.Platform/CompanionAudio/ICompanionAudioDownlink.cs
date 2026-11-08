namespace PicoSwitch.Companion.Platform.CompanionAudio;

/// <summary>
/// Companion audio downlink (EXPERIMENTAL): adapter-to-companion notifications
/// on the live management GATT service, one Opus packet each. Wire contract:
/// <c>firmware/src/management/ns2_companion_audio.h</c>.
///
/// Like the Controller Link data plane, this opens no connection of its own: it
/// subscribes to one more characteristic on the session the management owner
/// already holds, and unsubscribes again on dispose.
/// </summary>
public interface ICompanionAudioDownlink : IAsyncDisposable
{
    /// <summary>
    /// Resolve the characteristic and subscribe. False when the adapter does
    /// not expose the plane or the subscription is refused.
    /// </summary>
    Task<bool> OpenAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// One raw notification value, delivered on a platform callback thread.
    /// Handlers must be quick: decode and hand off, never block.
    /// </summary>
    event Action<byte[]>? FrameReceived;

    int AttMtu { get; }

    long NotificationsReceived { get; }
}

/// <summary>Obtains a companion audio downlink on the live management session.</summary>
public interface ICompanionAudioDownlinkProvider
{
    /// <summary>Null when there is no live session to attach to.</summary>
    ICompanionAudioDownlink? TryCreateAudioDownlink();
}

/// <summary>
/// Local playback of decoded PCM. Implementations own a BOUNDED buffer: when
/// the companion falls behind, they drop audio rather than let latency grow.
/// </summary>
public interface ICompanionAudioSink : IDisposable
{
    /// <summary>Open the output device. Interleaved signed 16-bit PCM.</summary>
    void Start(int sampleRate, int channels);

    /// <summary>
    /// Queue one block of interleaved samples. Returns false when the block was
    /// dropped because the bounded buffer was full.
    /// </summary>
    bool Write(ReadOnlySpan<short> interleaved);

    void Stop();

    /// <summary>Audio queued but not yet played, in milliseconds.</summary>
    int QueuedMillis { get; }

    /// <summary>Times the device ran dry while playing.</summary>
    long Underruns { get; }

    /// <summary>
    /// The prebuffer the sink currently waits for, in milliseconds -- what the
    /// jitter policy has grown it to (<see cref="CompanionAudioJitterPolicy"/>).
    /// </summary>
    int PrebufferMillis { get; }

    /// <summary>
    /// True once after the sink decided to deepen its buffer without a gap
    /// (<see cref="CompanionAudioPlayoutDecision.Extend"/>): the caller then
    /// writes one packet-loss-concealment block from the decoder.
    /// </summary>
    bool TakeConcealRequest() => false;

    /// <summary>Concealment blocks the sink asked for to deepen its buffer.</summary>
    long Concealments => 0;
}

/// <summary>Creates the platform's playback sink, or null when it has none.</summary>
public interface ICompanionAudioSinkFactory
{
    ICompanionAudioSink? CreateSink();
}
