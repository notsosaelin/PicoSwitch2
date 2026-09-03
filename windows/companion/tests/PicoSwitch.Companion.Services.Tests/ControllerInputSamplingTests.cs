using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Just-in-time sampling on the realtime path.
///
/// The defect these exist to prevent is quiet and expensive: a source refreshed
/// by its own timer and read by a separate publisher timer puts the publisher's
/// whole period of sample age on the wire, and nothing about the resulting
/// stream looks wrong — every counter is healthy, the frames are well formed,
/// the input is simply old.
/// </summary>
public sealed class ControllerInputSamplingTests
{
    private static readonly ControllerSourceIdentity Pad =
        new("test-pad", "Test Pad", 0x045E, 0x02FF);

    /// <summary>A source that only produces state when it is asked to.</summary>
    private sealed class OnDemandSource(ControllerInputSession session) : IControllerInputSampler
    {
        public int SampleCount { get; private set; }

        public ControllerButtonSet Next { get; set; } = ControllerButtonSet.Empty;

        public void Sample()
        {
            SampleCount++;
            session.ApplyPhysicalFrame(Pad, Next, AnalogFrame.Neutral);
        }
    }

    [Fact]
    public void ThePublisherReadsTheSourceBeforeSnapshotting()
    {
        var session = new ControllerInputSession();
        var source = new OnDemandSource(session);
        session.AttachSampler(source);

        // The state exists only at the source; nothing has pushed it yet.
        source.Next = ControllerButtonSet.Empty.With(ControllerButton.A, true);

        // Layout-agnostic on purpose: the published set is remapped for the
        // resolved face layout, and which Nintendo button an Xbox A becomes is a
        // different subsystem's business. What matters here is that the press
        // reached the snapshot at all.
        Assert.True(session.Snapshot.Buttons.IsEmpty);

        var sampled = session.SampleAndSnapshot();

        Assert.Equal(1, source.SampleCount);
        Assert.False(sampled.Buttons.IsEmpty);
    }

    [Fact]
    public void EveryPublishTickTakesAFreshReading()
    {
        // A press that begins and ends between two ticks must not be reported
        // from a snapshot the publisher happens to catch late.
        var session = new ControllerInputSession();
        var source = new OnDemandSource(session);
        session.AttachSampler(source);

        source.Next = ControllerButtonSet.Empty.With(ControllerButton.B, true);
        Assert.False(session.SampleAndSnapshot().Buttons.IsEmpty);

        source.Next = ControllerButtonSet.Empty;
        Assert.True(session.SampleAndSnapshot().Buttons.IsEmpty);

        Assert.Equal(2, source.SampleCount);
    }

    [Fact]
    public void SamplingIsOptional()
    {
        // An event-driven source (the Touch Gamepad) has already written its
        // current state, so there is nothing to fetch and no sampler attached.
        // SampleAndSnapshot must still be the correct call for the publisher.
        var session = new ControllerInputSession();

        var state = session.SampleAndSnapshot();

        Assert.NotNull(state);
        Assert.Equal(session.Snapshot.Buttons, state.Buttons);
    }

    [Fact]
    public void DetachingTheSamplerStopsTheReads()
    {
        var session = new ControllerInputSession();
        var source = new OnDemandSource(session);
        session.AttachSampler(source);
        session.SampleAndSnapshot();
        Assert.Equal(1, source.SampleCount);

        session.AttachSampler(null);
        session.SampleAndSnapshot();

        Assert.Equal(1, source.SampleCount);
    }

    [Fact]
    public async Task SamplingRunsOutsideTheSessionLock()
    {
        // Sample() publishes through ApplyPhysicalFrame, which takes the very
        // lock SampleAndSnapshot is coordinating. Holding it across the call
        // would deadlock the one path that must never stall, so this asserts
        // the ordering rather than trusting a comment about it. A regression
        // here hangs the report scheduler on its first tick.
        var session = new ControllerInputSession();
        var source = new OnDemandSource(session);
        session.AttachSampler(source);

        var work = Task.Run(() => session.SampleAndSnapshot());
        var finished = await Task.WhenAny(work, Task.Delay(TimeSpan.FromSeconds(5)));

        Assert.Same(work, finished);
    }
}
