namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// THE PicoSwitch Bridge HID report descriptor.
///
/// This is a BRIDGE artifact, not a platform one. Every platform backend
/// registers these exact bytes.
///
/// Byte-identical to <c>ANDROID_CONTROLLER_V2_HID_DESCRIPTOR</c> in
/// <c>tools/fixtures/android_controller_hid.h</c> and to
/// <c>BridgeHidDescriptor.bytes</c> in the Kotlin bridge core. The firmware
/// identifies this bridge by an EXACT match on these bytes — never by VID/PID,
/// because a host device reports its own PC/phone identity, which varies per
/// vendor and cannot authorize output. Any edit here must be made in all three
/// places; <c>tools/check_android_descriptor_parity.py</c> and
/// <c>BridgeHidDescriptorTests</c> make a one-sided change fail loudly instead of
/// silently ending recognition.
/// </summary>
public static class BridgeHidDescriptor
{
    public static ReadOnlySpan<byte> Bytes => Descriptor;

    private static readonly byte[] Descriptor =
    [
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,

        // v1 axes
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,
        0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02,

        // buttons 1..17 (15 = C / GameChat, 16 = GL, 17 = GR) + 7 pad bits,
        // three bytes wide since contract 4 -- which is what moved the hat and
        // the whole vendor extension one byte later.
        0x05, 0x09, 0x19, 0x01, 0x29, 0x11, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
        0x95, 0x11, 0x81, 0x02, 0x75, 0x01, 0x95, 0x07, 0x81, 0x03,

        // v1 hat
        0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01,
        0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04,
        0x95, 0x01, 0x81, 0x03,

        // v2 vendor extension: motion, battery, flags, timestamp
        0x06, 0x00, 0xFF, 0x65, 0x00,
        0x09, 0x20, 0x09, 0x21, 0x09, 0x22, 0x09, 0x23, 0x09, 0x24, 0x09, 0x25,
        0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06,
        0x81, 0x02,
        0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
        0x95, 0x02, 0x81, 0x02,
        0x09, 0x32, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
        0x75, 0x10, 0x95, 0x01, 0x81, 0x02,

        // v2 output report: rumble + player LED + flags
        0x85, 0x02, 0x09, 0x40, 0x09, 0x41, 0x09, 0x42, 0x09, 0x43,
        0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02,
        0xC0,
    ];

    /// <summary>A defensive copy, for callers that need a mutable array to hand to a platform API.</summary>
    public static byte[] ToArray() => (byte[])Descriptor.Clone();

    /*
     * How the bridge presents itself in the host's HID service record.
     *
     * Kept here rather than in a backend so every platform advertises the same
     * record; only the mechanism of publishing it is platform work.
     *
     * The name does not say "Android" (it did while Android was the only
     * backend). Safe to change: the firmware matches the descriptor bytes and
     * never the service name, and no host test or fixture reads it. It does
     * appear in UART captures, so captures taken before 2026-08-15 show the old
     * `PicoSwitch Android Controller`.
     */
    public const string SdpName = "PicoSwitch Bridge Controller";
    public const string SdpDescription = "Host controls passthrough";
    public const string SdpProvider = "PicoSwitch2";
}
