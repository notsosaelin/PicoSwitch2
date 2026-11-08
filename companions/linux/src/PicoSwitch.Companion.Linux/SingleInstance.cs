using System.Net.Sockets;
using System.Text;

namespace PicoSwitch.Companion.Linux;

/// <summary>
/// One companion process per user session.
///
/// ## Why this is mandatory, not polish
///
/// The adapter admits exactly one management client. Two processes would hold
/// two BlueZ sessions to it, and the Android companion has a hardware-evidenced
/// defect (2026-08-23) showing what that looks like: Disconnect appears to do
/// nothing because another owner keeps the real session alive. The Windows app
/// redirects activation through AppInstance for the same reason.
///
/// ## Why an abstract Unix socket
///
/// A name in the Linux abstract socket namespace (leading NUL) is released by
/// the kernel the moment its owner dies, so a crash can never leave a stale
/// lock file that blocks the next launch — the failure mode of pid files and
/// of advisory locks on a file some cleaner deleted. It also gives the second
/// launch somewhere to send "bring the window forward", which is what the user
/// meant by launching again (Steam's Game Mode relaunches rather than focusing).
///
/// The namespace is per network namespace, not per user, so the user id is
/// part of the name; otherwise a second account on the same machine would be
/// told the app is already running.
/// </summary>
public sealed class SingleInstance : IDisposable
{
    private const string ActivateMessage = "activate\n";

    private readonly Socket listener;
    private readonly CancellationTokenSource stopping = new();

    private SingleInstance(Socket listener, Action onActivate)
    {
        this.listener = listener;
        _ = AcceptLoopAsync(onActivate, stopping.Token);
    }

    /// <summary>The abstract socket name for this user and application key.</summary>
    public static string NameFor(string key) => $"\0{key}.{Environment.UserName}";

    /// <summary>
    /// Become the instance, or hand activation to the one that already is.
    /// Returns null when another instance owns the name; that instance has then
    /// been asked to come forward and this process should exit.
    /// </summary>
    public static SingleInstance? TryAcquire(string key, Action onActivate)
    {
        var endpoint = new UnixDomainSocketEndPoint(NameFor(key));
        var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            socket.Bind(endpoint);
            socket.Listen(4);
            return new SingleInstance(socket, onActivate);
        }
        catch (SocketException error) when (error.SocketErrorCode == SocketError.AddressAlreadyInUse)
        {
            socket.Dispose();
            NotifyExisting(endpoint);
            return null;
        }
    }

    public void Dispose()
    {
        stopping.Cancel();
        listener.Dispose();
        stopping.Dispose();
    }

    private static void NotifyExisting(UnixDomainSocketEndPoint endpoint)
    {
        try
        {
            using var client = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            client.Connect(endpoint);
            client.Send(Encoding.ASCII.GetBytes(ActivateMessage));
        }
        catch (SocketException)
        {
            // The owner is shutting down. Exiting is still right: starting a
            // second session against a process that is mid-teardown is the race
            // this class exists to prevent, and the user can launch again.
        }
    }

    private async Task AcceptLoopAsync(Action onActivate, CancellationToken cancellationToken)
    {
        var buffer = new byte[32];
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                using var client = await listener.AcceptAsync(cancellationToken).ConfigureAwait(false);
                var read = await client.ReceiveAsync(buffer, SocketFlags.None, cancellationToken).ConfigureAwait(false);
                if (Encoding.ASCII.GetString(buffer, 0, read) == ActivateMessage)
                {
                    onActivate();
                }
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (SocketException)
            {
                // One misbehaving client must not end the guard for the rest of
                // the session.
            }
        }
    }
}
