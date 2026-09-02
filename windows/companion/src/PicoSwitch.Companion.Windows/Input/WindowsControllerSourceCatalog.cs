using PicoSwitch.Bridge.Core;
using Windows.Devices.Enumeration;
using Windows.Devices.HumanInterfaceDevice;
using Windows.Gaming.Input;

namespace PicoSwitch.Companion.Windows.Input;

/// <summary>
/// Enumerates the controllers Windows can offer Controller Link.
///
/// ## Why RawGameController and not Gamepad
///
/// <c>Windows.Gaming.Input.Gamepad</c> is the XInput-class SUBSET.
/// <c>RawGameController</c> is the superset and is the correct enumeration
/// surface. Measured on this bench 2026-09-02 with a Switch 2 Pro Controller
/// attached: <c>RawGameController</c> count 1, <c>Gamepad</c> count 0. Polling
/// <c>Gamepad</c> alone — which the first implementation did — is silently blind
/// to DualSense, DualShock 4, Switch Pro and most non-Xbox hardware.
///
/// ## Why HID as well
///
/// <c>RawGameController.DisplayName</c> returned the literal string
/// "HID-compliant game controller" for that same device, while HID enumeration
/// gave "Switch 2 Pro Controller". A picker full of identical generic names is
/// not a picker, so names come from HID and fall back to the WinRT one.
///
/// HID also carries <c>System.Devices.ContainerId</c>, which is the only
/// reliable way to tell a handheld's built-in controls from something plugged
/// in: a built-in device sits in the machine's own container.
/// </summary>
public sealed class WindowsControllerSourceCatalog
{
    /// <summary>
    /// The container every device physically part of this machine belongs to.
    /// A handheld's built-in gamepad is exactly that; a plugged-in pad gets a
    /// container of its own.
    /// </summary>
    private static readonly Guid MachineContainer =
        new("00000000-0000-0000-FFFF-FFFFFFFFFFFF");

    private const string InstanceIdProperty = "System.Devices.DeviceInstanceId";
    private const string ContainerIdProperty = "System.Devices.ContainerId";

