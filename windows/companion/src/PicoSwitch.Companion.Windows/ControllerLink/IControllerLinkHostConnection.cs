namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Testable boundary around the packaged AppContainer HOGP host. Services owns
/// orchestration; this interface owns only activation, IPC, and report transport.
/// </summary>
public interface IControllerLinkHostConnection : IAsyncDisposable
{
    event Action<ControllerLinkHostState, string?>? StateChanged;

    event Action<ControllerLinkOutputReport>? OutputReportReceived;

    event Action<string>? Closed;

    HostHello? Handshake { get; }

    long InputReportsQueued { get; }

    long InputReportsSent { get; }

    long InputReportsCoalesced { get; }

    long OutputReportsReceived { get; }

    Task StartAsync(CancellationToken cancellationToken = default);

    void PublishInput(ReadOnlySpan<byte> report);

    Task StopAsync(CancellationToken cancellationToken = default);
}

public interface IControllerLinkHostFactory
{
    Task<IControllerLinkHostConnection> OpenAsync(CancellationToken cancellationToken = default);
}

public sealed class ControllerLinkHostFactory : IControllerLinkHostFactory
{
    public static readonly ControllerLinkHostFactory Instance = new();

    public async Task<IControllerLinkHostConnection> OpenAsync(
        CancellationToken cancellationToken = default) =>
        await ControllerLinkHostConnection.OpenAsync(cancellationToken).ConfigureAwait(false);
}
