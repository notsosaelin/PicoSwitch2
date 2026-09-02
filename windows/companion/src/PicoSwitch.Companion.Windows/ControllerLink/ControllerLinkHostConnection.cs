using System.Buffers.Binary;
using System.ComponentModel;
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Text;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Windows.ApplicationModel;
using Windows.ApplicationModel.AppService;
using Windows.Foundation.Collections;

namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Owns the lifetime lease and authenticated pipe to the hidden AppContainer
/// Controller Link host. App-service messages only activate and stop the host;
/// all report-rate traffic uses the fixed binary pipe contract.
/// </summary>
public sealed class ControllerLinkHostConnection : IAsyncDisposable
{
    public const string AppServiceName = "PicoSwitch.ControllerLink.Host";
    public const int IpcVersion = ControllerLinkIpcProtocol.Version;
    public const int BridgeContract = 4;
    public const int DescriptorBytes = 161;
    public const string DescriptorSha256 =
        "f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054";

    private static readonly TimeSpan ActivationDeadline = TimeSpan.FromSeconds(8);
    private static readonly TimeSpan HeartbeatInterval = TimeSpan.FromMilliseconds(500);

    private readonly AppServiceConnection connection;
    private readonly string packageFamilyName;
    private readonly SemaphoreSlim lifecycleGate = new(1, 1);
    private readonly SemaphoreSlim writeGate = new(1, 1);
    private readonly SemaphoreSlim inputAvailable = new(0, 1);
    private readonly object inputGate = new();

    private NamedPipeClientStream? pipe;
    private CancellationTokenSource? sessionCancellation;
    private Task? readerTask;
    private Task? heartbeatTask;
    private Task? inputTask;
    private byte[]? pendingInput;
    private AppServiceClosedStatus? lastCloseStatus;
    private ulong outgoingSequence;
    private bool started;
    private bool disposed;

    private ControllerLinkHostConnection(
        AppServiceConnection connection,
        string packageFamilyName)
    {
        this.connection = connection;
        this.packageFamilyName = packageFamilyName;
        connection.ServiceClosed += (_, args) =>
        {
            lastCloseStatus = args.Status;
            sessionCancellation?.Cancel();
            Closed?.Invoke($"Controller Link host closed: {args.Status}.");
        };
    }

    public event Action<ControllerLinkHostState, string?>? StateChanged;

    public event Action<ControllerLinkOutputReport>? OutputReportReceived;

    public event Action<string>? Closed;

    public HostHello? Handshake { get; private set; }

    public long InputReportsQueued { get; private set; }

    public long InputReportsSent { get; private set; }

    public long InputReportsCoalesced { get; private set; }

    public long OutputReportsReceived { get; private set; }

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

