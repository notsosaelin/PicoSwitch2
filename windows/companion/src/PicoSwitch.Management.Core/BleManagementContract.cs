namespace PicoSwitch.Management;

/// <summary>
/// BLE carrier constants and fragmentation, separate from logical command
/// semantics.
///
/// These constants live HERE and never in <see cref="ManagementProtocol"/>: the
/// logical newline/JSON contract is carrier-neutral and is also spoken over
/// UART. The architecture guard test asserts that separation, mirroring the
/// Kotlin <c>ArchitectureGuardTest</c>.
/// </summary>
public static class BleManagementContract
{
    public const int DefaultAttMtu = 23;
    public const string ServiceUuid = "7c5ad4ed-2731-417c-b316-058505c7c083";
    public const string RxUuid = "5252186a-817f-489f-ad75-94c3bd444769";
    public const string TxUuid = "81462706-8e64-407a-bc3d-d303529fbe1c";
    public const int AttPayloadWithDefaultMtu = 20;
    public const int MaxReplyPayloadBytes = 511;

    public static IReadOnlyList<byte[]> CommandChunks(string command, int payloadBytes)
    {
        if (payloadBytes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(payloadBytes), "Payload size must be positive");
        }

        var framed = ManagementProtocol.Frame(command);
        var chunks = new List<byte[]>((framed.Length + payloadBytes - 1) / payloadBytes);
        for (var offset = 0; offset < framed.Length; offset += payloadBytes)
        {
            var length = Math.Min(payloadBytes, framed.Length - offset);
            chunks.Add(framed.AsSpan(offset, length).ToArray());
        }

        return chunks;
    }
}

/// <summary>Incremental BLE notification assembly for one owned logical exchange.</summary>
public sealed class BleReplyAssembler
{
    private readonly int maxPayloadBytes;
    private readonly List<byte> payload = [];

    public BleReplyAssembler(int maxPayloadBytes = BleManagementContract.MaxReplyPayloadBytes)
    {
        if (maxPayloadBytes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxPayloadBytes), "Reply limit must be positive");
        }

        this.maxPayloadBytes = maxPayloadBytes;
    }

    /// <summary>
    /// Returns the complete reply at LF, or null while more notification bytes
    /// are required.
    /// </summary>
    public string? Accept(ReadOnlySpan<byte> fragment)
    {
        foreach (var value in fragment)
        {
            if (value == (byte)'\n')
            {
                if (payload.Count > 0 && payload[^1] == (byte)'\r')
                {
                    payload.RemoveAt(payload.Count - 1);
                }

                return System.Text.Encoding.UTF8.GetString(payload.ToArray());
            }

            payload.Add(value);
            if (payload.Count > maxPayloadBytes)
            {
                throw new ManagementReplyTooLargeException(
                    $"Adapter reply exceeded the {maxPayloadBytes}-byte wireless payload limit");
            }
        }

        return null;
    }
}
