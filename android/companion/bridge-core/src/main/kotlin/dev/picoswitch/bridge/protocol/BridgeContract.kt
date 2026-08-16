package dev.picoswitch.bridge.protocol

/**
 * The bridge contract this companion build speaks.
 *
 * ## Why a version number exists at all
 *
 * The firmware identifies this bridge by an EXACT match on the HID descriptor,
 * and every v2 capability — battery, motion, rumble, player LED — is gated on
 * that one match. When the two ends disagree the failure is silent and deeply
 * misleading: the v1 fields (buttons, sticks, triggers, hat) keep working
 * because they parse through the generic profile, while all three v2 features
 * vanish together with no error anywhere.
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
 * `tools/fixtures/android_controller_hid.h`, which carries the authoritative
 * bump rule and the version history.
 * `tools/check_android_descriptor_parity.py` fails if the two ever disagree, so
 * this cannot silently drift from the C side.
 *
 * In short: bump for anything a peer can observe — descriptor bytes, wire
 * layout, field units or semantics, declared capabilities, output report
 * contents. Do not bump for comments, formatting or internal refactors.
 */
object BridgeContract {
    /** See the version history in the C fixture. 3 = 15 buttons, 100 us motion ticks. */
    const val VERSION = 3

    /**
     * SHA-256 of the complete descriptor, registered per contract version.
     *
     * This is the guard that makes the version rule enforceable rather than
     * merely written down. Pinning a handful of interesting bytes only catches
     * the change you already thought of; a digest over all 161 bytes catches ANY
     * of them. Change one byte anywhere and the digest moves, the test fails, and
     * the only way forward is to bump [VERSION] and register the new digest —
     * which is exactly the deliberate act that was missing when the descriptor
     * went from 14 buttons to 15 and shipped against older firmware.
     *
     * Both languages are covered: `tools/check_android_descriptor_parity.py`
     * digests the C fixture's bytes and checks them against this same registry,
     * so a coordinated edit to both sides still fails without a bump.
     *
     * Historical entries are kept. They cost nothing and they let a capture or a
     * bug report from an older build be identified precisely.
     */
    val DESCRIPTOR_DIGESTS: Map<Int, String> = mapOf(
        // Contract 1 and 2 predate this registry; their descriptors are not
        // reproduced here because no build that speaks them is supported.
        3 to "6e94932f4cdd741a13fee02d0ad4e085d20970d5a1dba61df825952729b67e3a",
    )

    /** The digest this build's descriptor must have. Null if the version is unregistered. */
    val expectedDescriptorDigest: String? get() = DESCRIPTOR_DIGESTS[VERSION]

    /**
     * How the adapter's reported contract compares to [VERSION].
     *
     * [Unknown] is deliberately NOT folded into [Compatible]. Firmware that does
     * not report a contract predates this mechanism, which means it is older than
     * the build that introduced it — exactly the situation this exists to catch.
     * Claiming compatibility because nobody said otherwise is how the original
     * incident stayed invisible for so long.
     */
    sealed interface Compatibility {
        val summary: String

        /** Both ends agree. */
        data class Compatible(val version: Int) : Compatibility {
            override val summary = "compatible (bridge contract $version)"
        }

        /** Both ends reported, and they differ. Advanced features will be missing. */
        data class Mismatch(val firmware: Int, val companion: Int) : Compatibility {
            val firmwareIsOlder: Boolean get() = firmware < companion
            override val summary: String
                get() = if (firmwareIsOlder) {
                    "INCOMPATIBLE: adapter firmware implements bridge contract $firmware, " +
                        "this app expects $companion. Flash current firmware; until then " +
                        "battery, motion and rumble will not work."
                } else {
                    "INCOMPATIBLE: adapter firmware implements bridge contract $firmware, " +
                        "this app expects $companion. Install the matching companion build; " +
                        "until then battery, motion and rumble will not work."
                }
        }

        /**
         * Connected, but the adapter's identity reply has not arrived yet.
         *
         * Distinct from [Unknown] on purpose. "We have not asked yet" and "we
         * asked and this firmware has no contract" look identical if collapsed,
         * and collapsing them made a healthy adapter flash an incompatibility
         * warning for the second or two before its info reply landed.
         */
        data class Pending(val companion: Int) : Compatibility {
            override val summary = "checking adapter firmware (expecting bridge contract $companion)"
        }

        /** The adapter answered, and reported no contract at all. */
        data class Unknown(val companion: Int) : Compatibility {
            override val summary =
                "UNVERIFIED: this adapter firmware does not report a bridge contract, so it " +
                    "predates contract reporting and is older than this app (which expects " +
                    "$companion). If battery, motion or rumble are missing, flash current firmware."
        }

        /** Nothing connected; there is nothing to compare. */
        data object NotConnected : Compatibility {
            override val summary = "no adapter connected"
        }
    }

    /**
     * @param firmwareContract what the adapter reported, or null/<=0 when it
     *   reported nothing (older firmware, or the field was absent).
     * @param firmwareInfoAvailable whether the adapter's identity reply has been
     *   received at all. False means "not asked yet", which is [Compatibility.Pending]
     *   and NOT a compatibility claim in either direction.
     */
    fun evaluate(
        firmwareContract: Int?,
        connected: Boolean,
        firmwareInfoAvailable: Boolean = true,
    ): Compatibility = when {
        !connected -> Compatibility.NotConnected
        !firmwareInfoAvailable -> Compatibility.Pending(VERSION)
        firmwareContract == null || firmwareContract <= 0 -> Compatibility.Unknown(VERSION)
        firmwareContract == VERSION -> Compatibility.Compatible(VERSION)
        else -> Compatibility.Mismatch(firmwareContract, VERSION)
    }

    /** True only when both ends reported and agreed. Never true on Pending or Unknown. */
    fun isProvenCompatible(compatibility: Compatibility): Boolean =
        compatibility is Compatibility.Compatible

    /**
     * Whether the user should be warned. Pending is deliberately silent: it is a
     * transient state on every healthy connection, and warning during it trains
     * people to ignore the warning that matters.
     */
    fun warrantsWarning(compatibility: Compatibility): Boolean =
        compatibility is Compatibility.Mismatch || compatibility is Compatibility.Unknown
}
