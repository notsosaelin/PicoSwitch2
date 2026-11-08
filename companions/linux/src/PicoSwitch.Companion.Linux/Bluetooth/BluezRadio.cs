using PicoSwitch.Companion.Platform.Bluetooth;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// Reads what the local BlueZ adapter can do. No state, no caching; never throws.
///
/// BlueZ does not report "LE supported" as such. <c>Adapter1.Roles</c>
/// (BlueZ 5.53+) lists the LE roles the controller supports ("central",
/// "peripheral", "central-peripheral"), so its presence is the LE evidence. On
/// an older BlueZ without the property, central LE is assumed: every adapter
/// SteamOS ships with is a dual-mode controller, and the connect attempt itself
/// reports the truth if that assumption is ever wrong.
/// </summary>
internal static class BluezRadio
{
    public static async Task<BluetoothRadioCapabilities> ProbeAsync(IBluezBus bus)
    {
        BluezAdapter? adapter;
        try
        {
            adapter = await bus.GetAdapterAsync().ConfigureAwait(false);
        }
        catch (BluezException)
        {
            // No system bus, or bluetoothd is not running: no usable radio.
            return new BluetoothRadioCapabilities();
        }

        return adapter is null ? new BluetoothRadioCapabilities() : Describe(adapter);
    }

    public static BluetoothRadioCapabilities Describe(BluezAdapter adapter)
    {
        var roles = adapter.Roles;
        var central = roles is null || roles.Any(role => role.Contains("central", StringComparison.Ordinal));
        var peripheral = roles is not null && roles.Any(role => role.Contains("peripheral", StringComparison.Ordinal));
        return new BluetoothRadioCapabilities
        {
            RadioPresent = true,

            // rfkill soft-block and "Bluetooth off" in the shell both surface as
            // Powered=false.
            RadioOn = adapter.Powered,
            LowEnergySupported = roles is null || roles.Count > 0,
            CentralRoleSupported = central,
            PeripheralRoleSupported = peripheral,
            AdvertisementOffloadSupported = false,
            LocalAddress = adapter.Address,
        };
    }
}
