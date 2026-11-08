using System.Runtime.InteropServices;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;

namespace PicoSwitch.Companion.Windows.CompanionAudio;

/// <summary>
/// Captures the default microphone with the classic waveIn API, the capture
/// twin of <see cref="WaveOutCompanionAudioSink"/> (EXPERIMENTAL): no package,
/// no COM, and a buffer model that is the uplink's block model.
///
/// Each device buffer is exactly one 20 ms block (960 samples, 48 kHz mono
/// 16-bit); a full buffer is one block for the encoder. <see cref="PoolBlocks"/>
/// buffers are queued so the device always has somewhere to write while the
/// capture thread hands a finished one on: 80 ms of slack against a late
/// thread. Windows resamples a device whose native format differs; the
/// adapter's drift control absorbs this device's clock.
///
/// Echo: waveIn applies no echo cancellation. Console audio played on the PC's
/// SPEAKERS will reach the microphone and the console's chat; headphones avoid
/// it, as with a real headset.
/// </summary>
public sealed class WaveInCompanionMicCapture : ICompanionMicCapture
{
    private const int PoolBlocks = 4;
    private const int BlockSamples = CompanionMicDataPlane.SamplesPerFrame;
    private const int BlockBytes = BlockSamples * 2;

    private readonly IntPtr[] headers = new IntPtr[PoolBlocks];
    private readonly IntPtr[] buffers = new IntPtr[PoolBlocks];
    private readonly AutoResetEvent ready = new(false);
    private IntPtr device;
    private Thread? thread;
    private volatile bool running;
    private Action<short[]>? onBlock;
    private long captured;

    public long BlocksCaptured => Interlocked.Read(ref captured);

    public void Start(Action<short[]> onBlock)
    {
        if (device != IntPtr.Zero)
        {
            return;
        }

        this.onBlock = onBlock;
        var format = new WaveFormatEx
        {
            FormatTag = WaveFormatPcm,
            Channels = (ushort)CompanionMicDataPlane.Channels,
            SamplesPerSec = (uint)CompanionMicDataPlane.SampleRate,
            BitsPerSample = 16,
            BlockAlign = (ushort)(CompanionMicDataPlane.Channels * 2),
            AvgBytesPerSec = (uint)(CompanionMicDataPlane.SampleRate * CompanionMicDataPlane.Channels * 2),
            ExtraSize = 0,
        };
        Check(WaveInOpen(out device, WaveMapper, ref format,
                         ready.SafeWaitHandle.DangerousGetHandle(), IntPtr.Zero, CallbackEvent),
              "waveInOpen");
        try
        {
            var headerSize = (uint)Marshal.SizeOf<WaveHeader>();
            for (var i = 0; i < PoolBlocks; i++)
            {
                buffers[i] = Marshal.AllocHGlobal(BlockBytes);
                headers[i] = Marshal.AllocHGlobal((int)headerSize);
                Marshal.StructureToPtr(new WaveHeader { Data = buffers[i], BufferLength = BlockBytes }, headers[i], false);
                Check(WaveInPrepareHeader(device, headers[i], headerSize), "waveInPrepareHeader");
                Check(WaveInAddBuffer(device, headers[i], headerSize), "waveInAddBuffer");
            }

            running = true;
            thread = new Thread(Run) { IsBackground = true, Name = "companion-mic", Priority = ThreadPriority.AboveNormal };
            thread.Start();
            Check(WaveInStart(device), "waveInStart");
        }
        catch
        {
            Stop();
            throw;
        }
    }