        var client = new ControllerLinkHostConnection(connection, packageFamily);
        try
        {
            var hello = await client.SendAsync("hello", null, cancellationToken).ConfigureAwait(false);
            client.ValidateAppServiceHandshake(hello);
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
        ObjectDisposedException.ThrowIf(disposed, this);
        await lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (started)
            {
                return;
            }

            var challenge = ControllerLinkIpcProtocol.CreateChallenge();
            var pipeName = $"LOCAL\\PicoSwitch.ControllerLink.{Guid.NewGuid():N}";
            var packageSid = DerivePackageSid(packageFamilyName);

            // LOCAL is intentionally interpreted by each caller. Inside the
            // helper it resolves to the package's AppContainerNamedObjects
            // directory; a medium-IL desktop process resolves LOCAL to its
            // ordinary session namespace instead. Address the helper's private
            // namespace explicitly on this side so the binary channel remains
            // inside the package container.
            var appContainerPipeName =
                $"Sessions\\{Process.GetCurrentProcess().SessionId}\\" +
                ResolveAppContainerNamedObjectPath(packageSid) + "\\" +
                pipeName["LOCAL\\".Length..];
            var client = new NamedPipeClientStream(
                ".",
                appContainerPipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            pipe = client;

            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(ActivationDeadline);
            var startResponseTask = SendAsync(
                "start",
                new Dictionary<string, object>
                {
                    ["pipeName"] = pipeName,
                    ["challenge"] = Convert.ToHexString(challenge).ToLowerInvariant(),
                },
                deadline.Token);

            await client.ConnectAsync(deadline.Token).ConfigureAwait(false);
            VerifySamePackageServer(client);

            var helloFrame = await ControllerLinkIpcProtocol.ReadAsync(client, deadline.Token)
                .ConfigureAwait(false);
            if (helloFrame.Type != ControllerLinkMessageType.HostHello)
            {
                throw new ControllerLinkProtocolException(
                    $"Expected HostHello, received {helloFrame.Type}.");
            }

            Handshake = ControllerLinkIpcProtocol.ParseAndValidateHostHello(
                helloFrame.Payload,
                challenge);
            await WriteFrameAsync(
                    ControllerLinkMessageType.MainHello,
                    ReadOnlyMemory<byte>.Empty,
                    deadline.Token)
                .ConfigureAwait(false);

            var response = await startResponseTask.ConfigureAwait(false);
            if (!ReadBoolean(response, "ok"))
            {
                throw new ControllerLinkHostException(
                    "Controller Link advertising did not start: " +
                    (ReadString(response, "error") ?? ReadString(response, "status") ?? "unknown error") + ".");
            }

            sessionCancellation = new CancellationTokenSource();
            readerTask = ReadLoopAsync(sessionCancellation.Token);
            heartbeatTask = HeartbeatLoopAsync(sessionCancellation.Token);
            inputTask = InputLoopAsync(sessionCancellation.Token);
            started = true;
        }
        catch
        {
            await TearDownPipeAsync().ConfigureAwait(false);
            throw;
        }
        finally
        {
            lifecycleGate.Release();
        }
    }

    /// <summary>
    /// Publish the newest complete report. There is one pending slot: replacing
    /// it increments the coalesce counter, so a stalled helper can never replay a
    /// backlog of obsolete stick positions.
    /// </summary>
    public void PublishInput(ReadOnlySpan<byte> report)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (!started)
        {
            throw new InvalidOperationException("Controller Link has not started.");
        }

        if (report.Length != ControllerLinkIpcProtocol.InputReportSize)
        {
            throw new ArgumentException(
                $"Input report must be {ControllerLinkIpcProtocol.InputReportSize} bytes.",
                nameof(report));
        }

        lock (inputGate)
        {
            InputReportsQueued++;
            if (pendingInput is not null)
            {
                InputReportsCoalesced++;
            }

            pendingInput = report.ToArray();
            if (inputAvailable.CurrentCount == 0)
            {
                inputAvailable.Release();
            }
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        await lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!started)
            {
                return;
            }

