using System.Runtime.InteropServices;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;
using SDL;

namespace PicoSwitch.Companion.Linux.Audio;

/// <summary>
/// SDL3's audio subsystem, reference-counted: the sink and the microphone each
/// hold it while open. SDL3 is already this companion's gamepad library and
/// ships inside the executable, so audio adds no native dependency; SDL picks
/// PipeWire, PulseAudio or ALSA, whichever the system runs (SteamOS: PipeWire).
/// </summary>
internal static class SdlAudio
{
    private static readonly object Gate = new();

    /// <summary>Hints are global and read when a device opens, so opening is serialised.</summary>
    public static object OpenGate => Gate;

    public static void Acquire()
    {
        if (!SDL3.SDL_InitSubSystem(SDL_InitFlags.SDL_INIT_AUDIO))
        {
            throw new InvalidOperationException($"SDL audio did not start: {SDL3.SDL_GetError()}");
        }
    }

    public static void Release() => SDL3.SDL_QuitSubSystem(SDL_InitFlags.SDL_INIT_AUDIO);
}

/// <summary>
/// Plays companion audio through the default output with an SDL3 audio stream
/// (EXPERIMENTAL): the Linux counterpart of the Windows
/// <c>WaveOutCompanionAudioSink</c> and the Android <c>AudioTrackCompanionAudioSink</c>.
///
/// Every buffering decision is <see cref="CompanionAudioPlayout"/>'s, the policy
/// the pipeline simulation validated, given the queue depth the stream reports
/// (<c>SDL_GetAudioStreamQueued</c>: written, not yet taken by the device). The
/// device's own period sits beyond that count; it is asked to be 10 ms
/// (<c>SDL_AUDIO_DEVICE_SAMPLE_FRAMES</c> = 480), and as a constant it does not
/// disturb the drift control, which holds the queue depth, not the absolute
/// latency. The stream is paused while prebuffering and after running dry,
/// exactly where the other sinks pause their devices.
/// </summary>
public sealed unsafe class SdlCompanionAudioSink : ICompanionAudioSink
{
    private const int BlockMillis = CompanionAudioJitterPolicy.BlockMillis;
    private const int MaxBlockFrames = CompanionAudioDriftControl.BlockFrames + 8;

    private readonly object gate = new();
    private readonly CompanionAudioPlayout playout;
    private SDL_AudioStream* stream;
    private bool acquired;
    private bool playing;
    private bool concealRequested;
    private int channels;
    private int frameBytes;
    private short[] stretched = [];

    /// <param name="initialPrebufferBlocks">See <see cref="CompanionAudioPlayout"/>. A
    /// Linux companion's Controller Link is BLE, so the ordinary floor applies.</param>
    public SdlCompanionAudioSink(int initialPrebufferBlocks = CompanionAudioJitterPolicy.MinPrebufferBlocks)
    {
        playout = new CompanionAudioPlayout(initialPrebufferBlocks);
    }

    public long Underruns { get { lock (gate) { return playout.Underruns; } } }

    public long Concealments { get { lock (gate) { return playout.Extensions; } } }

    public int PrebufferMillis { get { lock (gate) { return playout.PrebufferMillis; } } }

    public int QueuedMillis { get { lock (gate) { return stream is null ? 0 : (int)QueuedFrames() / 48; } } }

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
            if (stream is not null)
            {
                return;
            }

            SdlAudio.Acquire();
            acquired = true;
            var spec = new SDL_AudioSpec
            {
                format = SDL_AudioFormat.SDL_AUDIO_S16LE,
                channels = channels,
                freq = sampleRate,
            };
            lock (SdlAudio.OpenGate)
            {
                SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
                SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Switch 2 console audio");
                SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Game");
                stream = SDL3.SDL_OpenAudioDeviceStream(SDL3.SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, null, IntPtr.Zero);
            }

            if (stream is null)
            {
                var error = SDL3.SDL_GetError();
                SdlAudio.Release();
                acquired = false;
                throw new InvalidOperationException($"no audio output: {error}");
            }

