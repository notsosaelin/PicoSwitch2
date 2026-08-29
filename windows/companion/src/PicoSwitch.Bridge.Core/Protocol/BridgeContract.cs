namespace PicoSwitch.Bridge.Protocol;

/*
 * The bridge contract this companion build speaks.
 *
 * ## Why a version number exists at all
 *
 * The firmware identifies this bridge by an EXACT match on the HID descriptor,
 * and every v2 capability -- battery, motion, rumble, player LED -- is gated on
 * that one match. When the two ends disagree the failure is silent and deeply
 * misleading: the v1 fields (buttons, sticks, triggers, hat) keep working because
 * they parse through the generic profile, while all three v2 features vanish
 * together with no error anywhere.
 *
 * That happened on 2026-08-15. C/GameChat changed the descriptor from 14 buttons
 * + 2 pad bits to 15 + 1; the APK was updated while the adapter kept running
 * older firmware. Every source-level parity check passed, because they compare
 * the source tree against the source tree and cannot see what is flashed.
 *
 * This number is reported by BOTH ends at runtime so that skew is visible
 * immediately instead of being inferred from which features stopped working.
 *
 * ## Bumping
 *
 * Mirror of `ANDROID_BRIDGE_CONTRACT_VERSION` in
 * `tools/fixtures/android_controller_hid.h`, which carries the authoritative bump
 * rule and the version history. `tools/check_android_descriptor_parity.py` fails
 * if the C, Kotlin and C# copies ever disagree, so this cannot silently drift.
 *
 * In short: bump for anything a peer can observe -- descriptor bytes, wire
 * layout, field units or semantics, declared capabilities, output report
 * contents. Do not bump for comments, formatting or internal refactors.
 */

public static class BridgeContract
{
    /// <summary>
    /// See the version history in the C fixture.
    ///
    /// 3 = 15 buttons, 100 us motion ticks.
    /// 4 = 17 buttons (16 = GL, 17 = GR); the button field grew to three bytes,
    ///     which moved the hat and the entire vendor extension one byte later.
    /// </summary>
    public const int Version = 4;

    /// <summary>
    /// SHA-256 of the complete descriptor, registered per contract version.
    ///
    /// This is the guard that makes the version rule enforceable rather than
    /// merely written down. Pinning a handful of interesting bytes only catches
    /// the change you already thought of; a digest over all 161 bytes catches ANY
    /// of them. Change one byte anywhere and the digest moves, the test fails, and
    /// the only way forward is to bump <see cref="Version"/> and register the new
    /// digest — which is exactly the deliberate act that was missing when the
    /// descriptor went from 14 buttons to 15 and shipped against older firmware.
    ///
    /// All three languages are covered:
    /// <c>tools/check_android_descriptor_parity.py</c> digests the C fixture's
    /// bytes and checks them against this same registry in both the Kotlin and the
    /// C# source, so a coordinated edit to two sides still fails without a bump.
    ///
    /// Historical entries are kept. They cost nothing and they let a capture or a
    /// bug report from an older build be identified precisely.
    /// </summary>
    public static readonly IReadOnlyDictionary<int, string> DescriptorDigests =
        new Dictionary<int, string>
        {
            // Contract 1 and 2 predate this registry; their descriptors are not
            // reproduced here because no build that speaks them is supported.
            [3] = "6e94932f4cdd741a13fee02d0ad4e085d20970d5a1dba61df825952729b67e3a",

            // 4: buttons 1..17. Same 161 bytes -- only three of them changed (usage
            // maximum, report count, pad count) -- which is precisely why a digest
            // is the guard and not a length check.
            [4] = "f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054",
        };

    /// <summary>The digest this build's descriptor must have. Null if the version is unregistered.</summary>
    public static string? ExpectedDescriptorDigest =>
        DescriptorDigests.TryGetValue(Version, out var digest) ? digest : null;