            var neutral = ControllerReportEncoder.Encode(ControllerState.Neutral);
            try
            {
                await WriteFrameAsync(
                        ControllerLinkMessageType.InputReport,
                        neutral,
                        cancellationToken)
                    .ConfigureAwait(false);
                await WriteFrameAsync(
                        ControllerLinkMessageType.Stop,
                        ReadOnlyMemory<byte>.Empty,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (IOException)
            {
            }

            var response = await SendAsync("stop", null, cancellationToken).ConfigureAwait(false);
            if (!ReadBoolean(response, "ok"))
            {
                throw new ControllerLinkHostException(
                    $"Controller Link host refused Stop: {ReadString(response, "error") ?? "unknown error"}.");
            }
        }
        finally
        {
            started = false;
            await TearDownPipeAsync().ConfigureAwait(false);
            lifecycleGate.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        try
        {
            await StopAsync().ConfigureAwait(false);
        }
        catch
        {
        }

        disposed = true;
        connection.Dispose();
        lifecycleGate.Dispose();
        writeGate.Dispose();
        inputAvailable.Dispose();
    }

    private async Task InputLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (true)
            {
                await inputAvailable.WaitAsync(cancellationToken).ConfigureAwait(false);
                byte[]? report;
                lock (inputGate)
                {
                    report = pendingInput;
                    pendingInput = null;
                }

                if (report is null)
                {
                    continue;
                }

                await WriteFrameAsync(
                        ControllerLinkMessageType.InputReport,
                        report,
                        cancellationToken)
                    .ConfigureAwait(false);
                InputReportsSent++;
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Closed?.Invoke($"Controller Link input channel failed: {error.Message}");
            sessionCancellation?.Cancel();
        }
    }

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            using var timer = new PeriodicTimer(HeartbeatInterval);
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                await WriteFrameAsync(
                        ControllerLinkMessageType.Heartbeat,
                        ReadOnlyMemory<byte>.Empty,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Closed?.Invoke($"Controller Link heartbeat failed: {error.Message}");
            sessionCancellation?.Cancel();
        }
    }

    private async Task ReadLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (pipe is { IsConnected: true } stream)
            {
                var frame = await ControllerLinkIpcProtocol.ReadAsync(stream, cancellationToken)
                    .ConfigureAwait(false);
                switch (frame.Type)
                {
                    case ControllerLinkMessageType.HostState:
                        if (frame.Payload.Length < 1 ||
                            !Enum.IsDefined(typeof(ControllerLinkHostState), frame.Payload[0]))
                        {
                            throw new ControllerLinkProtocolException("Malformed host-state frame.");
                        }

                        var detail = frame.Payload.Length > 1
                            ? Encoding.UTF8.GetString(frame.Payload, 1, frame.Payload.Length - 1)
                            : null;
                        StateChanged?.Invoke((ControllerLinkHostState)frame.Payload[0], detail);
                        break;

                    case ControllerLinkMessageType.OutputReport:
                        OutputReportsReceived++;
                        OutputReportReceived?.Invoke(new ControllerLinkOutputReport(
                            frame.Sequence,
                            frame.Timestamp,
                            Stopwatch.GetTimestamp(),
                            frame.Payload));
                        break;

                    case ControllerLinkMessageType.Diagnostics:
                        break;

                    default:
                        throw new ControllerLinkProtocolException(
                            $"Unexpected host-to-main message {frame.Type}.");
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (EndOfStreamException)
        {
            Closed?.Invoke("Controller Link helper disconnected.");
            sessionCancellation?.Cancel();
        }
        catch (Exception error)
        {
            Closed?.Invoke($"Controller Link host channel failed: {error.Message}");
            sessionCancellation?.Cancel();
        }
    }

    private async Task WriteFrameAsync(
        ControllerLinkMessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        var stream = pipe;
        if (stream is null || !stream.IsConnected)
        {
            throw new IOException("Controller Link pipe is not connected.");
        }

        var frame = ControllerLinkIpcProtocol.CreateFrame(
            type,
            unchecked(++outgoingSequence),
            payload.Span);
        var encoded = ControllerLinkIpcProtocol.Encode(frame);
        await writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await stream.WriteAsync(encoded, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            writeGate.Release();
        }
    }

    private async Task TearDownPipeAsync()
    {
        var cancellation = sessionCancellation;
        sessionCancellation = null;
        cancellation?.Cancel();

        var stream = pipe;
        pipe = null;
        stream?.Dispose();

        var tasks = new[] { readerTask, heartbeatTask, inputTask }
            .Where(task => task is not null)
            .Cast<Task>()
            .ToArray();
        readerTask = null;
        heartbeatTask = null;
        inputTask = null;
        if (tasks.Length > 0)
        {
            try
            {
                await Task.WhenAll(tasks).ConfigureAwait(false);
            }
            catch
            {
            }
        }

        cancellation?.Dispose();
        lock (inputGate)
        {
            pendingInput = null;
        }
    }

    private async Task<ValueSet> SendAsync(
        string command,
        IReadOnlyDictionary<string, object>? fields,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var message = new ValueSet { ["command"] = command };
        if (fields is not null)
        {
            foreach (var (key, value) in fields)
            {
                message[key] = value;
            }
        }

        var result = await connection.SendMessageAsync(message).AsTask(cancellationToken).ConfigureAwait(false);
        if (result.Status != AppServiceResponseStatus.Success)
        {
            throw new ControllerLinkHostException(
                $"Controller Link host request '{command}' failed: {result.Status}; " +
                $"serviceClosed={lastCloseStatus?.ToString() ?? "not reported"}.");
        }

        return result.Message;
    }

    private void ValidateAppServiceHandshake(ValueSet response)
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

    private void VerifySamePackageServer(NamedPipeClientStream stream)
    {
        if (!NativeMethods.GetNamedPipeServerProcessId(stream.SafePipeHandle, out var processId))
        {
            throw new ControllerLinkHostException(
                $"Could not identify Controller Link host process ({Marshal.GetLastWin32Error()}).");
        }

        using var process = NativeMethods.OpenProcess(
            NativeMethods.ProcessQueryLimitedInformation,
            false,
            processId);
        if (process.IsInvalid)
        {
            throw new ControllerLinkHostException(
                $"Could not inspect Controller Link host process ({Marshal.GetLastWin32Error()}).");
        }

        uint length = 0;
        _ = NativeMethods.GetPackageFamilyName(process, ref length, null);
        var buffer = new StringBuilder(checked((int)length));
        var result = NativeMethods.GetPackageFamilyName(process, ref length, buffer);
        if (result != 0 || !string.Equals(buffer.ToString(), packageFamilyName, StringComparison.Ordinal))
        {
            throw new ControllerLinkHostException(
                "Controller Link pipe peer does not belong to this PicoSwitch Companion package.");
        }
    }

    /// <summary>
    /// Stable Windows package-SID derivation. The Win32 convenience API must not
    /// be used here: from a process that already has package identity it can
    /// return a child AppContainer SID rather than the package SID.
    /// </summary>
    private static SecurityIdentifier DerivePackageSid(string packageFamily)
    {
        var moniker = Encoding.Unicode.GetBytes(packageFamily.Trim().ToLowerInvariant());
        var hash = SHA256.HashData(moniker);
        var builder = new StringBuilder("S-1-15-2");
        for (var index = 0; index < 7; index++)
        {
            builder.Append('-').Append(
                BinaryPrimitives.ReadUInt32LittleEndian(hash.AsSpan(index * sizeof(uint), sizeof(uint))));
        }

        return new SecurityIdentifier(builder.ToString());
    }

    private static string ResolveAppContainerNamedObjectPath(SecurityIdentifier packageSid)
    {
        var sid = new byte[packageSid.BinaryLength];
        packageSid.GetBinaryForm(sid, 0);
        var sidPointer = Marshal.AllocHGlobal(sid.Length);
        try
        {
            Marshal.Copy(sid, 0, sidPointer, sid.Length);
            _ = NativeMethods.GetAppContainerNamedObjectPath(
                IntPtr.Zero,
                sidPointer,
                0,
                null,
                out var length);
            if (length == 0)
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "Windows did not expose the Controller Link AppContainer object path.");
            }

            var path = new StringBuilder(checked((int)length));
            if (!NativeMethods.GetAppContainerNamedObjectPath(
                    IntPtr.Zero,
                    sidPointer,
                    length,
                    path,
                    out _))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "Could not resolve the Controller Link AppContainer object path.");
            }

            return path.ToString();
        }
        finally
        {
            Marshal.FreeHGlobal(sidPointer);
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

    private static class NativeMethods
    {
        internal const uint ProcessQueryLimitedInformation = 0x1000;

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetNamedPipeServerProcessId(
            Microsoft.Win32.SafeHandles.SafePipeHandle pipe,
            out uint serverProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern Microsoft.Win32.SafeHandles.SafeProcessHandle OpenProcess(
            uint desiredAccess,
            [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
            uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        internal static extern int GetPackageFamilyName(
            Microsoft.Win32.SafeHandles.SafeProcessHandle process,
            ref uint packageFamilyNameLength,
            StringBuilder? packageFamilyName);

        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetAppContainerNamedObjectPath(
            IntPtr token,
            IntPtr appContainerSid,
            uint objectPathLength,
            StringBuilder? objectPath,
            out uint returnLength);

    }
}

public sealed record ControllerLinkOutputReport(
    ulong Sequence,
    long HostTimestamp,
    long MainReceiveTimestamp,
    byte[] Payload)
{
    public TimeSpan PipeLatency => TimeSpan.FromSeconds(
        (MainReceiveTimestamp - HostTimestamp) / (double)Stopwatch.Frequency);
}

public sealed class ControllerLinkHostException : Exception
{
    public ControllerLinkHostException(string message) : base(message) { }

    public ControllerLinkHostException(string message, Exception innerException)
        : base(message, innerException) { }
}