            // Opened paused: held until the prebuffer fills.
            this.channels = channels;
            frameBytes = channels * sizeof(short);
            playing = false;
        }
    }

    public bool Write(ReadOnlySpan<short> interleaved)
    {
        lock (gate)
        {
            if (stream is null)
            {
                return false;
            }

            var queuedFrames = QueuedFrames();
            var queuedBlocks = (int)((queuedFrames + CompanionAudioDriftControl.BlockFrames - 1) /
                                     CompanionAudioDriftControl.BlockFrames);
            var decision = playout.Accept(queuedBlocks, queuedFrames / 48.0);
            if (decision.Underrun && playing)
            {
                SDL3.SDL_PauseAudioStreamDevice(stream);
                playing = false;
            }

            if (decision.Drop)
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

            fixed (short* data = stretched)
            {
                if (!SDL3.SDL_PutAudioStreamData(stream, (IntPtr)data, samples * sizeof(short)))
                {
                    return false;
                }
            }

            concealRequested |= decision.Extend;
            if (playout.Queued(queuedBlocks + 1) && !playing)
            {
                SDL3.SDL_ResumeAudioStreamDevice(stream);
                playing = true;
            }

            return true;
        }
    }

    public void Stop()
    {
        lock (gate)
        {
            if (stream is not null)
            {
                SDL3.SDL_DestroyAudioStream(stream); // also closes the device it opened
                stream = null;
            }

            if (acquired)
            {
                SdlAudio.Release();
                acquired = false;
            }

            playing = false;
            playout.Stopped();
        }
    }

    public void Dispose() => Stop();

    // Called under gate.
    private long QueuedFrames()
    {
        var bytes = SDL3.SDL_GetAudioStreamQueued(stream);
        return bytes <= 0 ? 0 : bytes / frameBytes;
    }
}

/// <summary>The Linux companion's playback for companion audio.</summary>
public sealed class SdlCompanionAudioSinkFactory : ICompanionAudioSinkFactory
{
    public ICompanionAudioSink? CreateSink() => new SdlCompanionAudioSink();
}

/// <summary>
/// Captures the default microphone with an SDL3 recording stream at 48 kHz mono
/// 16-bit, delivered as whole 20 ms blocks on a capture thread (EXPERIMENTAL):
/// the counterpart of the Windows <c>WaveInCompanionMicCapture</c>. SDL converts
/// from whatever the device runs at; the adapter's drift control absorbs this
/// device's clock.
///
/// Echo: no echo cancellation is applied here (PipeWire's echo-cancel module
/// can provide it system-wide). Console audio played on SPEAKERS reaches the
/// microphone; headphones avoid it, as with a real headset.
/// </summary>
public sealed unsafe class SdlCompanionMicCapture : ICompanionMicCapture
{
    private const int BlockSamples = CompanionMicDataPlane.SamplesPerFrame;
    private const int BlockBytes = BlockSamples * sizeof(short);
    private const int PollMillis = 5;

    private SDL_AudioStream* stream;
    private Thread? thread;
    private volatile bool running;
    private long captured;

    public long BlocksCaptured => Interlocked.Read(ref captured);

    public void Start(Action<short[]> onBlock)
    {
        if (stream is not null)
        {
            return;
        }

        SdlAudio.Acquire();
        var spec = new SDL_AudioSpec
        {
            format = SDL_AudioFormat.SDL_AUDIO_S16LE,
            channels = CompanionMicDataPlane.Channels,
            freq = CompanionMicDataPlane.SampleRate,
        };
        lock (SdlAudio.OpenGate)
        {
            SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
            SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Switch 2 headset microphone");
            SDL3.SDL_SetHint(SDL3.SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Communication");
            stream = SDL3.SDL_OpenAudioDeviceStream(SDL3.SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, null, IntPtr.Zero);
        }

        if (stream is null)
        {
            var error = SDL3.SDL_GetError();
            SdlAudio.Release();
            throw new InvalidOperationException($"no microphone: {error}");
        }

        if (!SDL3.SDL_ResumeAudioStreamDevice(stream))
        {
            var error = SDL3.SDL_GetError();
            Stop();
            throw new InvalidOperationException($"the microphone would not start: {error}");
        }

        running = true;
        thread = new Thread(() => Run(onBlock)) { IsBackground = true, Name = "companion-mic", Priority = ThreadPriority.AboveNormal };
        thread.Start();
    }

    private void Run(Action<short[]> onBlock)
    {
        var raw = Marshal.AllocHGlobal(BlockBytes);
        try
        {
            while (running)
            {
                var source = stream;
                if (source is null || SDL3.SDL_GetAudioStreamAvailable(source) < BlockBytes)
                {
                    Thread.Sleep(PollMillis);
                    continue;
                }

                if (SDL3.SDL_GetAudioStreamData(source, raw, BlockBytes) != BlockBytes)
                {
                    continue;
                }

                var block = new short[BlockSamples];
                Marshal.Copy(raw, block, 0, BlockSamples);
                Interlocked.Increment(ref captured);
                try
                {
                    onBlock(block);
                }
                catch (Exception)
                {
                    // A consumer fault must not stop the device.
                }
            }
        }
        finally
        {
            Marshal.FreeHGlobal(raw);
        }
    }

    public void Stop()
    {
        running = false;
        thread?.Join(1000);
        thread = null;
        if (stream is null)
        {
            return;
        }

        SDL3.SDL_DestroyAudioStream(stream);
        stream = null;
        SdlAudio.Release();
    }

    public void Dispose() => Stop();
}

/// <summary>The Linux microphone for the companion microphone uplink.</summary>
public sealed class SdlCompanionMicCaptureFactory : ICompanionMicCaptureFactory
{
    public ICompanionMicCapture? CreateCapture() => new SdlCompanionMicCapture();
}
