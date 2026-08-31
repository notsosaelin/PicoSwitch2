using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;

namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>
/// Measures how far this PC can get towards the LE peripheral role.
///
/// ## Why this exists, and why it is not a property read
///
/// `WINDOWS_PASS.md` §8.3 and §14.3 both treat
/// `BluetoothAdapter.IsPeripheralRoleSupported` as the peripheral-role
/// precondition. On the only radio this project has measured — Intel
/// `VID_8087&PID_0032`, driver 24.40.10.8 — **that property reports true and
/// every connectable advertisement aborts.** A capability check built on it
/// would offer the user Controller Link, fail, and give no reason.
///
/// So this attempts the thing. It publishes a GATT service and asks for a
/// connectable advertisement, and reports the furthest step that actually
/// succeeded. See
/// docs/experiments/windows-hogp-bridge-feasibility-2026-08-31.md.
///
/// ## What it deliberately does not do
///
/// It does NOT build the full HOGP service, stream reports, or try to reach the
/// adapter. Controller Link is not implemented — §14.5's B3, whether the
/// adapter's HOGP client proceeds without the Device Information Service that
/// Windows forbids an application from publishing, has never been measured,
/// because no radio has advertised long enough to ask. This probe answers only
/// the question that blocks everything else, so that the page can tell the truth
/// per machine instead of hard-coding one bench's result.
///
/// A radio that DOES advertise makes the page say so, which is what lets the
/// experiment be resumed on new hardware rather than rediscovered.
///
/// ## The service it publishes
///
/// A neutral 128-bit UUID, not HID `0x1812`. The 2026-08-31 controls proved the
/// refusal is not service-specific — a meaningless service aborts identically —
/// so nothing is lost, and publishing a HID service the adapter might briefly
/// see, from a product that cannot then honour it, would be worse than useless.
/// The advertisement is stopped as soon as its status settles.
/// </summary>
public static class ControllerLinkProbe
{
    /// <summary>
    /// Deliberately meaningless. Advertising is what is being measured, not the
    /// service; see the class remarks.
    /// </summary>
    private static readonly Guid ProbeService = new("2b6f0e41-7c85-4a52-9d1f-3e8c6a04b7d9");

    /// <summary>How long to let Windows decide before calling it refused.</summary>
    private const int SettleMillis = 4_000;

    public static async Task<ControllerLinkCapability> MeasureAsync(
        CancellationToken cancellationToken = default)
    {
        BluetoothAdapter? adapter;
        try
        {
            adapter = await BluetoothAdapter.GetDefaultAsync().AsTask(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            return new ControllerLinkCapability(
                ControllerLinkStep.NoRadio, Detail: error.Message);
        }

        if (adapter is null)
        {
            return new ControllerLinkCapability(ControllerLinkStep.NoRadio);
        }

        var address = FormatAddress(adapter.BluetoothAddress);
        var claims = adapter.IsPeripheralRoleSupported;

        if (!claims)
        {
            // The one case where the property is trustworthy: a radio that does
            // not even claim the role certainly does not have it. Believing it in
            // the negative direction costs nothing.
            return new ControllerLinkCapability(
                ControllerLinkStep.NoPeripheralRole, claims, address);
        }

        GattServiceProviderResult created;
        try
        {
            created = await GattServiceProvider.CreateAsync(ProbeService).AsTask(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            return new ControllerLinkCapability(
                ControllerLinkStep.ServiceRefused, claims, address, error.Message);
        }

        if (created.Error != BluetoothError.Success)
        {
            return new ControllerLinkCapability(
                ControllerLinkStep.ServiceRefused, claims, address, created.Error.ToString());
        }

        var provider = created.ServiceProvider;
        try
        {
            provider.StartAdvertising(new GattServiceProviderAdvertisingParameters
            {
                IsConnectable = true,
                IsDiscoverable = true,
            });
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            return new ControllerLinkCapability(
                ControllerLinkStep.AdvertisingRefused, claims, address, error.Message);
        }

        try
        {
            var settled = await SettleAsync(provider, cancellationToken).ConfigureAwait(false);
            return settled is GattServiceProviderAdvertisementStatus.Started or
                             GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData
                ? new ControllerLinkCapability(
                    ControllerLinkStep.Advertising, claims, address, settled.ToString())
                : new ControllerLinkCapability(
                    ControllerLinkStep.AdvertisingRefused, claims, address, settled.ToString());
        }
        finally
        {
            // Never leave the radio advertising on the way out. The probe is a
            // measurement, and a measurement that changes what the machine is
            // broadcasting after it returns is a side effect nobody asked for.
            try
            {
                provider.StopAdvertising();
            }
            catch (Exception)
            {
                // Best effort; the process exiting stops it regardless.
            }
        }
    }

    /// <summary>
    /// Wait for a decision.
    ///
    /// `Created` and `Stopped` are both pre-decision states, and reading the
    /// property straight after <c>StartAdvertising</c> records "we did not wait"
    /// as though it were an answer — which is exactly the mistake the lab probe
    /// made on its first run.
    /// </summary>
    private static async Task<GattServiceProviderAdvertisementStatus> SettleAsync(
        GattServiceProvider provider, CancellationToken cancellationToken)
    {
        var deadline = Environment.TickCount64 + SettleMillis;
        while (Environment.TickCount64 < deadline &&
               provider.AdvertisementStatus is GattServiceProviderAdvertisementStatus.Created
                                            or GattServiceProviderAdvertisementStatus.Stopped)
        {
            await Task.Delay(50, cancellationToken).ConfigureAwait(false);
        }

        return provider.AdvertisementStatus;
    }

    private static string FormatAddress(ulong value) =>
        string.Join(':', BitConverter.GetBytes(value).Take(6).Reverse().Select(b => b.ToString("X2")));
}
