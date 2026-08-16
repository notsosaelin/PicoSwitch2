package dev.picoswitch.bridge.core

/**
 * One host input source the platform offered, reduced to the four signals the
 * bridge needs in order to decide whether it can drive a console.
 *
 * Every desktop and mobile platform enumerates more "gamepads" than the user
 * physically owns — synthetic keyboards, mapping services, virtual devices left
 * behind by other software. Filling this in is a platform backend's job; deciding
 * what is usable is not, because the rule is the same everywhere and getting it
 * wrong either hides real hardware or offers a source that can never produce
 * input.
 *
 * ## The case this rule was written from
 *
 * Identified on Android, 2026-08-14: an entry named `Virtual` with VID/PID
 * `0000:0000` appeared even on a phone with **no built-in controller at all**,
 * which ruled out a game-assistant or touch-mapping service and identified it as
 * the platform's own virtual keyboard. It reached the list because it advertises
 * a D-pad source, and the enumeration accepted a D-pad alone as evidence of a
 * controller. It is not one, and it can never produce stick, trigger, or
 * gamepad-button input.
 *
 * ## What the exclusion is based on (and what it deliberately is NOT)
 *
 * No device-name blacklist: names are unstable across vendors and controller
 * modes (the same physical controls enumerate as `Odin Controller`, then
 * `Xbox Gamepad` after a mode switch). Three ordered signals, strongest first:
 *
 *  1. [isVirtual] — the platform's own "this device is synthetic" classification,
 *     which is exactly what identifies the entry above.
 *  2. **No gamepad or joystick source.** A real controller classifies as a gamepad
 *     or joystick; a D-pad/keyboard classification alone describes a keyboard-like
 *     device.
 *  3. Anonymous **and** capability-less: no VID/PID *and* no motion axes *and* no
 *     gamepad buttons.
 *
 * (3) requires both halves so legitimate unusual hardware stays visible: a device
 * with a real VID/PID is never hidden however odd its capabilities, and a
 * VID/PID-less device that genuinely reports sticks or buttons is kept too — some
 * kernel-level built-in controllers look like that. [isVirtual] is deliberately
 * NOT inferred from being backed by a virtual kernel node: the audited Retroid and
 * AYN Thor built-in controllers are, and are classified external, while being
 * entirely real. A backend must pass through the platform's classification rather
 * than deriving one.
 */
data class ControllerCandidate(
    val id: Int,
    val descriptor: String,
    val name: String,
    val vendorId: Int,
    val productId: Int,
    /** Reports at least one stick/trigger motion axis. */
    val hasMotionAxes: Boolean,
    /** Reports at least one standard gamepad button. */
    val hasGamepadButtons: Boolean,
    /** The platform classifies this as a synthetic device. Pass it through; never derive it. */
    val isVirtual: Boolean = false,
    /**
     * The platform classifies this as a gamepad or joystick. A D-pad/keyboard
     * classification alone does not count — that is what let a virtual keyboard
     * into the list.
     */
    val hasGamepadSource: Boolean = true,
) {
    /** True when the device carries a real USB/Bluetooth identity. */
    val hasRealIdentity: Boolean get() = vendorId != 0 || productId != 0

    /** True when the device demonstrates any usable controller capability. */
    val hasAnyCapability: Boolean get() = hasMotionAxes || hasGamepadButtons

    /**
     * Why this device was hidden, or null when it is usable. Surfaced in
     * Diagnostics so a wrongly-hidden device can be identified from the field
     * instead of guessed at.
     */
    val exclusionReason: String?
        get() = when {
            isVirtual -> "The system reports this as a virtual device"
            !hasGamepadSource -> "Not a gamepad or joystick (D-pad/keyboard source only)"
            !hasRealIdentity && !hasAnyCapability ->
                "No VID/PID and no sticks or gamepad buttons (virtual or mapping device)"
            else -> null
        }

    val isUsable: Boolean get() = exclusionReason == null
}

object ControllerCandidates {
    /** Devices that can actually drive the console, in enumeration order. */
    fun usable(candidates: List<ControllerCandidate>): List<ControllerCandidate> =
        candidates.filter { it.isUsable }

    fun excluded(candidates: List<ControllerCandidate>): List<ControllerCandidate> =
        candidates.filterNot { it.isUsable }

    /**
     * The device to use without asking, or null when the user genuinely has a
     * choice (or nothing usable).
     *
     * Exactly one usable controller is the overwhelmingly common case on a
     * handheld, and making the user select the only possible option is pure
     * friction. With two or more, the app must not guess.
     */
    fun autoSelect(candidates: List<ControllerCandidate>): ControllerCandidate? =
        usable(candidates).singleOrNull()

    /**
     * Resolve the selection for a refresh: keep the user's existing choice when it
     * is still present and usable, otherwise auto-select when unambiguous.
     * Returns null when the user must choose (or nothing is usable).
     */
    fun resolveSelection(
        candidates: List<ControllerCandidate>,
        currentDescriptor: String?,
    ): ControllerCandidate? {
        val usable = usable(candidates)
        val kept = usable.firstOrNull { it.descriptor == currentDescriptor }
        return kept ?: usable.singleOrNull()
    }

    /** True when selection UI is worth showing at all. */
    fun needsUserChoice(candidates: List<ControllerCandidate>): Boolean =
        usable(candidates).size > 1
}
