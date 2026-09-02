using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The only management information Controller Link may consume. The helper
/// never opens a second management connection; the full-trust owner supplies
/// trust/contract state and invokes the adapter's existing remote-pairing verbs.
/// </summary>
public interface IControllerLinkManagement
{
    bool Ready { get; }

    string? UnavailableReason { get; }

    event Action? Changed;

    Task<PairingStatus> StartPairingAsync(CancellationToken cancellationToken = default);

    Task<PairingStatus> PairingStatusAsync(CancellationToken cancellationToken = default);

    Task<PairingStatus> CancelPairingAsync(CancellationToken cancellationToken = default);
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

    public Task<PairingStatus> StartPairingAsync(CancellationToken cancellationToken = default) =>
        adapters.StartControllerPairingAsync(cancellationToken);

    public Task<PairingStatus> PairingStatusAsync(CancellationToken cancellationToken = default) =>
        adapters.ControllerPairingStatusAsync(cancellationToken);

    public Task<PairingStatus> CancelPairingAsync(CancellationToken cancellationToken = default) =>
        adapters.CancelControllerPairingAsync(cancellationToken);

    private void OnChanged() => Changed?.Invoke();
}
