namespace PicoSwitch.Companion.Platform.CompanionAudio;

/// <summary>
/// How much companion audio a playback sink holds before it plays (EXPERIMENTAL).
///
/// A fixed 60 ms prebuffer played cleanly when Controller Link was the only
/// other traffic on the radio. With a Bluetooth Classic DualSense Edge sharing
/// the adapter's radio the maintainer heard skips (2026-11-07), which is what
/// a sink running dry and refilling sounds like. One fixed number cannot fit
/// both cases without adding latency to the quiet one, so the buffer adapts:
///
/// <list type="bullet">
/// <item>A near underrun -- the queue below <see cref="CompanionAudioPlayout.LowWaterMillis"/>
/// when a block lands -- grows the prebuffer by one 20 ms block BEFORE the
/// device runs dry (<see cref="OnNearUnderrun"/>); the playout fills the extra
/// block with packet-loss concealment, so growth costs one concealed block
/// instead of a gap.</item>
/// <item>Every underrun grows the prebuffer by one 20 ms block, up to
/// <see cref="MaxPrebufferBlocks"/>. A link that stalls once tends to stall
/// again at the same depth.</item>
/// <item>After <see cref="StableBlocksToShrink"/> blocks (10 minutes) in which no
/// block needed the deepest one -- every block landed with more than a block
/// still queued -- the prebuffer gives one block back, down to its floor, so a
/// transient storm does not cost latency for the rest of the session. The
/// pipeline simulation found a 30 s timer gave blocks back that the link still
/// needed, trading latency for repeated gaps.</item>
/// <item>The queue may run <see cref="HeadroomBlocks"/> past the prebuffer
/// before blocks are dropped. That keeps a burst after a stall playable
/// while still bounding the delay.</item>
/// </list>
///
/// Pure so its policy is testable; the sink owns the device.
/// </summary>
public sealed class CompanionAudioJitterPolicy
{
    public const int BlockMillis = 20;
    public const int MinPrebufferBlocks = 3;
    public const int MaxPrebufferBlocks = 8;
    public const int HeadroomBlocks = 3;
    public const int StableBlocksToShrink = 30_000;

    /// <summary>The most blocks a sink ever needs to hold at once.</summary>
    public const int MaxQueuedBlocksCeiling = MaxPrebufferBlocks + HeadroomBlocks;

    private int stableBlocks;
    private readonly int floorBlocks;

    /// <param name="initialPrebufferBlocks">Where to start, and the floor it
    /// never shrinks below. <see cref="MinPrebufferBlocks"/> suits a BLE-only
    /// radio; a link known to share the radio with Bluetooth Classic (the
    /// Android companion's own Controller Link) starts deeper rather than learn
    /// its depth through an underrun.</param>
    public CompanionAudioJitterPolicy(int initialPrebufferBlocks = MinPrebufferBlocks)
    {
        floorBlocks = Math.Clamp(initialPrebufferBlocks, MinPrebufferBlocks, MaxPrebufferBlocks);
        PrebufferBlocks = floorBlocks;
    }

    public int PrebufferBlocks { get; private set; }

    public int MaxQueuedBlocks => PrebufferBlocks + HeadroomBlocks;

    public int PrebufferMillis => PrebufferBlocks * BlockMillis;

    public void OnUnderrun() => _ = Grow();

    /// <returns>True when the prebuffer grew (it was below the ceiling).</returns>
    public bool OnNearUnderrun() => Grow();

    private bool Grow()
    {
        stableBlocks = 0;
        if (PrebufferBlocks >= MaxPrebufferBlocks)
        {
            return false;
        }

        PrebufferBlocks++;
        return true;
    }

    /// <param name="spareBlock">False when this block landed with less than a
    /// block (plus the low-water margin) still queued, i.e. the deepest block
    /// was needed. Any such block restarts the window: a block is given back
    /// only after <see cref="StableBlocksToShrink"/> blocks that never touched it.</param>
    public void OnBlockQueued(bool spareBlock = true)
    {
        if (!spareBlock)
        {
            stableBlocks = 0;
            return;
        }

        if (++stableBlocks < StableBlocksToShrink)
        {
            return;
        }

        stableBlocks = 0;
        if (PrebufferBlocks > floorBlocks)
        {
            PrebufferBlocks--;
        }
    }
}
