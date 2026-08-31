using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Bluetooth;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Controller Link, in the shape `WINDOWS_PASS.md` §14.6 says it takes when the
/// §14.5 gate does not pass.
///
/// **There is no transport here, and that is the deliverable.** The gate was run
/// on 2026-08-31 and is recorded in
/// docs/experiments/windows-hogp-bridge-feasibility-2026-08-31.md: B1 and B2
/// passed, and B3–B6 were never reached because this PC's radio refuses the
/// connectable advertisement the adapter would have to find. §31 Phase 6 "If the
/// gate fails" then requires the page to name the platform limitation and the
/// missing radio capability, and the release to ship without Controller Link.
///
/// What this class owns is the honest answer for THIS machine. The probe runs on
/// demand rather than at startup — it briefly asks the radio to advertise, which
/// is not something to do to every user on every launch for a feature that does
/// not exist yet — and the page holds a Check button.
///
/// The result is deliberately re-measurable. "We can revisit the concept later"
/// is only true if a user who plugs in a different Bluetooth adapter is told
/// something different, and hard-coding one bench's finding would have made that
/// impossible.
/// </summary>
public sealed class ControllerLinkService
{
    private readonly Func<CancellationToken, Task<ControllerLinkCapability>> measure;
    private readonly DiagnosticLog diagnostics;
    private readonly StateValue<ControllerLinkView> view = new(ControllerLinkView.Idle);
    private readonly SemaphoreSlim gate = new(1, 1);

    /// <param name="measure">
    /// Injectable for the same reason the radio probe and the pairing gateway
    /// are: a test that depended on the developing machine's Bluetooth hardware
    /// would fail on a build agent for reasons unrelated to the code, and every
    /// branch of the explanation has to be assertable.
    /// </param>
    public ControllerLinkService(
        DiagnosticLog diagnostics,
        Func<CancellationToken, Task<ControllerLinkCapability>>? measure = null)
    {
        this.diagnostics = diagnostics;
        this.measure = measure ?? ControllerLinkProbe.MeasureAsync;
    }

    public IReadOnlyStateValue<ControllerLinkView> View => view;

    /// <summary>The last measurement, or <see cref="ControllerLinkCapability.Unknown"/>.</summary>
    public ControllerLinkCapability Capability { get; private set; } = ControllerLinkCapability.Unknown;

    public async Task CheckAsync(CancellationToken cancellationToken = default)
    {
        if (!await gate.WaitAsync(0, cancellationToken).ConfigureAwait(false))
        {
            // Already measuring. A second Check while the radio is mid-attempt
            // would race the first one's StopAdvertising and could leave the
            // machine advertising after both returned.
            return;
        }

        try
        {
            view.Set(ControllerLinkView.Busy);
            var capability = await measure(cancellationToken).ConfigureAwait(false);
            Capability = capability;
            view.Set(ControllerLinkView.Of(capability));

            // Logged at Info even for the expected negative: this is the line a
            // support bundle needs to distinguish "this radio cannot" from "the
            // feature is unfinished", and the two look identical to a user.
            diagnostics.Info(
                "gamepad",
                $"controller-link probe: step={capability.Step} " +
                $"claimsPeripheralRole={capability.ClaimsPeripheralRole} " +
                $"radio={capability.RadioAddress ?? "unknown"} " +
                $"detail={capability.Detail ?? "none"}");
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            // A probe that throws must not leave the page saying "measuring…"
            // forever. Report it as a service refusal, which is what an
            // unexpected platform failure at this layer amounts to.
            Capability = new ControllerLinkCapability(
                ControllerLinkStep.ServiceRefused, Detail: error.Message);
            view.Set(ControllerLinkView.Of(Capability));
            diagnostics.Error("gamepad", $"controller-link probe failed: {error.Message}");
        }
        finally
        {
            gate.Release();
        }
    }
}
