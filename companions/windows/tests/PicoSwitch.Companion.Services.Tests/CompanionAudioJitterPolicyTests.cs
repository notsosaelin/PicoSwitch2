using PicoSwitch.Companion.Platform.CompanionAudio;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

public sealed class CompanionAudioJitterPolicyTests
{
    [Fact]
    public void StartsAtTheValidatedSixtyMilliseconds()
    {
        var policy = new CompanionAudioJitterPolicy();
        Assert.Equal(60, policy.PrebufferMillis);
        Assert.Equal(6, policy.MaxQueuedBlocks);
    }

    [Fact]
    public void EachUnderrunGrowsThePrebufferUpToTheCeiling()
    {
        var policy = new CompanionAudioJitterPolicy();
        policy.OnUnderrun();
        Assert.Equal(80, policy.PrebufferMillis);
        for (var i = 0; i < 10; i++)
        {
            policy.OnUnderrun();
        }

        Assert.Equal(CompanionAudioJitterPolicy.MaxPrebufferBlocks, policy.PrebufferBlocks);
        Assert.Equal(CompanionAudioJitterPolicy.MaxQueuedBlocksCeiling, policy.MaxQueuedBlocks);
    }

    [Fact]
    public void TenQuietMinutesGiveOneBlockBackButNeverBelowTheFloor()
    {
        var policy = new CompanionAudioJitterPolicy();
        policy.OnUnderrun();
        policy.OnUnderrun();
        Assert.Equal(5, policy.PrebufferBlocks);

        for (var i = 0; i < CompanionAudioJitterPolicy.StableBlocksToShrink - 1; i++)
        {
            policy.OnBlockQueued();
        }

        Assert.Equal(5, policy.PrebufferBlocks);
        policy.OnBlockQueued();
        Assert.Equal(4, policy.PrebufferBlocks);

        for (var i = 0; i < CompanionAudioJitterPolicy.StableBlocksToShrink * 5; i++)
        {
            policy.OnBlockQueued();
        }

        Assert.Equal(CompanionAudioJitterPolicy.MinPrebufferBlocks, policy.PrebufferBlocks);
    }

    [Fact]
    public void AnUnderrunRestartsTheStableWindow()
    {
        var policy = new CompanionAudioJitterPolicy();
        policy.OnUnderrun();
        for (var i = 0; i < CompanionAudioJitterPolicy.StableBlocksToShrink - 1; i++)
        {
            policy.OnBlockQueued();
        }

        policy.OnUnderrun();
        policy.OnBlockQueued();
        Assert.Equal(5, policy.PrebufferBlocks);
    }

    [Fact]
    public void AWindowThatNeededTheDeepestBlockGivesNothingBack()
    {
        var policy = new CompanionAudioJitterPolicy();
        policy.OnUnderrun();
        for (var i = 0; i < CompanionAudioJitterPolicy.StableBlocksToShrink - 1; i++)
        {
            policy.OnBlockQueued();
        }

        // One tight block restarts the window; the shrink that was one block
        // away does not happen.
        policy.OnBlockQueued(spareBlock: false);
        policy.OnBlockQueued();
        Assert.Equal(4, policy.PrebufferBlocks);
    }

    [Fact]
    public void AClassicSharedSessionStartsDeepAndNeverShrinksBelowItsStart()
    {
        var policy = new CompanionAudioJitterPolicy(CompanionAudioPlayout.ClassicSharedPrebufferBlocks);
        Assert.Equal(120, policy.PrebufferMillis);
        policy.OnUnderrun();
        for (var i = 0; i < CompanionAudioJitterPolicy.StableBlocksToShrink * 4; i++)
        {
            policy.OnBlockQueued();
        }

        Assert.Equal(CompanionAudioPlayout.ClassicSharedPrebufferBlocks, policy.PrebufferBlocks);
    }

    [Fact]
    public void ANearUnderrunGrowsUntilTheCeilingThenReportsItCannot()
    {
        var policy = new CompanionAudioJitterPolicy();
        var grew = 0;
        while (policy.OnNearUnderrun())
        {
            grew++;
        }

        Assert.Equal(CompanionAudioJitterPolicy.MaxPrebufferBlocks - CompanionAudioJitterPolicy.MinPrebufferBlocks, grew);
    }
}

