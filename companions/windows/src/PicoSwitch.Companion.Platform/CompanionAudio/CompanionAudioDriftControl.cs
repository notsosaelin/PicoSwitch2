namespace PicoSwitch.Companion.Platform.CompanionAudio;

/// <summary>
/// Keeps a companion audio sink's queue at a constant depth while two clocks
/// disagree (EXPERIMENTAL).
///
/// The console paces the audio: it fills the adapter's USB speaker stream at
/// its own 48 kHz, so frames arrive at the CONSOLE's rate. This device's sound
/// output plays them at ITS rate. The two crystals never agree exactly. USB
/// full speed allows the host clock to be off by ±500 ppm, and a consumer
/// sound device typically by ±50-100 ppm. At 500 ppm the queue gains or loses
/// 20 ms, one whole block, every 40 s. Without correction that is a dropped
/// block (a click) or an underrun (a gap) every 40 s, forever, which no
/// prebuffer size fixes.
///
/// The remedy is inaudible: each 20 ms block is played one sample frame longer
/// or shorter (960 ± 1 frames, linear interpolation), which moves the queue
/// 20.8 µs per block. That corrects up to 1,042 ppm, twice the USB worst case.
/// A pitch change of 0.1 % is far below audibility.
///
/// Control law: the caller names the depth to hold (the playout holds the
/// queue, measured just before each write, at one block less than its
/// prebuffer, so a prebuffer of P blocks rides out an arrival gap of about
/// P x 20 ms). After <see cref="SettleBlocks"/> blocks of smoothing, when the
/// smoothed depth leaves the target by more than <see cref="DeadbandMillis"/>,
/// blocks are stretched or shrunk until it is back within
/// <see cref="ReleaseMillis"/>. The same loop moves the queue, inaudibly,
/// when the target itself moves (the playout grew or gave back a block). Pure,
/// so the whole loop is simulated against the measured link timing in tests.
/// </summary>
public sealed class CompanionAudioDriftControl
{
    public const int BlockFrames = CompanionAudioJitterPolicy.BlockMillis * 48;

    /// <summary>One second of smoothing before the first correction.</summary>
    public const int SettleBlocks = 50;

    /// <summary>Exponential smoothing over about one second of blocks.</summary>
    public const double Smoothing = 1.0 / 50.0;

    public const double DeadbandMillis = 4.0;

    public const double ReleaseMillis = 1.0;

    private int blocks;
    private double smoothed;
    private double? target;
    private int correcting;

    /// <summary>The depth being held, once smoothing has settled.</summary>
    public double? BaselineMillis => target;

    /// <summary>The smoothed queue depth the control acts on.</summary>
    public double SmoothedMillis => smoothed;

    /// <summary>Sample frames added (+) or removed (-) since the last reset.</summary>
    public long FramesAdjusted { get; private set; }

    /// <summary>Start over, as after playback (re)starts.</summary>
    public void Reset()
    {
        blocks = 0;
        smoothed = 0;
        target = null;
        correcting = 0;
    }

    /// <summary>
    /// How many frames the next block plays as, given the queue depth just
    /// before it is written and the depth to hold.
    /// </summary>
    public int FramesFor(double queuedMillis, double targetMillis)
    {
        smoothed = blocks == 0 ? queuedMillis : smoothed + Smoothing * (queuedMillis - smoothed);
        blocks++;
        if (blocks < SettleBlocks)
        {
            return BlockFrames;
        }

        target = targetMillis;
        var error = smoothed - targetMillis;
        var toward = error > 0 ? -1 : 1;
        if (Math.Abs(error) > DeadbandMillis)
        {
            // Always toward the target -- including when the target itself
            // moved past the queue while a correction was under way.
            correcting = toward;
        }
        else if (correcting != toward || Math.Abs(error) < ReleaseMillis)
        {
            // Inside the deadband: finish an approach, never push away.
            correcting = 0;
        }

        FramesAdjusted += correcting;
        return BlockFrames + correcting;
    }

    /// <summary>
    /// Plays <paramref name="input"/> (interleaved, <paramref name="channels"/>
    /// channels) as exactly <paramref name="outputFrames"/> frames by linear
    /// interpolation, the first and last frames kept exactly. Returns samples
    /// written.
    /// </summary>
    public static int Stretch(ReadOnlySpan<short> input, int channels, int outputFrames, Span<short> output)
    {
        var inputFrames = input.Length / channels;
        if (outputFrames <= 0 || inputFrames == 0)
        {
            return 0;
        }

        if (outputFrames == inputFrames)
        {
            input[..(inputFrames * channels)].CopyTo(output);
            return inputFrames * channels;
        }

        var step = outputFrames == 1 ? 0.0 : (inputFrames - 1) / (double)(outputFrames - 1);
        for (var i = 0; i < outputFrames; i++)
        {
            var position = i * step;
            var left = (int)position;
            var right = Math.Min(left + 1, inputFrames - 1);
            var fraction = position - left;
            for (var c = 0; c < channels; c++)
            {
                var a = input[left * channels + c];
                var b = input[right * channels + c];
                output[i * channels + c] = (short)Math.Round(a + (b - a) * fraction);
            }
        }

        return outputFrames * channels;
    }
}
