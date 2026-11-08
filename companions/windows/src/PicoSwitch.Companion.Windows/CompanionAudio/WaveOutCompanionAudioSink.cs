using System.Runtime.InteropServices;
using PicoSwitch.Companion.Platform.CompanionAudio;

namespace PicoSwitch.Companion.Windows.CompanionAudio;

/// <summary>
/// Plays companion audio through the default output device with the classic
/// waveOut API (EXPERIMENTAL).
///
/// Chosen for the prototype because it needs no package and no COM interop,
/// and because its buffer model maps exactly onto the policy we want:
///
/// <list type="bullet">
/// <item>One device buffer per 20 ms block, from a fixed pool.</item>
/// <item><b>Bounded latency.</b> At most the policy's MaxQueuedBlocks are ever
/// queued; a block arriving beyond that is dropped and counted, so a burst
/// after a radio stall can never grow the delay permanently.</item>
/// <item><b>Prebuffer, and re-prebuffer after an underrun.</b> Playback starts
/// only once the policy's prebuffer is queued, which absorbs the BLE
/// connection-event jitter. Running dry pauses the device, counts an underrun
/// and grows the prebuffer (<see cref="CompanionAudioJitterPolicy"/>) rather
/// than stuttering block by block.</item>
/// <item><b>Growth before it runs dry, and clock drift.</b> Every decision is
/// made by <see cref="CompanionAudioPlayout"/>, given the exact queue depth
/// (written frames minus <c>waveOutGetPosition</c>). It may ask for one
/// concealment block to deepen the buffer (<see cref="TakeConcealRequest"/>)
/// and plays each block as 960 +/- 1 frames to hold the depth against the
/// console/PC clock difference (<see cref="CompanionAudioDriftControl"/>).</item>
/// </list>
/// </summary>
public sealed class WaveOutCompanionAudioSink : ICompanionAudioSink
{
    private const int PoolBlocks = CompanionAudioJitterPolicy.MaxQueuedBlocksCeiling + 2;
    private const int BlockMillis = CompanionAudioJitterPolicy.BlockMillis;

    // A stretched block is one frame longer than nominal; a little spare.
    private const int MaxBlockFrames = CompanionAudioDriftControl.BlockFrames + 8;

    private readonly CompanionAudioPlayout playout;
    private bool concealRequested;
    private uint writtenFrames;
    private short[] stretched = [];
    private int channels;

    private readonly object gate = new();
    private readonly IntPtr[] headers = new IntPtr[PoolBlocks];
    private readonly IntPtr[] buffers = new IntPtr[PoolBlocks];
    private readonly bool[] submitted = new bool[PoolBlocks];
    private IntPtr device;
    private int blockBytes;
    private bool playing;
    /// <param name="initialPrebufferBlocks">See <see cref="CompanionAudioPlayout"/>.</param>
    public WaveOutCompanionAudioSink(int initialPrebufferBlocks = CompanionAudioJitterPolicy.MinPrebufferBlocks)
    {
        playout = new CompanionAudioPlayout(initialPrebufferBlocks);
    }

    public long Underruns
    {
        get
        {
            lock (gate)
            {
                return playout.Underruns;
            }
        }
    }

    public long Concealments
    {
        get
        {
            lock (gate)
            {
                return playout.Extensions;
            }
        }
    }

    public int PrebufferMillis
    {
        get
        {
            lock (gate)
            {
                return playout.PrebufferMillis;
            }
        }
    }

    public int QueuedMillis
    {
        get
        {
            lock (gate)
            {
                return device == IntPtr.Zero ? 0 : (int)QueuedMillisExact(InFlight());
            }
        }
    }

    public bool TakeConcealRequest()
    {
        lock (gate)
        {
            var requested = concealRequested;
            concealRequested = false;
            return requested;
        }
    }

