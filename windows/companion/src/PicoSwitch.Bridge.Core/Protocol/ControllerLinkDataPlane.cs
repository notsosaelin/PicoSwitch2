namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// Windows Controller Link (Path C) data-plane framing — the Windows half of
/// the contract <c>include/ns2_companion_link.h</c> defines.
///
/// Gameplay rides a dedicated binary characteristic on the trusted management
/// link, never the newline-JSON command channel: 125 Hz of controller state on
/// a single-flight request/response channel would starve management, which is
/// the whole reason this data plane exists.
///
/// Windows has no Classic HID Device role and cannot hold a second LE
/// relationship to the adapter, so there is no second connection to carry this.
/// See <c>docs/experiments/windows-classic-hid-device-feasibility-2026-09-02.md</c>
/// and <c>docs/experiments/windows-hogp-legacy-advertising-2026-09-02.md</c>.
/// </summary>
public static class ControllerLinkDataPlane
{
    /// <summary>
    /// Data-plane version. Must equal <c>NS2_COMPANION_LINK_VERSION</c>; a
    /// mismatch is refused at Start through the control plane rather than
    /// discovered as garbage input.
    /// </summary>
    public const byte Version = 1;

    public const byte OpcodeState = 0x01;   // Windows -> adapter
    public const byte OpcodeOutput = 0x81;  // adapter -> Windows

    /// <summary><c>[version][opcode][sequence lo][sequence hi]</c></summary>
    public const int HeaderBytes = 4;

    /// <summary>
    /// The canonical v2 payload, report ID excluded — the exact bytes
    /// <see cref="ControllerReportEncoder.Encode"/> produces.
    /// </summary>
    public const int PayloadBytes = ControllerReportEncoder.PayloadSizeV2;

    public const int FrameBytes = HeaderBytes + PayloadBytes;

    /// <summary>
    /// One gameplay frame must fit one ATT operation: 30-byte value plus three
    /// bytes of ATT Write Command overhead. Below this every report fragments
    /// onto the command path, so the transport refuses to start rather than
    /// silently degrading — and it MEASURES the negotiated MTU, never assumes.
    /// </summary>
    public const int MinimumAttMtu = FrameBytes + 3;

    /// <summary><c>[version][opcode][report id]</c> then the feedback body.</summary>
    public const int OutputHeaderBytes = 3;

    /// <summary>
    /// Derived from the canonical output contract, not chosen:
    /// <see cref="BridgeOutputCodec.BodySize"/>, C
    /// <c>ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN</c> and
    /// <c>ANDROID_BRIDGE_FEEDBACK_MAX_LEN</c> all agree.
    /// </summary>
    public const int OutputMaxPayload = BridgeOutputCodec.BodySize;

    /// <summary>
    /// Frame one encoded report for transmission.
    ///
    /// The sequence number is a defensive protocol property, not a claim that
    /// ATT reorders write commands: it guarantees a stale, duplicated or
    /// out-of-order producer frame can never overwrite newer state on the
    /// adapter, whatever the scheduler or a future carrier does.
    /// </summary>
    public static void EncodeInput(ReadOnlySpan<byte> payload, ushort sequence, Span<byte> destination)
    {
        if (payload.Length != PayloadBytes)
        {
            throw new ArgumentException(
                $"Controller Link payload must be {PayloadBytes} bytes, got {payload.Length}.",
                nameof(payload));
        }

        if (destination.Length < FrameBytes)
        {
            throw new ArgumentException(
                $"Controller Link frame needs {FrameBytes} bytes.", nameof(destination));
        }

        destination[0] = Version;
        destination[1] = OpcodeState;
        destination[2] = (byte)(sequence & 0xFF);
        destination[3] = (byte)(sequence >> 8);
        payload.CopyTo(destination[HeaderBytes..]);
    }

    /// <summary>
    /// Decode one adapter-to-Windows feedback frame, or null when it cannot be
    /// one. Returning null rather than throwing keeps a stray notification from
    /// tearing down a healthy link — it is counted and ignored.
    /// </summary>
    public static ControllerLinkOutputFrame? DecodeOutput(ReadOnlySpan<byte> frame)
    {
        if (frame.Length < OutputHeaderBytes)
        {
            return null;
        }

        if (frame[0] != Version || frame[1] != OpcodeOutput)
        {
            return null;
        }

        var body = frame[OutputHeaderBytes..];
        if (body.Length > OutputMaxPayload)
        {
            return null;
        }

        return new ControllerLinkOutputFrame(frame[2], body.ToArray());
    }

    /// <summary>Does the negotiated ATT MTU carry one whole gameplay frame?</summary>
    public static bool MtuSufficient(int attMtu) => attMtu >= MinimumAttMtu;
}

/// <summary>One decoded feedback frame: the report id and its raw body.</summary>
public sealed record ControllerLinkOutputFrame(byte ReportId, byte[] Payload);
