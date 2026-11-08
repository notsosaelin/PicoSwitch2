using System.Globalization;
using System.Runtime.InteropServices;

namespace PicoSwitch.Companion.Linux.Input;

/// <summary>One hidraw node, as sysfs describes it.</summary>
internal sealed record HidrawNode(string DevicePath, int Index, ushort Vendor, ushort Product, string Name, string? Uniq);

/// <summary>
/// hidraw discovery and reading. Discovery is sysfs only (no device opened);
/// <see cref="Open"/> is the one place a node is opened, read-only and
/// non-blocking, so a reader can always be stopped.
/// </summary>
internal static class Hidraw
{
    private const string ClassDirectory = "/sys/class/hidraw";

    /// <summary>Every hidraw node, ordered by index.</summary>
    public static IReadOnlyList<HidrawNode> Enumerate()
    {
        var nodes = new List<HidrawNode>();
        if (!Directory.Exists(ClassDirectory))
        {
            return nodes;
        }

        foreach (var entry in Directory.EnumerateDirectories(ClassDirectory))
        {
            var leaf = Path.GetFileName(entry);
            if (!leaf.StartsWith("hidraw", StringComparison.Ordinal) ||
                !int.TryParse(leaf.AsSpan(6), NumberStyles.None, CultureInfo.InvariantCulture, out var index))
            {
                continue;
            }

            try
            {
                var uevent = File.ReadAllText(Path.Combine(entry, "device", "uevent"));
                if (ParseUevent(uevent, $"/dev/{leaf}", index) is { } node)
                {
                    nodes.Add(node);
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                // A node that vanished between listing and reading is simply gone.
            }
        }

        nodes.Sort((a, b) => a.Index.CompareTo(b.Index));
        return nodes;
    }

    /// <summary>
    /// The parent HID device's uevent -> identity. <c>HID_ID</c> is
    /// <c>bus:vendor:product</c> in hex, each zero-padded (e.g.
    /// <c>0003:000028DE:000012FD</c>).
    /// </summary>
    public static HidrawNode? ParseUevent(string uevent, string devicePath, int index)
    {
        string? id = null, name = null, uniq = null;
        foreach (var raw in uevent.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            var eq = line.IndexOf('=');
            if (eq <= 0)
            {
                continue;
            }

            var value = line[(eq + 1)..];
            switch (line[..eq])
            {
                case "HID_ID": id = value; break;
                case "HID_NAME": name = value; break;
                case "HID_UNIQ": uniq = value; break;
            }
        }

        var parts = id?.Split(':');
        if (parts is not { Length: 3 } ||
            !uint.TryParse(parts[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var vendor) ||
            !uint.TryParse(parts[2], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var product) ||
            vendor > ushort.MaxValue || product > ushort.MaxValue)
        {
            return null;
        }

        return new HidrawNode(
            devicePath,
            index,
            (ushort)vendor,
            (ushort)product,
            string.IsNullOrWhiteSpace(name) ? "Controller" : name.Trim(),
            string.IsNullOrWhiteSpace(uniq) ? null : uniq.Trim());
    }

    /// <summary>Open read-only and non-blocking; null (with errno text) when refused.</summary>
    public static Handle? Open(string devicePath, out string? error)
    {
        var fd = Native.open(devicePath, Native.O_RDONLY | Native.O_NONBLOCK | Native.O_CLOEXEC);
        if (fd < 0)
        {
            error = Marshal.GetLastPInvokeErrorMessage();
            return null;
        }

        error = null;
        return new Handle(fd);
    }

    /// <summary>An open hidraw file descriptor.</summary>
    internal sealed class Handle : IDisposable
    {
        private int fd;

        public Handle(int fd) => this.fd = fd;

        /// <summary>
        /// One report, waiting at most <paramref name="timeoutMillis"/>. Returns the
        /// byte count, 0 on timeout, -1 when the device has gone or failed.
        /// </summary>
        public unsafe int Read(Span<byte> buffer, int timeoutMillis)
        {
            var poll = new Native.PollFd { fd = fd, events = Native.POLLIN };
            var ready = Native.poll(&poll, 1, timeoutMillis);
            if (ready < 0)
            {
                return Marshal.GetLastPInvokeError() == Native.EINTR ? 0 : -1;
            }

            if (ready == 0)
            {
                return 0;
            }

            if ((poll.revents & (Native.POLLERR | Native.POLLHUP | Native.POLLNVAL)) != 0)
            {
                return -1;
            }

            fixed (byte* data = buffer)
            {
                var count = Native.read(fd, data, (nint)buffer.Length);
                if (count < 0)
                {
                    var errno = Marshal.GetLastPInvokeError();
                    return errno is Native.EAGAIN or Native.EINTR ? 0 : -1;
                }

                return (int)count;
            }
        }

        /// <summary>
        /// Read until a report satisfying <paramref name="accept"/> arrives or the
        /// time is up. Used to recognise a report format before listing a node.
        /// </summary>
        public bool Probe(Func<ReadOnlySpan<byte>, bool> accept, int timeoutMillis)
        {
            Span<byte> buffer = stackalloc byte[256];
            var deadline = Environment.TickCount64 + timeoutMillis;
            while (true)
            {
                var left = (int)(deadline - Environment.TickCount64);
                if (left <= 0)
                {
                    return false;
                }

                var count = Read(buffer, left);
                if (count < 0)
                {
                    return false;
                }

                if (count > 0 && accept(buffer[..count]))
                {
                    return true;
                }
            }
        }

        public void Dispose()
        {
            var old = Interlocked.Exchange(ref fd, -1);
            if (old >= 0)
            {
                Native.close(old);
            }
        }
    }

    private static class Native
    {
        public const int O_RDONLY = 0;
        public const int O_NONBLOCK = 0x800;
        public const int O_CLOEXEC = 0x80000;
        public const short POLLIN = 0x1;
        public const short POLLERR = 0x8;
        public const short POLLHUP = 0x10;
        public const short POLLNVAL = 0x20;
        public const int EINTR = 4;
        public const int EAGAIN = 11;

        [StructLayout(LayoutKind.Sequential)]
        public struct PollFd
        {
            public int fd;
            public short events;
            public short revents;
        }

        [DllImport("libc", SetLastError = true)]
        public static extern int open([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int flags);

        [DllImport("libc", SetLastError = true)]
        public static extern unsafe int poll(PollFd* fds, nuint count, int timeout);

        [DllImport("libc", SetLastError = true)]
        public static extern unsafe nint read(int fd, byte* buffer, nint count);

        [DllImport("libc", SetLastError = true)]
        public static extern int close(int fd);
    }
}
