using Windows.ApplicationModel;
using Windows.ApplicationModel.AppService;
using Windows.Foundation.Collections;

namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Owns the lifetime lease for the hidden AppContainer Controller Link host.
/// Control traffic uses the packaged app-service channel; the report-rate path
/// is deliberately separate and will use the package-scoped named pipe.
/// </summary>
public sealed class ControllerLinkHostConnection : IAsyncDisposable
{
    public const string AppServiceName = "PicoSwitch.ControllerLink.Host";
    public const int IpcVersion = 1;
    public const int BridgeContract = 4;
    public const int DescriptorBytes = 161;
    public const string DescriptorSha256 =
        "f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054";

    private readonly AppServiceConnection connection;
    private AppServiceClosedStatus? lastCloseStatus;
    private bool disposed;

    private ControllerLinkHostConnection(AppServiceConnection connection)
    {
        this.connection = connection;
        connection.ServiceClosed += (_, args) => lastCloseStatus = args.Status;
    }

    public static async Task<ControllerLinkHostConnection> OpenAsync(
        CancellationToken cancellationToken = default)
    {
        string packageFamily;
        try
        {
            packageFamily = Package.Current.Id.FamilyName;
        }
        catch (InvalidOperationException error)
        {
            throw new ControllerLinkHostException(
                "Controller Link requires the packaged PicoSwitch Companion build.",
                error);
        }

        var connection = new AppServiceConnection
        {
            AppServiceName = AppServiceName,
            PackageFamilyName = packageFamily,
        };

        var openStatus = await connection.OpenAsync().AsTask(cancellationToken).ConfigureAwait(false);
        if (openStatus != AppServiceConnectionStatus.Success)
        {
            connection.Dispose();
            throw new ControllerLinkHostException($"Controller Link host activation failed: {openStatus}.");
        }

        var client = new ControllerLinkHostConnection(connection);
        try
        {
            var hello = await client.SendAsync("hello", cancellationToken).ConfigureAwait(false);
            client.ValidateHandshake(hello);
            return client;
        }
        catch
        {
            await client.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        var response = await SendAsync("start", cancellationToken).ConfigureAwait(false);
        if (!ReadBoolean(response, "ok"))
        {
            throw new ControllerLinkHostException(
                $"Controller Link advertising did not start: {ReadString(response, "error") ?? ReadString(response, "status") ?? "unknown error"}.");
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        if (disposed)
        {
            return;
        }

        var response = await SendAsync("stop", cancellationToken).ConfigureAwait(false);
        if (!ReadBoolean(response, "ok"))
        {
            throw new ControllerLinkHostException(
                $"Controller Link host refused Stop: {ReadString(response, "error") ?? "unknown error"}.");
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        connection.Dispose();
        await Task.CompletedTask.ConfigureAwait(false);
    }

    private async Task<ValueSet> SendAsync(string command, CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var message = new ValueSet { ["command"] = command };
        var result = await connection.SendMessageAsync(message).AsTask(cancellationToken).ConfigureAwait(false);
        if (result.Status != AppServiceResponseStatus.Success)
        {
            throw new ControllerLinkHostException(
                $"Controller Link host request '{command}' failed: {result.Status}; " +
                $"serviceClosed={lastCloseStatus?.ToString() ?? "not reported"}.");
        }

        return result.Message;
    }

    private void ValidateHandshake(ValueSet response)
    {
        if (!ReadBoolean(response, "ok") ||
            ReadInt32(response, "ipcVersion") != IpcVersion ||
            ReadInt32(response, "bridgeContract") != BridgeContract ||
            ReadInt32(response, "descriptorBytes") != DescriptorBytes ||
            !string.Equals(ReadString(response, "descriptorSha256"), DescriptorSha256, StringComparison.Ordinal))
        {
            throw new ControllerLinkHostException(
                "Controller Link unavailable: installed components are out of sync " +
                $"(ipc={ReadInt32(response, "ipcVersion")?.ToString() ?? "missing"}, " +
                $"bridge={ReadInt32(response, "bridgeContract")?.ToString() ?? "missing"}, " +
                $"descriptorBytes={ReadInt32(response, "descriptorBytes")?.ToString() ?? "missing"}, " +
                $"descriptorSha256={ReadString(response, "descriptorSha256") ?? "missing"}).");
        }
    }

    private static bool ReadBoolean(ValueSet values, string key) =>
        values.TryGetValue(key, out var value) && value is bool result && result;

    private static int? ReadInt32(ValueSet values, string key)
    {
        if (!values.TryGetValue(key, out var value) || value is null)
        {
            return null;
        }

        try
        {
            return Convert.ToInt32(value, System.Globalization.CultureInfo.InvariantCulture);
        }
        catch (Exception) when (value is not string)
        {
            return null;
        }
    }

    private static string? ReadString(ValueSet values, string key) =>
        values.TryGetValue(key, out var value) ? value as string : null;
}

public sealed class ControllerLinkHostException : Exception
{
    public ControllerLinkHostException(string message) : base(message) { }

    public ControllerLinkHostException(string message, Exception innerException)
        : base(message, innerException) { }
}
