using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Radios;

namespace PicoSwitch.Companion.Windows.Bluetooth;

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
        !RadioPresent ? "This PC has no Bluetooth radio."
        : !RadioOn ? "Bluetooth is turned off."
        : !LowEnergySupported ? "This PC's Bluetooth radio does not support Bluetooth LE."
        : !CentralRoleSupported ? "This PC's Bluetooth radio cannot act as an LE central."
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
            ? "This PC's Bluetooth radio cannot advertise as a peripheral, so it cannot act as a controller."
            : null;

    public string Describe() =>
        $"radio={(RadioPresent ? "present" : "absent")} " +
        $"power={(RadioOn ? "on" : "off")} " +
        $"le={LowEnergySupported} central={CentralRoleSupported} " +
        $"peripheral={PeripheralRoleSupported} offload={AdvertisementOffloadSupported} " +
        $"addr={LocalAddress ?? "unknown"}";
}

/// <summary>Reads the radio's capabilities once per probe. No state, no caching.</summary>
public static class WindowsBluetoothRadio
{
    public static async Task<BluetoothRadioCapabilities> ProbeAsync()
    {
        BluetoothAdapter? adapter;
        try
        {
            adapter = await BluetoothAdapter.GetDefaultAsync();
        }
        catch (Exception)
        {
            // A machine with no radio, or a stack that is mid-reinstall, throws
            // rather than returning null. Either way the honest answer is "no
            // usable radio", not an exception thrown at whoever asked.
            adapter = null;
        }

        if (adapter is null)
        {
            return new BluetoothRadioCapabilities();
        }

        var radioOn = false;
        try
        {
            var radio = await adapter.GetRadioAsync();
            radioOn = radio?.State == RadioState.On;
        }
        catch (Exception)
        {
            // Reading the radio needs a capability the app has; a failure here is
            // reported as "off" rather than as an absent radio, because the
            // adapter itself was found.
        }

        return new BluetoothRadioCapabilities
        {
            RadioPresent = true,
            RadioOn = radioOn,
            LowEnergySupported = adapter.IsLowEnergySupported,
            CentralRoleSupported = adapter.IsCentralRoleSupported,
            PeripheralRoleSupported = adapter.IsPeripheralRoleSupported,
            AdvertisementOffloadSupported = adapter.IsAdvertisementOffloadSupported,
            LocalAddress = BluetoothAddressFormat.ToText(adapter.BluetoothAddress),
        };
    }

    /// <summary>
    /// Throw the management-blocking reason, if there is one.
    ///
    /// Called before a connection attempt so "Bluetooth is turned off" surfaces as
    /// itself rather than as a connect timeout fifteen seconds later.
    /// </summary>
    public static void RequireManagementCapable(BluetoothRadioCapabilities capabilities)
    {
        if (capabilities.ManagementBlockedReason is { } reason)
        {
            throw new ManagementException(reason);
        }
    }
}