    public void Start(int sampleRate, int channels)
    {
        lock (gate)
        {
            if (device != IntPtr.Zero)
            {
                return;
            }

            var format = new WaveFormatEx
            {
                FormatTag = WaveFormatPcm,
                Channels = (ushort)channels,
                SamplesPerSec = (uint)sampleRate,
                BitsPerSample = 16,
                BlockAlign = (ushort)(channels * 2),
                AvgBytesPerSec = (uint)(sampleRate * channels * 2),
                ExtraSize = 0,
            };
            Check(WaveOutOpen(out device, WaveMapper, ref format, IntPtr.Zero, IntPtr.Zero, CallbackNull),
                  "waveOutOpen");
            // Held paused until the prebuffer fills.
            Check(WaveOutPause(device), "waveOutPause");
            playing = false;
            this.channels = channels;
            writtenFrames = 0;

            blockBytes = MaxBlockFrames * channels * 2;
            var headerSize = Marshal.SizeOf<WaveHeader>();
            for (var i = 0; i < PoolBlocks; i++)
            {
                buffers[i] = Marshal.AllocHGlobal(blockBytes);
                headers[i] = Marshal.AllocHGlobal(headerSize);
                var header = new WaveHeader { Data = buffers[i], BufferLength = (uint)blockBytes };
                Marshal.StructureToPtr(header, headers[i], false);
                Check(WaveOutPrepareHeader(device, headers[i], (uint)headerSize), "waveOutPrepareHeader");
                submitted[i] = false;
            }
        }
    }

    public bool Write(ReadOnlySpan<short> interleaved)
    {
        lock (gate)
        {
            if (device == IntPtr.Zero)
            {
                return false;
            }

            var inFlight = InFlight();
            var decision = playout.Accept(inFlight, QueuedMillisExact(inFlight));
            if (decision.Underrun)
            {
                // Ran dry: pause and refill the prebuffer instead of playing
                // each late block the moment it lands.
                WaveOutPause(device);
                playing = false;
            }

            if (decision.Drop)
            {
                return false;
            }

            var slot = FreeSlot();
            if (slot < 0)
            {
                return false;
            }

            var frames = Math.Min(decision.Frames, MaxBlockFrames);
            if (stretched.Length < frames * channels)
            {
                stretched = new short[frames * channels];
            }

            var samples = CompanionAudioDriftControl.Stretch(interleaved, channels, frames, stretched);
            if (samples == 0)
            {
                return false;
            }

            Marshal.Copy(stretched, 0, buffers[slot], samples);
            var bytes = samples * sizeof(short);

            Marshal.WriteInt32(headers[slot], BufferLengthOffset, bytes);
            if (WaveOutWrite(device, headers[slot], (uint)Marshal.SizeOf<WaveHeader>()) != 0)
            {
                return false;
            }

            submitted[slot] = true;
            writtenFrames = unchecked(writtenFrames + (uint)(samples / channels));
            concealRequested |= decision.Extend;
            if (playout.Queued(inFlight + 1) && !playing)
            {
                WaveOutRestart(device);
                playing = true;
            }

            return true;
        }
    }

    public void Stop()
    {
        lock (gate)
        {
            if (device == IntPtr.Zero)
            {
                return;
            }

            // Reset returns every queued buffer marked done, after which they
            // can be unprepared and freed.
            WaveOutReset(device);
            var headerSize = (uint)Marshal.SizeOf<WaveHeader>();
            for (var i = 0; i < PoolBlocks; i++)
            {
                if (headers[i] != IntPtr.Zero)
                {
                    WaveOutUnprepareHeader(device, headers[i], headerSize);
                    Marshal.FreeHGlobal(headers[i]);
                    headers[i] = IntPtr.Zero;
                }

                if (buffers[i] != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(buffers[i]);
                    buffers[i] = IntPtr.Zero;
                }

                submitted[i] = false;
            }

            WaveOutClose(device);
            device = IntPtr.Zero;
            playing = false;
            playout.Stopped();
        }
    }