    /// <summary>
    /// How the adapter's reported contract compares to <see cref="Version"/>.
    ///
    /// <c>Unknown</c> is deliberately NOT folded into <c>Compatible</c>. Firmware
    /// that does not report a contract predates this mechanism, which means it is
    /// older than the build that introduced it — exactly the situation this exists
    /// to catch. Claiming compatibility because nobody said otherwise is how the
    /// original incident stayed invisible for so long.
    /// </summary>
    public abstract record Compatibility
    {
        private Compatibility()
        {
        }

        public abstract string Summary { get; }

        /// <summary>Both ends agree.</summary>
        public sealed record Compatible(int Version) : Compatibility
        {
            public override string Summary => $"compatible (bridge contract {Version})";
        }

        /// <summary>Both ends reported, and they differ. Advanced features will be missing.</summary>
        public sealed record Mismatch(int Firmware, int Companion) : Compatibility
        {
            public bool FirmwareIsOlder => Firmware < Companion;

            public override string Summary => FirmwareIsOlder
                ? $"INCOMPATIBLE: adapter firmware implements bridge contract {Firmware}, " +
                  $"this app expects {Companion}. Flash current firmware; until then " +
                  "battery, motion and rumble will not work."
                : $"INCOMPATIBLE: adapter firmware implements bridge contract {Firmware}, " +
                  $"this app expects {Companion}. Install the matching companion build; " +
                  "until then battery, motion and rumble will not work.";
        }

        /// <summary>
        /// Connected, but the adapter's identity reply has not arrived yet.
        ///
        /// Distinct from <c>Unknown</c> on purpose. "We have not asked yet" and "we
        /// asked and this firmware has no contract" look identical if collapsed,
        /// and collapsing them made a healthy adapter flash an incompatibility
        /// warning for the second or two before its info reply landed.
        /// </summary>
        public sealed record Pending(int Companion) : Compatibility
        {
            public override string Summary =>
                $"checking adapter firmware (expecting bridge contract {Companion})";
        }

        /// <summary>The adapter answered, and reported no contract at all.</summary>
        public sealed record Unknown(int Companion) : Compatibility
        {
            public override string Summary =>
                "UNVERIFIED: this adapter firmware does not report a bridge contract, so it " +
                "predates contract reporting and is older than this app (which expects " +
                $"{Companion}). If battery, motion or rumble are missing, flash current firmware.";
        }

        /// <summary>Nothing connected; there is nothing to compare.</summary>
        public sealed record NotConnected : Compatibility
        {
            public static readonly NotConnected Instance = new();

            public override string Summary => "no adapter connected";
        }
    }

    /// <param name="firmwareContract">
    /// What the adapter reported, or null/&lt;=0 when it reported nothing (older
    /// firmware, or the field was absent).
    /// </param>
    /// <param name="connected">Whether an adapter is connected at all.</param>
    /// <param name="firmwareInfoAvailable">
    /// Whether the adapter's identity reply has been received at all. False means
    /// "not asked yet", which is <c>Pending</c> and NOT a compatibility claim in
    /// either direction.
    /// </param>
    public static Compatibility Evaluate(
        int? firmwareContract,
        bool connected,
        bool firmwareInfoAvailable = true) =>
        !connected ? Compatibility.NotConnected.Instance
        : !firmwareInfoAvailable ? new Compatibility.Pending(Version)
        : firmwareContract is null or <= 0 ? new Compatibility.Unknown(Version)
        : firmwareContract == Version ? new Compatibility.Compatible(Version)
        : new Compatibility.Mismatch(firmwareContract.Value, Version);

    /// <summary>True only when both ends reported and agreed. Never true on Pending or Unknown.</summary>
    public static bool IsProvenCompatible(Compatibility compatibility) =>
        compatibility is Compatibility.Compatible;

    /// <summary>
    /// Whether the user should be warned. Pending is deliberately silent: it is a
    /// transient state on every healthy connection, and warning during it trains
    /// people to ignore the warning that matters.
    /// </summary>
    public static bool WarrantsWarning(Compatibility compatibility) =>
        compatibility is Compatibility.Mismatch or Compatibility.Unknown;
}
