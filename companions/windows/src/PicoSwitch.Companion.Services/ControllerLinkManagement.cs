using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Windows.ControllerLink;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The only management surface Controller Link may consume.
///
/// Path C rides the management connection, so this boundary now supplies both
/// halves of it: the control plane (<c>clink start|stop|status</c>) and access
/// to the binary data plane on the same session. Controller Link still opens
/// nothing itself — there is one Bluetooth session in the product and the
/// management owner holds it.
/// </summary>
public interface IControllerLinkManagement
{
    /// <summary>Trusted management is connected and the bridge contract is proven.</summary>
    bool Ready { get; }

    string? UnavailableReason { get; }

    event Action? Changed;

    /// <summary>
    /// Arm the adapter's data plane. The adapter measures the negotiated ATT
    /// MTU here, so the returned state answers "may I stream", not merely "did
    /// the command land".
    /// </summary>
    Task<ControllerLinkState> StartDataPlaneAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Release the data plane. The adapter publishes neutral before dropping the
    /// source and keeps the management link up: Controller Link stopping is not
    /// carrier loss.
    /// </summary>
    Task<ControllerLinkState> StopDataPlaneAsync(CancellationToken cancellationToken = default);

    Task<ControllerLinkState> DataPlaneStatusAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Two more characteristics on the live session, or null when there is no
    /// session to attach to.
    /// </summary>
    IControllerLinkDataPlane? TryCreateDataPlane();
}

public sealed class ControllerLinkManagement(AdapterConnectionService adapters)
    : IControllerLinkManagement
{
    public event Action? Changed;

    public bool Ready => UnavailableReason is null;

    public string? UnavailableReason
    {
        get
        {
            if (!adapters.Connection.Value.Connected ||
                adapters.Relationship.Value.Phase != AdapterRelationshipPhase.Connected)
            {
                return "Connect to a trusted PicoSwitch adapter first.";
            }

            var firmware = adapters.Snapshot.Value.Firmware;
            var compatibility = BridgeContract.Evaluate(
                firmware.BridgeContract,
                connected: true,
                firmwareInfoAvailable: !string.IsNullOrWhiteSpace(firmware.Id));
            return BridgeContract.IsProvenCompatible(compatibility)
                ? null
                : compatibility.Summary;
        }
    }

    public ControllerLinkManagement Subscribe()
    {
        adapters.Connection.Changed += OnChanged;
        adapters.Relationship.Changed += OnChanged;
        adapters.Snapshot.Changed += OnChanged;
        return this;
    }

    public Task<ControllerLinkState> StartDataPlaneAsync(
        CancellationToken cancellationToken = default) =>
        adapters.StartControllerLinkAsync(cancellationToken);

    public Task<ControllerLinkState> StopDataPlaneAsync(
        CancellationToken cancellationToken = default) =>
        adapters.StopControllerLinkAsync(cancellationToken);

    public Task<ControllerLinkState> DataPlaneStatusAsync(
        CancellationToken cancellationToken = default) =>
        adapters.ControllerLinkStatusAsync(cancellationToken);

    public IControllerLinkDataPlane? TryCreateDataPlane() =>
        adapters.DataPlanes?.TryCreateDataPlane();

    private void OnChanged() => Changed?.Invoke();
}