    public void Stop()
    {
        running = false;
        ready.Set();
        thread?.Join(1000);
        thread = null;
        if (device == IntPtr.Zero)
        {
            return;
        }

        // Returns every queued buffer as done; nothing is delivered after this.
        _ = WaveInReset(device);
        var headerSize = (uint)Marshal.SizeOf<WaveHeader>();
        for (var i = 0; i < PoolBlocks; i++)
        {
            if (headers[i] != IntPtr.Zero)
            {
                _ = WaveInUnprepareHeader(device, headers[i], headerSize);
                Marshal.FreeHGlobal(headers[i]);
                headers[i] = IntPtr.Zero;
            }

            if (buffers[i] != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(buffers[i]);
                buffers[i] = IntPtr.Zero;
            }
        }

        _ = WaveInClose(device);
        device = IntPtr.Zero;
    }

    public void Dispose()
    {
        Stop();
        ready.Dispose();
    }

    private void Run()
    {
        var headerSize = (uint)Marshal.SizeOf<WaveHeader>();
        var next = 0;
        while (running)
        {
            ready.WaitOne(100);
            // The device fills buffers in the order they were added.
            while (running)
            {
                var header = Marshal.PtrToStructure<WaveHeader>(headers[next]);
                if ((header.Flags & HeaderDone) == 0)
                {
                    break;
                }

                if (header.BytesRecorded == BlockBytes)
                {
                    var block = new short[BlockSamples];
                    Marshal.Copy(buffers[next], block, 0, BlockSamples);
                    Interlocked.Increment(ref captured);
                    try
                    {
                        onBlock?.Invoke(block);
                    }
                    catch (Exception)
                    {
                        // A consumer fault must not stop the device.
                    }
                }

                header.Flags &= ~HeaderDone;
                header.BytesRecorded = 0;
                Marshal.StructureToPtr(header, headers[next], false);
                if (WaveInAddBuffer(device, headers[next], headerSize) != 0)
                {
                    running = false;
                    break;
                }

                next = (next + 1) % PoolBlocks;
            }
        }
    }

    private static void Check(uint result, string call)
    {
        if (result != 0)
        {
            throw new InvalidOperationException($"{call} failed (MMRESULT {result}).");
        }
    }

    private const ushort WaveFormatPcm = 1;
    private const uint WaveMapper = unchecked((uint)-1);
    private const uint CallbackEvent = 0x00050000;
    private const uint HeaderDone = 0x1;

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    private struct WaveFormatEx
    {
        public ushort FormatTag;
        public ushort Channels;
        public uint SamplesPerSec;
        public uint AvgBytesPerSec;
        public ushort BlockAlign;
        public ushort BitsPerSample;
        public ushort ExtraSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WaveHeader
    {
        public IntPtr Data;
        public uint BufferLength;
        public uint BytesRecorded;
        public IntPtr User;
        public uint Flags;
        public uint Loops;
        public IntPtr Next;
        public IntPtr Reserved;
    }

    [DllImport("winmm.dll", EntryPoint = "waveInOpen")]
    private static extern uint WaveInOpen(out IntPtr device, uint deviceId, ref WaveFormatEx format,
                                          IntPtr callback, IntPtr instance, uint flags);

    [DllImport("winmm.dll", EntryPoint = "waveInPrepareHeader")]
    private static extern uint WaveInPrepareHeader(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveInUnprepareHeader")]
    private static extern uint WaveInUnprepareHeader(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveInAddBuffer")]
    private static extern uint WaveInAddBuffer(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveInStart")]
    private static extern uint WaveInStart(IntPtr device);

    [DllImport("winmm.dll", EntryPoint = "waveInReset")]
    private static extern uint WaveInReset(IntPtr device);

    [DllImport("winmm.dll", EntryPoint = "waveInClose")]
    private static extern uint WaveInClose(IntPtr device);
}

/// <summary>The Windows microphone capture for the companion microphone uplink.</summary>
public sealed class WaveInCompanionMicCaptureFactory : ICompanionMicCaptureFactory
{
    public ICompanionMicCapture? CreateCapture() =>
        WaveInGetNumDevs() == 0 ? null : new WaveInCompanionMicCapture();

    [DllImport("winmm.dll", EntryPoint = "waveInGetNumDevs")]
    private static extern uint WaveInGetNumDevs();
}
