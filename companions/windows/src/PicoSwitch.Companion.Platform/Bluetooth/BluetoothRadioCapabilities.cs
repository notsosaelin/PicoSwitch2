using PicoSwitch.Management;

namespace PicoSwitch.Companion.Platform.Bluetooth;

/// <summary>
/// What this machine's Bluetooth radio can actually do.
///
/// Probed rather than assumed, and reported rather than worked around. Two of
/// these decide whether a whole half of the product exists on this PC:
///
/// - <c>CentralRoleSupported</c> gates management. Without it the app cannot talk
///   to an adapter at all and must say so plainly.
/// - <c>PeripheralRoleSupported</c> gates Controller Link. It is **not universal
///   across the population** — this is Confirmed, not a hypothesis — so a machine
///   without it is a normal machine, and the UI must never present it as a fault.
/// </summary>
public sealed record BluetoothRadioCapabilities
{
    public bool RadioPresent { get; init; }

    public bool RadioOn { get; init; }

    public bool LowEnergySupported { get; init; }

    public bool CentralRoleSupported { get; init; }

    public bool PeripheralRoleSupported { get; init; }

    public bool AdvertisementOffloadSupported { get; init; }

    /// <summary>The adapter's own address, for the support bundle. Never used as identity.</summary>
    public string? LocalAddress { get; init; }

    /// <summary>Why management cannot run here, or null when it can.</summary>
    public string? ManagementBlockedReason =>
        !RadioPresent ? "This computer has no Bluetooth radio."
        : !RadioOn ? "Bluetooth is turned off."
        : !LowEnergySupported ? "This computer's Bluetooth radio does not support Bluetooth LE."
        : !CentralRoleSupported ? "This computer's Bluetooth radio cannot act as an LE central."
        : null;

    /// <summary>
    /// Why Controller Link cannot run here, or null when the radio permits it.
    ///
    /// A null answer is NOT a promise that Controller Link works: the peripheral
    /// role is necessary and not sufficient, and whether a Windows GATT server can
    /// satisfy the adapter's HOGP client at all is the open experiment in
    /// WINDOWS_PASS.md §14.5.
    /// </summary>
    public string? PeripheralBlockedReason =>
        ManagementBlockedReason is { } blocked ? blocked
        : !PeripheralRoleSupported
            ? "This computer's Bluetooth radio cannot advertise as a peripheral, so it cannot act as a controller."
            : null;

    public string Describe() =>
        $"radio={(RadioPresent ? "present" : "absent")} " +
        $"power={(RadioOn ? "on" : "off")} " +
        $"le={LowEnergySupported} central={CentralRoleSupported} " +
        $"peripheral={PeripheralRoleSupported} offload={AdvertisementOffloadSupported} " +
        $"addr={LocalAddress ?? "unknown"}";

    /// <summary>
    /// Throw the management-blocking reason, if there is one.
    ///
    /// Called before a connection attempt so "Bluetooth is turned off" surfaces as
    /// itself rather than as a connect timeout fifteen seconds later.
    /// </summary>
    public void RequireManagementCapable()
    {
        if (ManagementBlockedReason is { } reason)
        {
            throw new ManagementException(reason);
        }
    }
}
