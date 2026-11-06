namespace PicoSwitch.Management;

/// <summary>
/// One already-connected logical management channel.
///
/// Discovery, pairing, operating-system peer handles, GATT lifecycle, and UI
/// state intentionally do not belong here. Implementations must return exactly
/// one complete reply for the command or fail the current session.
/// </summary>
public interface IManagementChannel
{
    Task<string> TransactAsync(
        string command,
        long timeoutMillis = ManagementChannel.DefaultTimeoutMillis,
        CancellationToken cancellationToken = default);
}

public static class ManagementChannel
{
    public const long DefaultTimeoutMillis = 10_000L;
}

public class ManagementException : Exception
{
    public ManagementException(string message, Exception? innerException = null)
        : base(message, innerException)
    {
    }
}

public sealed class ManagementReplyTooLargeException(string message) : ManagementException(message);

public sealed class AdapterCommandException(string command, int? code, string adapterMessage)
    : ManagementException(adapterMessage)
{
    public string Command { get; } = command;

    public int? Code { get; } = code;

    public string AdapterMessage { get; } = adapterMessage;

    /// <summary>
    /// Whether this error means "this firmware does not have that command".
    ///
    /// Matched on the adapter's own message shape rather than on an exception
    /// type, because that is what keeps an older adapter usable instead of
    /// failing a whole refresh. Ported verbatim from the Kotlin
    /// <c>AdapterCommandException.isUnsupported()</c>; see WINDOWS_PASS.md §13.2
    /// rule 6.
    /// </summary>
    public bool IsUnsupported() =>
        AdapterMessage.Equals("unknown command", StringComparison.OrdinalIgnoreCase) ||
        AdapterMessage.Equals("unavailable", StringComparison.OrdinalIgnoreCase) ||
        AdapterMessage.StartsWith("unavailable in ", StringComparison.OrdinalIgnoreCase) ||
        AdapterMessage.Equals(
            "command unavailable over Bluetooth",
            StringComparison.OrdinalIgnoreCase);
}

/// <summary>The adapter answered, but with something this build cannot use.</summary>
public class ManagementProtocolException(string message, Exception? innerException = null)
    : ManagementException(message, innerException);

/// <summary>
/// A multi-reply read did not reconstruct a complete, consistent result.
/// </summary>
/// <remarks>
/// Derives from <see cref="ManagementProtocolException"/> because that is what it
/// is: the adapter said something unusable. They were siblings, so a handler that
/// meant "the adapter's answer was bad" had to name both, and one that named only
/// the base type let a pagination failure escape as an unhandled transport error.
/// </remarks>
public sealed class ManagementPaginationException(string message)
    : ManagementProtocolException(message);