public sealed class CompanionAudioDriftControlTests
{
    [Fact]
    public void HoldsTheTargetAndCorrectsByOneFramePerBlock()
    {
        var drift = new CompanionAudioDriftControl();
        for (var i = 0; i < CompanionAudioDriftControl.SettleBlocks - 1; i++)
        {
            Assert.Equal(960, drift.FramesFor(70, 60));
        }

        // Settled and 10 ms too deep: shorten blocks.
        Assert.Equal(959, drift.FramesFor(70, 60));
        // Too shallow: lengthen them.
        var shallow = 0;
        for (var i = 0; i < 400; i++)
        {
            shallow = drift.FramesFor(40, 60);
        }

        Assert.Equal(961, shallow);
        // Back on target: stop.
        var steady = 0;
        for (var i = 0; i < 400; i++)
        {
            steady = drift.FramesFor(60, 60);
        }

        Assert.Equal(960, steady);
    }

    [Fact]
    public void ATargetThatMovesPastTheQueueReversesTheCorrection()
    {
        // Regression (pipeline simulation): a correction under way kept its
        // direction after the target moved, stretching an over-deep queue
        // until it dropped blocks every 45 s.
        var drift = new CompanionAudioDriftControl();
        for (var i = 0; i < 300; i++)
        {
            drift.FramesFor(40, 60); // lengthening
        }

        var frames = 0;
        for (var i = 0; i < 5; i++)
        {
            frames = drift.FramesFor(40, 20); // the target dropped below the queue
        }

        Assert.Equal(959, frames);
    }

    [Fact]
    public void StretchKeepsBothEndsAndInterpolatesBetween()
    {
        var input = new short[960 * 2];
        for (var i = 0; i < 960; i++)
        {
            input[i * 2] = (short)(i * 10);
            input[i * 2 + 1] = (short)(-i * 10);
        }

        foreach (var frames in new[] { 959, 961 })
        {
            var output = new short[frames * 2];
            Assert.Equal(frames * 2, CompanionAudioDriftControl.Stretch(input, 2, frames, output));
            Assert.Equal(0, output[0]);
            Assert.Equal(9590, output[^2]);
            Assert.Equal(-9590, output[^1]);
            // A ramp stays a ramp: monotonic, no sample out of the input's range.
            for (var i = 1; i < frames; i++)
            {
                Assert.True(output[i * 2] >= output[(i - 1) * 2]);
            }
        }
    }
}

public sealed class CompanionAudioPlayoutTests
{
    [Fact]
    public void StartsWhenThePrebufferIsQueuedAndUnderrunsWhenDry()
    {
        var playout = new CompanionAudioPlayout();
        Assert.False(playout.Accept(0, 0).Underrun);
        Assert.False(playout.Queued(1));
        playout.Accept(1, 20);
        Assert.False(playout.Queued(2));
        playout.Accept(2, 40);
        Assert.True(playout.Queued(3));

        var dry = playout.Accept(0, 0);
        Assert.True(dry.Underrun);
        Assert.Equal(1, playout.Underruns);
        Assert.Equal(80, playout.PrebufferMillis);
    }

    [Fact]
    public void ANearlyDryQueueGrowsWithOneConcealedBlockInsteadOfAGap()
    {
        var playout = new CompanionAudioPlayout();
        for (var i = 0; i < 3; i++)
        {
            playout.Accept(i, i * 20);
            playout.Queued(i + 1);
        }

        var decision = playout.Accept(1, CompanionAudioPlayout.LowWaterMillis - 1);
        Assert.True(decision.Extend);
        Assert.False(decision.Underrun);
        Assert.Equal(1, playout.Extensions);
        Assert.Equal(60, playout.TargetMillis);
    }

    [Fact]
    public void ABlockBeyondTheBoundIsDropped()
    {
        var playout = new CompanionAudioPlayout();
        var decision = playout.Accept(playout.MaxQueuedBlocks, 200);
        Assert.True(decision.Drop);
        Assert.Equal(1, playout.Drops);
    }
}
