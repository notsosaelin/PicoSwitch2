using PicoSwitch.Companion.Platform.Bluetooth;
using Windows.Devices.Bluetooth;
using Windows.Devices.Radios;

namespace PicoSwitch.Companion.Windows.Bluetooth;

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
}