    /// <summary>
    /// Nintendo USB identities the adapter can emulate. Used ONLY to notice that
    /// a candidate might be the connected adapter's own output — never to hide
    /// anything, because a genuine controller carries the same identity.
    /// </summary>
    /// <remarks>
    /// Keys are the management wire names from <c>Personalities.WireName</c>,
    /// not invented ones — the adapter reports "jcl", not "joycon2l", and a
    /// guessed key would silently never match, which is the worst outcome here:
    /// the guard would look present and do nothing.
    /// </remarks>
    private static readonly IReadOnlyDictionary<string, ushort> PersonalityProducts =
        new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase)
        {
            ["pro2"] = 0x2069,   // Pro Controller 2
            ["gc"] = 0x2073,     // NSO GameCube Controller
            ["jcl"] = 0x2067,    // Joy-Con 2 (L)
            ["jcr"] = 0x2066,    // Joy-Con 2 (R)
        };

    private const ushort NintendoVendor = 0x057E;

    /// <summary>
    /// The USB product id a management personality wire name emulates, or null
    /// when that name is not a controller personality. Exposed so a test can
    /// prove every personality is covered — a missing one makes the
    /// adapter-echo guard silently inert.
    /// </summary>
    public static ushort? PersonalityProductId(string? wireName) =>
        wireName is not null && PersonalityProducts.TryGetValue(wireName, out var product)
            ? product
            : null;

    /// <summary>
    /// Enumerate the current sources.
    /// </summary>
    /// <param name="adapterPersonality">
    /// The personality the connected adapter reports (<c>pro2</c>, <c>gc</c>, …),
    /// or null when no adapter is connected. Used to flag a candidate that is
    /// probably this adapter echoing itself.
    /// </param>
    public async Task<IReadOnlyList<WindowsControllerSource>> EnumerateAsync(
        string? adapterPersonality = null,
        CancellationToken cancellationToken = default)
    {
        var names = await ReadHidDevicesAsync(cancellationToken).ConfigureAwait(false);

        var sources = new List<WindowsControllerSource>();
        var index = 0;
        foreach (var raw in RawGameController.RawGameControllers)
        {
            var vendor = raw.HardwareVendorId;
            var product = raw.HardwareProductId;
            var isGamepadClass = Gamepad.FromGameController(raw) is not null;

            names.TryGetValue((vendor, product), out var hid);

            var candidate = new ControllerCandidate(
                Id: index++,
                Descriptor: raw.NonRoamableId ?? string.Empty,
                Name: FriendlyName(hid?.Name, raw.DisplayName, vendor, product),
                VendorId: vendor,
                ProductId: product,
                // RawGameController reports axes and buttons directly, which is
                // exactly the capability evidence the shared rule wants.
                HasMotionAxes: raw.AxisCount > 0,
                HasGamepadButtons: raw.ButtonCount > 0,
                // Windows has no "this is synthetic" flag on this surface, and
                // the shared rule is explicit that a backend must pass the
                // platform's classification through rather than invent one.
                IsVirtual: false,
                HasGamepadSource: true);

            sources.Add(new WindowsControllerSource(
                candidate,
                Attachment(hid?.ContainerId),
                isGamepadClass,
                MayBeThisAdapter(vendor, product, adapterPersonality)));
        }

        return sources;
    }

    private static string FriendlyName(
        string? hidName, string? winRtName, ushort vendor, ushort product)
    {
        // HID first: it is the only surface that returned a real product name.
        if (!string.IsNullOrWhiteSpace(hidName) && !IsGenericName(hidName))
        {
            return hidName;
        }

        if (!string.IsNullOrWhiteSpace(winRtName) && !IsGenericName(winRtName))
        {
            return winRtName;
        }

        // Both were generic. A VID/PID is worse than a name but far better than
        // three identical rows: it at least distinguishes the devices.
        return $"Controller {vendor:X4}:{product:X4}";
    }

    private static bool IsGenericName(string name) =>
        name.Contains("HID-compliant", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("USB Input Device", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("XINPUT compatible", StringComparison.OrdinalIgnoreCase);

    private static ControllerAttachment Attachment(Guid? containerId) =>
        containerId is null ? ControllerAttachment.Unknown
        : containerId.Value == MachineContainer ? ControllerAttachment.BuiltIn
        : ControllerAttachment.External;

    private static bool MayBeThisAdapter(
        ushort vendor, ushort product, string? adapterPersonality)
    {
        if (vendor != NintendoVendor || string.IsNullOrWhiteSpace(adapterPersonality))
        {
            return false;
        }

        return PersonalityProducts.TryGetValue(adapterPersonality, out var expected) &&
               expected == product;
    }

    private sealed record HidInfo(string? Name, Guid? ContainerId);

    /// <summary>
    /// HID gamepads and joysticks, keyed by USB identity.
    ///
    /// Keyed on VID/PID rather than paired one-to-one with RawGameController:
    /// the two surfaces expose different identifiers and there is no documented
    /// join between them. That means two identical controllers share a name and
    /// an attachment label — acceptable, because the shared candidate rule keys
    /// selection on NonRoamableId, which stays unique per device.
    /// </summary>
    private static async Task<Dictionary<(ushort, ushort), HidInfo>> ReadHidDevicesAsync(
        CancellationToken cancellationToken)
    {
        var map = new Dictionary<(ushort, ushort), HidInfo>();
        string[] properties = [InstanceIdProperty, ContainerIdProperty];

        // 0x05 gamepad and 0x04 joystick, the two Generic Desktop usages a
        // controller declares. Mouse, keyboard and system control are excluded
        // by WinRT itself and are not wanted here anyway.
        foreach (var usage in new ushort[] { 0x05, 0x04 })
        {
            DeviceInformationCollection found;
            try
            {
                found = await DeviceInformation
                    .FindAllAsync(HidDevice.GetDeviceSelector(0x01, usage), properties)
                    .AsTask(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Enumeration is best effort: without it the catalog still works
                // from RawGameController alone, with generic names.
                continue;
            }

            foreach (var device in found)
            {
                var instance = device.Properties.TryGetValue(InstanceIdProperty, out var raw)
                    ? raw as string
                    : null;
                if (!TryParseIdentity(instance, out var identity))
                {
                    continue;
                }

                Guid? container = device.Properties.TryGetValue(ContainerIdProperty, out var c)
                    && c is Guid guid ? guid : null;

                // First writer wins: a device with several HID collections
                // enumerates more than once and the entries agree on identity.
                map.TryAdd(identity, new HidInfo(device.Name, container));
            }
        }

        return map;
    }

    /// <summary>
    /// Pull VID/PID out of a device instance id such as
    /// <c>HID\VID_057E&amp;PID_2069&amp;MI_00\8&amp;2ca74762&amp;0&amp;0000</c>.
    /// </summary>
    public static bool TryParseIdentity(string? instanceId, out (ushort Vendor, ushort Product) identity)
    {
        identity = default;
        if (string.IsNullOrWhiteSpace(instanceId))
        {
            return false;
        }

        var vendor = ExtractHex(instanceId, "VID_");
        var product = ExtractHex(instanceId, "PID_");

        // Bluetooth LE devices spell it VID&0002057E / PID&2069 instead.
        vendor ??= ExtractHex(instanceId, "VID&");
        product ??= ExtractHex(instanceId, "PID&");

        if (vendor is null || product is null)
        {
            return false;
        }

        identity = (vendor.Value, product.Value);
        return true;
    }

    private static ushort? ExtractHex(string text, string marker)
    {
        var start = text.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
        if (start < 0)
        {
            return null;
        }

        start += marker.Length;
        var digits = 0;
        while (start + digits < text.Length && Uri.IsHexDigit(text[start + digits]))
        {
            digits++;
        }

        if (digits < 4)
        {
            return null;
        }

        // BLE spells the vendor as eight digits with a two-digit namespace
        // prefix (VID&0002057E); the identity is the low four.
        var slice = text.Substring(start + digits - 4, 4);
        return ushort.TryParse(slice, System.Globalization.NumberStyles.HexNumber,
                               System.Globalization.CultureInfo.InvariantCulture, out var value)
            ? value
            : null;
    }
}
