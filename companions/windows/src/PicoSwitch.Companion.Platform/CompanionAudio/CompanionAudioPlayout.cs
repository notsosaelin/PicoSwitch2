namespace PicoSwitch.Companion.Platform.CompanionAudio;

/// <summary>What a sink does with one arriving block.</summary>
/// <param name="Underrun">The device had run dry: pause it until the prebuffer refills.</param>
/// <param name="Drop">The queue is at its bound: discard the block.</param>
/// <param name="Frames">Otherwise, play the block as this many frames (drift control).</param>
/// <param name="Extend">After this block, queue one packet-loss-concealment block:
/// the queue nearly ran dry and the prebuffer grew by a block.</param>
public readonly record struct CompanionAudioPlayoutDecision(bool Underrun, bool Drop, int Frames, bool Extend = false);

/// <summary>
/// Every buffering decision a companion audio sink makes, separated from the
/// device so the Windows (waveOut) and Android (AudioTrack) sinks share one
/// policy and the policy can be simulated against the measured link timing
/// (CompanionAudioPipelineSimulationTests).
///
/// Per block: <see cref="Accept"/> with the device's queue as it is now, write
/// the block (stretched to <c>Frames</c>) unless dropped, and a concealment
/// block after it when <c>Extend</c>; then <see cref="Queued"/> once with the
/// new queue length; start the device when that returns true.
/// </summary>
public sealed class CompanionAudioPlayout
{
    /// <summary>
    /// A block landing with less than this queued was nearly late. It is a
    /// half-block: the next frame's normal arrival jitter alone can exceed it.
    /// </summary>
    public const double LowWaterMillis = 10.0;

    /// <summary>
    /// Starting depth, and floor, for a radio shared with Bluetooth Classic:
    /// 6 blocks, 120 ms. From the pipeline simulation: with a fifth of the BLE
    /// connection events lost to Classic traffic, runs of lost events reach
    /// 110 ms about every 20 minutes, which 100 ms does not cover.
    /// </summary>
    public const int ClassicSharedPrebufferBlocks = 6;

    private readonly CompanionAudioJitterPolicy policy;
    private readonly CompanionAudioDriftControl drift = new();
    private double lastQueuedMillis = double.MaxValue;

    public CompanionAudioPlayout(int initialPrebufferBlocks = CompanionAudioJitterPolicy.MinPrebufferBlocks)
    {
        policy = new CompanionAudioJitterPolicy(initialPrebufferBlocks);
    }

    public bool Playing { get; private set; }

    public long Underruns { get; private set; }

    public long Drops { get; private set; }

    /// <summary>Concealment blocks queued to grow the buffer without a gap.</summary>
    public long Extensions { get; private set; }

    public int PrebufferBlocks => policy.PrebufferBlocks;

    public int PrebufferMillis => policy.PrebufferMillis;

    public int MaxQueuedBlocks => policy.MaxQueuedBlocks;

    public CompanionAudioDriftControl Drift => drift;

    /// <summary>
    /// The queue depth held just before each write: one block under the
    /// prebuffer, so the queue rides out an arrival gap of about the prebuffer.
    /// </summary>
    public double TargetMillis =>
        (policy.PrebufferBlocks - 1) * CompanionAudioJitterPolicy.BlockMillis;

    /// <param name="queuedBlocks">Blocks written and not yet fully played.</param>
    /// <param name="queuedMillis">The same queue in exact time (written minus played).</param>
    public CompanionAudioPlayoutDecision Accept(int queuedBlocks, double queuedMillis)
    {
        lastQueuedMillis = Playing ? queuedMillis : double.MaxValue;
        var underrun = false;
        if (Playing && queuedBlocks == 0)
        {
            // Ran dry: pause and refill the prebuffer instead of playing each
            // late block the moment it lands.
            Underruns++;
            policy.OnUnderrun();
            drift.Reset();
            Playing = false;
            underrun = true;
        }

        if (queuedBlocks >= policy.MaxQueuedBlocks)
        {
            Drops++;
            return new CompanionAudioPlayoutDecision(underrun, true, 0);
        }

        if (!Playing)
        {
            return new CompanionAudioPlayoutDecision(underrun, false, CompanionAudioDriftControl.BlockFrames);
        }

        // Nearly dry: grow now, while there is still audio to conceal from,
        // rather than after the gap. The concealed block raises the queue by
        // one block, which is the new depth the drift control holds.
        var extend = queuedMillis < LowWaterMillis && policy.OnNearUnderrun();
        if (extend)
        {
            Extensions++;
        }

        var frames = drift.FramesFor(queuedMillis, TargetMillis);

        return new CompanionAudioPlayoutDecision(underrun, false, frames, extend);
    }

    /// <returns>True when the device should start (the prebuffer is full).</returns>
    public bool Queued(int queuedBlocksAfter)
    {
        // A calm link may give a block back; the drift control then drains the
        // queue to the shallower target, inaudibly.
        policy.OnBlockQueued(
            lastQueuedMillis >= CompanionAudioJitterPolicy.BlockMillis + LowWaterMillis);

        if (!Playing && queuedBlocksAfter >= policy.PrebufferBlocks)
        {
            Playing = true;
            drift.Reset();
            return true;
        }

        return false;
    }

    /// <summary>The device was stopped from outside (session end).</summary>
    public void Stopped()
    {
        Playing = false;
        drift.Reset();
    }
}
