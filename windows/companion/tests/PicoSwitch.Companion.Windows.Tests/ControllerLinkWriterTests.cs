using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Windows.ControllerLink;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// The latest-state writer's contract, exercised through a fake that stands in
/// for the WinRT characteristic.
///
/// <see cref="GattControllerLinkDataPlane"/> itself needs a real
/// <c>GattDeviceService</c> and cannot be constructed without a radio, so the
/// mailbox/pump policy lives in <see cref="ControllerLinkWriter"/> where it can
/// be tested. That split is the point: the property being protected here —
/// never more than one write in flight, never a queue of stale frames — is
/// policy, and policy that only exists inside an untestable class is policy
/// nobody can prove.
/// </summary>
public sealed class ControllerLinkWriterTests
{
    private static byte[] Frame(byte marker)
    {
        var frame = new byte[ControllerLinkDataPlane.FrameBytes];
        var payload = new byte[ControllerLinkDataPlane.PayloadBytes];
        payload[0] = marker;
        ControllerLinkDataPlane.EncodeInput(payload, marker, frame);
        return frame;
    }

    /// <summary>A write we can hold open, so "in flight" is observable.</summary>
    private sealed class FakeSink
    {
        private readonly List<TaskCompletionSource<bool>> pendingWrites = [];

        public List<byte[]> Written { get; } = [];

        public int InFlight { get; private set; }

        public int MaximumObservedInFlight { get; private set; }

        public Task<bool> WriteAsync(byte[] frame)
        {
            lock (Written)
            {
                Written.Add(frame);
                InFlight++;
                if (InFlight > MaximumObservedInFlight)
                {
                    MaximumObservedInFlight = InFlight;
                }
            }

            var completion = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            lock (pendingWrites)
            {
                pendingWrites.Add(completion);
            }

            return CompleteAsync(completion);
        }

        private async Task<bool> CompleteAsync(TaskCompletionSource<bool> completion)
        {
            var result = await completion.Task.ConfigureAwait(false);
            lock (Written)
            {
                InFlight--;
            }

            return result;
        }

        public void CompleteOldest(bool success = true)
        {
            TaskCompletionSource<bool> completion;
            lock (pendingWrites)
            {
                completion = pendingWrites[0];
                pendingWrites.RemoveAt(0);
            }

            completion.SetResult(success);
        }

        public int Outstanding
        {
            get { lock (pendingWrites) { return pendingWrites.Count; } }
        }
    }

    private static async Task SettleAsync()
    {
        // The pump hops threads; give continuations a chance to run.
        for (var i = 0; i < 20; i++)
        {
            await Task.Yield();
            await Task.Delay(1);
        }
    }

    [Fact]
    public async Task NeverHasMoreThanOneWriteInFlight()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        // Flood far faster than the sink completes. Fire-and-forget would put
        // every one of these into the stack simultaneously.
        for (byte i = 1; i <= 50; i++)
        {
            writer.Publish(Frame(i));
        }

        await SettleAsync();

        Assert.Equal(1, sink.Outstanding);
        Assert.Equal(1, sink.MaximumObservedInFlight);
        Assert.Equal(1, writer.MaximumInFlight);
    }

    [Fact]
    public async Task ReplacesPendingStateInsteadOfQueueingIt()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        writer.Publish(Frame(1));   // goes out immediately
        await SettleAsync();
        Assert.Single(sink.Written);

        // These pile up against ONE mailbox slot while the first write is open.
        writer.Publish(Frame(2));
        writer.Publish(Frame(3));
        writer.Publish(Frame(4));
        await SettleAsync();

        Assert.Single(sink.Written);          // still only the first
        Assert.Equal(2, writer.StatesCoalesced); // 2 and 3 were superseded

        sink.CompleteOldest();
        await SettleAsync();

        // The NEWEST state goes next. 2 and 3 are gone, not queued behind it:
        // an older controller state has no value once a newer one exists.
        Assert.Equal(2, sink.Written.Count);
        Assert.Equal(4, sink.Written[1][ControllerLinkDataPlane.HeaderBytes]);
    }

    [Fact]
    public async Task IdlesWhenNothingIsPendingAndResumesOnTheNextPublish()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        writer.Publish(Frame(1));
        await SettleAsync();
        sink.CompleteOldest();
        await SettleAsync();

        // Drained: the pump must stop rather than spin on an empty mailbox.
        Assert.Single(sink.Written);
        Assert.Equal(0, sink.Outstanding);

        writer.Publish(Frame(2));
        await SettleAsync();
        Assert.Equal(2, sink.Written.Count);
    }

    [Fact]
    public async Task DoesNotRetransmitAFailedState()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        writer.Publish(Frame(1));
        await SettleAsync();
        sink.CompleteOldest(success: false);
        await SettleAsync();

        Assert.Equal(1, writer.WriteFailures);

        // A failed historical state is superseded, never retried: replaying it
        // would put stale input on the console after newer input existed.
        Assert.Single(sink.Written);

        writer.Publish(Frame(9));
        await SettleAsync();
        Assert.Equal(2, sink.Written.Count);
        Assert.Equal(9, sink.Written[1][ControllerLinkDataPlane.HeaderBytes]);
    }

    [Fact]
    public async Task CountsEveryPublishedStateSeparatelyFromIssuedWrites()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        writer.Publish(Frame(1));
        await SettleAsync();
        writer.Publish(Frame(2));
        writer.Publish(Frame(3));
        await SettleAsync();
        sink.CompleteOldest();
        await SettleAsync();
        sink.CompleteOldest();
        await SettleAsync();

        // generated vs issued vs coalesced must be separable, or a qualification
        // run cannot tell "the scheduler is slow" from "the radio is behind".
        Assert.Equal(3, writer.StatesPublished);
        Assert.Equal(2, writer.WritesIssued);
        Assert.Equal(1, writer.StatesCoalesced);
    }

    [Fact]
    public async Task StopsWritingOnceClosed()
    {
        var sink = new FakeSink();
        var writer = new ControllerLinkWriter(sink.WriteAsync);

        writer.Publish(Frame(1));
        await SettleAsync();
        writer.Publish(Frame(2));   // parked in the mailbox

        writer.Close();
        sink.CompleteOldest();
        await SettleAsync();

        // The parked frame is dropped, not flushed. A frame escaping after
        // teardown is exactly the held input Stop exists to prevent.
        Assert.Single(sink.Written);

        writer.Publish(Frame(3));
        await SettleAsync();
        Assert.Single(sink.Written);
    }
}
