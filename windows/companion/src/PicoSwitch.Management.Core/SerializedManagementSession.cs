namespace PicoSwitch.Management;

/// <summary>
/// Single-flight ownership for carriers whose replies have no request id.
///
/// Cancellation while waiting for ownership is normal. Once
/// <see cref="ExchangeAsync{T}"/> starts, however, the operation runs to
/// completion so its reply is consumed (or the backend invalidates the session)
/// before another caller can transmit. This prevents a late reply from becoming
/// the next request's reply — the failure mode that exists because management
/// replies carry no request identifier (WINDOWS_PASS.md §4.1).
///
/// The C# expression of Kotlin's <c>withContext(NonCancellable)</c> is simply
/// NOT forwarding the caller's token into the operation: a cancelled caller
/// stops awaiting, the exchange still finishes, and the gate is still released.
/// </summary>
public sealed class SerializedManagementSession
{
    private readonly SemaphoreSlim gate = new(1, 1);

    public async Task<T> ExchangeAsync<T>(
        Func<Task<T>> operation,
        CancellationToken cancellationToken = default)
    {
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await operation().ConfigureAwait(false);
        }
        finally
        {
            gate.Release();
        }
    }

    /// <summary>
    /// Serialize lifecycle mutation, such as disconnect, against an exchange.
    /// Once ownership is acquired, cleanup also runs to completion so caller
    /// cancellation cannot expose a half-invalidated carrier to another owner.
    /// </summary>
    public Task<T> MutateAsync<T>(
        Func<Task<T>> operation,
        CancellationToken cancellationToken = default) =>
        ExchangeAsync(operation, cancellationToken);

    public async Task MutateAsync(
        Func<Task> operation,
        CancellationToken cancellationToken = default) =>
        await ExchangeAsync<object?>(
            async () =>
            {
                await operation().ConfigureAwait(false);
                return null;
            },
            cancellationToken).ConfigureAwait(false);
}
