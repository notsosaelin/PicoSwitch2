using System.Text;

namespace PicoSwitch.Bridge.Core;

/// <summary>
/// Boundary counters for locating the FIRST point where expected data disappears.
///
/// Written after a refactor lost battery, motion and rumble together while
/// ordinary input kept working. 302 passing unit tests did not catch it, because
/// they proved <c>fake transport -&gt; BridgeSession</c>, never
/// <c>real HID callback -&gt; real transport -&gt; live session</c>. Counters
/// close that gap on hardware: read them in order, and the first one that stays
/// at zero while its upstream neighbour advances is the broken boundary.
///
/// Deliberately counters, not logs. Each is a single interlocked increment on
/// paths that run at 125 Hz, and the whole set is rendered only when something
/// asks.
///
/// Read them in this order:
///
/// <code>
/// REVERSE  (adapter -> host)
///   TransportOutputCallbacks   HID stack delivered an output report at all
///   OutputFramesDecoded        it parsed as a bridge output report
///   OutputFramesRejected       it did not (framing/report-id mismatch)
///   SessionOutputApplied       the live session applied it
///   MotionWantedTransitions    the console's motion gate actually changed
///   RumbleRequestsProduced     a non-silent rumble reached the output backend
///
/// FORWARD  (host -> adapter)
///   ReportsSent                input reports the transport accepted
///   ReportsWithMotionBlock     ...carrying a valid motion block
///   ReportsWithBatteryBlock    ...carrying a valid battery level
///   MotionSamplesValid         backend returned a usable IMU sample
///   BatterySamplesValid        backend returned a usable battery reading
/// </code>
/// </summary>
public sealed class BridgeCounters
{
    public Counter TransportOutputCallbacks { get; } = new();

    public Counter OutputFramesDecoded { get; } = new();

    public Counter OutputFramesRejected { get; } = new();

    public Counter SessionOutputApplied { get; } = new();

    public Counter MotionWantedTransitions { get; } = new();

    public Counter RumbleRequestsProduced { get; } = new();

    public Counter ReportsSent { get; } = new();

    public Counter ReportsWithMotionBlock { get; } = new();

    public Counter ReportsWithBatteryBlock { get; } = new();

    public Counter MotionSamplesValid { get; } = new();

    public Counter BatterySamplesValid { get; } = new();

    /// <summary>One line, ordered so the first zero after a non-zero is the divergence.</summary>
    public string Snapshot()
    {
        var text = new StringBuilder();
        text.Append("in:cb=").Append(TransportOutputCallbacks.Value);
        text.Append(" dec=").Append(OutputFramesDecoded.Value);
        text.Append(" rej=").Append(OutputFramesRejected.Value);
        text.Append(" applied=").Append(SessionOutputApplied.Value);
        text.Append(" motionGate=").Append(MotionWantedTransitions.Value);
        text.Append(" rumble=").Append(RumbleRequestsProduced.Value);
        text.Append(" | out:sent=").Append(ReportsSent.Value);
        text.Append(" motionBlk=").Append(ReportsWithMotionBlock.Value);
        text.Append(" battBlk=").Append(ReportsWithBatteryBlock.Value);
        text.Append(" imuSamples=").Append(MotionSamplesValid.Value);
        text.Append(" battSamples=").Append(BatterySamplesValid.Value);
        return text.ToString();
    }

    /// <summary>
    /// The first boundary that produced nothing while its upstream neighbour did,
    /// or null when every stage that should have advanced did advance.
    /// </summary>
    public string? FirstDivergence()
    {
        (string Name, long Value)[] stages =
        [
            ("HID output callbacks", TransportOutputCallbacks.Value),
            ("output frames decoded", OutputFramesDecoded.Value),
            ("session applied output", SessionOutputApplied.Value),
        ];

        for (var index = 0; index < stages.Length - 1; index++)
        {
            var upstream = stages[index];
            var downstream = stages[index + 1];
            if (upstream.Value > 0 && downstream.Value == 0)
            {
                return $"{downstream.Name} is 0 while {upstream.Name} is {upstream.Value}";
            }
        }

        if (TransportOutputCallbacks.Value == 0 && ReportsSent.Value > 0)
        {
            return $"no HID output callbacks at all while {ReportsSent.Value} input reports were sent " +
                "-- the adapter is not sending feedback, or it did not recognize this bridge";
        }

        return null;
    }

    public void Reset()
    {
        foreach (var counter in All())
        {
            counter.Set(0);
        }
    }

    private IEnumerable<Counter> All()
    {
        yield return TransportOutputCallbacks;
        yield return OutputFramesDecoded;
        yield return OutputFramesRejected;
        yield return SessionOutputApplied;
        yield return MotionWantedTransitions;
        yield return RumbleRequestsProduced;
        yield return ReportsSent;
        yield return ReportsWithMotionBlock;
        yield return ReportsWithBatteryBlock;
        yield return MotionSamplesValid;
        yield return BatterySamplesValid;
    }

    /// <summary>A monotonic counter written from the input thread and read from anywhere.</summary>
    public sealed class Counter
    {
        private long value;

        public long Value => Interlocked.Read(ref value);

        public long Increment() => Interlocked.Increment(ref value);

        public void Set(long next) => Interlocked.Exchange(ref value, next);
    }
}
