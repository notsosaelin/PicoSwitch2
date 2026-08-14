package dev.picoswitch.companion.controller

/**
 * Which Android input devices can actually serve as a PicoSwitch2 controller.
 *
 * Android exposes more "gamepads" than a user has, so selecting a source used to
 * be a manual step with a non-functional entry in the list.
 *
 * ## What the reported `Virtual` / VID 0000 : PID 0000 entry actually is
 *
 * Identified 2026-08-14: the same entry appears on a **phone with no built-in
 * controller at all**, which rules out a handheld game-assistant or touch-mapping
 * service and identifies it as Android's own **virtual keyboard** device
 * (`KeyCharacterMap.VIRTUAL_KEYBOARD`, device id `-1`, literally named
 * "Virtual"). It exists on every Android device.
 *
 * It reached the list because it advertises `SOURCE_DPAD` — the virtual keyboard
 * can emit D-pad key events — and the enumeration accepted `SOURCE_DPAD` alone as
 * evidence of a controller. It is not one, and it can never produce stick,
 * trigger, or gamepad-button input.
 *
 * ## What the exclusion is based on (and what it deliberately is NOT)
 *
 * No device-name blacklist: names are unstable across OEMs and controller modes
 * (the same physical controls enumerate as `Odin Controller`, then `Xbox Gamepad`
 * after a mode switch). Three ordered signals, strongest first:
 *
 *  1. [android.view.InputDevice.isVirtual] — authoritative for Android's synthetic
 *     devices, and exactly what identifies the entry above.
 *  2. **No gamepad or joystick source.** A real controller reports `SOURCE_GAMEPAD`
 *     or `SOURCE_JOYSTICK`; `SOURCE_DPAD` alone describes a keyboard-like device.
 *  3. Anonymous **and** capability-less: no VID/PID *and* no motion axes *and* no
 *     gamepad buttons.
 *
 * (3) requires both halves so legitimate unusual hardware stays visible: a device
 * with a real VID/PID is never hidden however odd its capabilities, and a
 * VID/PID-less device that genuinely reports sticks or buttons is kept too — some
 * kernel-level built-in controllers look like that. `isVirtual` is deliberately
 * not inferred from being backed by a virtual kernel node: the audited Retroid and
 * AYN Thor built-in controllers are, and report `isExternal = true`, while being
 * entirely real.
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
    /** Android classifies this as a synthetic device. */
    val isVirtual: Boolean = false,
    /**
     * Reports `SOURCE_GAMEPAD` or `SOURCE_JOYSTICK`. `SOURCE_DPAD` alone does not
     * count — that is what let Android's virtual keyboard into the list.
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
            isVirtual -> "Android reports this as a virtual device"
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
