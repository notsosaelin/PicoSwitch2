namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>
/// Which step of the peripheral path this PC can actually perform.
///
/// Ordered from "nothing works" upward, so a caller can compare. Each value is
/// something that was MEASURED, never something a property claimed.
/// </summary>
public enum ControllerLinkStep
{
    /// <summary>Nothing has been measured yet.</summary>
    Unknown,

    /// <summary>No Bluetooth radio at all.</summary>
    NoRadio,

    /// <summary>A radio, but it does not claim the LE peripheral role.</summary>
    NoPeripheralRole,

    /// <summary>Windows refused to publish the GATT service. This is §14.5's B1.</summary>
    ServiceRefused,

    /// <summary>
    /// The service published, and the connectable advertisement did not start.
    ///
    /// The state this machine is in, and the reason Controller Link does not
    /// ship on Windows — see
    /// docs/experiments/windows-hogp-bridge-feasibility-2026-08-31.md.
    /// </summary>
    AdvertisingRefused,

    /// <summary>
    /// The PC published the service AND advertised connectably.
    ///
    /// Reaching this does NOT mean Controller Link works: §14.5's B3 (whether the
    /// adapter's HOGP client proceeds without the Device Information Service,
    /// which Windows will not let an application publish) has never been
    /// measured, because no radio has got this far. It means the platform
    /// blocker is gone and the experiment can be resumed.
    /// </summary>
    Advertising,
}

/// <summary>
/// What this PC can do towards acting as a controller for the adapter.
/// </summary>
/// <param name="Step">The furthest step that succeeded.</param>
/// <param name="ClaimsPeripheralRole">
/// What <c>BluetoothAdapter.IsPeripheralRoleSupported</c> reported.
///
/// Carried SEPARATELY from <paramref name="Step"/>, and never used to decide
/// anything, because the two disagree on real hardware: the 2026-08-31 run found
/// it reporting true on an Intel radio whose every connectable advertisement
/// aborted. Keeping both is what lets the UI say "your radio claims to support
/// this and does not do it", which is a far more useful sentence than either
/// fact alone.
/// </param>
/// <param name="RadioAddress">For the support bundle and the experiment record.</param>
/// <param name="Detail">The platform's own words, when it gave any.</param>
public sealed record ControllerLinkCapability(
    ControllerLinkStep Step,
    bool ClaimsPeripheralRole = false,
    string? RadioAddress = null,
    string? Detail = null)
{
    public static readonly ControllerLinkCapability Unknown = new(ControllerLinkStep.Unknown);

    /// <summary>
    /// Whether the platform blocker measured on 2026-08-31 is absent here.
    ///
    /// Deliberately NOT called "supported". Controller Link needs B3 as well, and
    /// no machine has ever reached the point where B3 could be asked.
    /// </summary>
    public bool PlatformPathOpen => Step == ControllerLinkStep.Advertising;
}