    public void Dispose() => Stop();

    // Called under gate. Written minus played, exactly, while playing; whole
    // blocks while paused (the device position does not move then).
    private double QueuedMillisExact(int inFlight)
    {
        if (!playing)
        {
            return inFlight * BlockMillis;
        }

        var time = new MmTime { Type = TimeSamples };
        if (WaveOutGetPosition(device, ref time, (uint)Marshal.SizeOf<MmTime>()) != 0 ||
            time.Type != TimeSamples)
        {
            return inFlight * BlockMillis;
        }

        var queued = unchecked(writtenFrames - time.Value);
        // A position past what was written means the counter was reset; trust
        // the block count instead.
        return queued > (uint)(PoolBlocks * MaxBlockFrames) ? inFlight * BlockMillis : queued / 48.0;
    }

    // Called under gate.
    private int InFlight()
    {
        var count = 0;
        for (var i = 0; i < PoolBlocks; i++)
        {
            if (submitted[i] && (Flags(i) & HeaderDone) != 0)
            {
                submitted[i] = false;
            }

            if (submitted[i])
            {
                count++;
            }
        }

        return count;
    }

    private int FreeSlot()
    {
        for (var i = 0; i < PoolBlocks; i++)
        {
            if (!submitted[i])
            {
                return i;
            }
        }

        return -1;
    }

    private uint Flags(int slot) => (uint)Marshal.ReadInt32(headers[slot], FlagsOffset);

    private static void Check(uint result, string call)
    {
        if (result != 0)
        {
            throw new InvalidOperationException($"{call} failed (MMRESULT {result}).");
        }
    }

    private const ushort WaveFormatPcm = 1;
    private const uint WaveMapper = unchecked((uint)-1);
    private const uint CallbackNull = 0;
    private const uint HeaderDone = 0x1;
    private static readonly int FlagsOffset = (int)Marshal.OffsetOf<WaveHeader>(nameof(WaveHeader.Flags));
    private static readonly int BufferLengthOffset = (int)Marshal.OffsetOf<WaveHeader>(nameof(WaveHeader.BufferLength));

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

    private const uint TimeSamples = 0x0002;

    // MMTIME: a type tag and an 8-byte union; TIME_SAMPLES uses the first word.
    [StructLayout(LayoutKind.Sequential)]
    private struct MmTime
    {
        public uint Type;
        public uint Value;
        public uint Pad;
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

    [DllImport("winmm.dll", EntryPoint = "waveOutOpen")]
    private static extern uint WaveOutOpen(out IntPtr device, uint deviceId, ref WaveFormatEx format,
                                           IntPtr callback, IntPtr instance, uint flags);

    [DllImport("winmm.dll", EntryPoint = "waveOutPrepareHeader")]
    private static extern uint WaveOutPrepareHeader(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveOutUnprepareHeader")]
    private static extern uint WaveOutUnprepareHeader(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveOutWrite")]
    private static extern uint WaveOutWrite(IntPtr device, IntPtr header, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveOutPause")]
    private static extern uint WaveOutPause(IntPtr device);

    [DllImport("winmm.dll", EntryPoint = "waveOutRestart")]
    private static extern uint WaveOutRestart(IntPtr device);

    [DllImport("winmm.dll", EntryPoint = "waveOutReset")]
    private static extern uint WaveOutReset(IntPtr device);

    [DllImport("winmm.dll", EntryPoint = "waveOutGetPosition")]
    private static extern uint WaveOutGetPosition(IntPtr device, ref MmTime time, uint size);

    [DllImport("winmm.dll", EntryPoint = "waveOutClose")]
    private static extern uint WaveOutClose(IntPtr device);
}

/// <summary>The Windows playback sink for companion audio.</summary>
public sealed class WaveOutCompanionAudioSinkFactory : ICompanionAudioSinkFactory
{
    public ICompanionAudioSink? CreateSink() => new WaveOutCompanionAudioSink();
}
