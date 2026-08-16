package dev.picoswitch.bridge.core

/**
 * Physical face-button LAYOUT, kept separate from logical button semantics.
 *
 * Host platforms report face buttons POSITIONALLY: the bottom face button is
 * "A" on Android and on XInput regardless of what the plastic says. A handheld
 * with Nintendo-style legends therefore reports the button labelled B as A. The
 * bridge's [ControllerButton] set is LOGICAL — what the console should receive —
 * so something has to convert, and this is it.
 *
 * The pipeline is explicitly three-stage:
 *
 * ```text
 * platform key/bit  ->  positional ControllerButton  ->  layout mapper  ->  logical ControllerButton
 *      (backend)              (backend)                    (core)              (wire)
 * ```
 */
enum class ControllerFaceLayout(
    val key: String,
    val title: String,
    val description: String,
) {
    Auto("auto", "Auto", "Use a known handheld profile, otherwise the platform's standard button positions"),
    Nintendo("nintendo", "Nintendo", "Match Nintendo-style A/B and X/Y printed labels"),
    Xbox("xbox", "Xbox", "Match the platform's positional (Xbox-style) face-button order"),
    ;

    companion object {
        fun fromKey(value: String?): ControllerFaceLayout = entries.firstOrNull { it.key == value } ?: Auto
    }
}

/**
 * Stable identity of a host input source.
 *
 * [descriptor] is whatever opaque, stable string the platform uses to recognize
 * the same physical device across reconnects (Android's `InputDevice.descriptor`,
 * a Linux `/dev/input/by-id` name, a Windows HID instance path). The bridge never
 * parses it; it only compares and persists it.
 */
data class ControllerSourceIdentity(
    val descriptor: String,
    val name: String,
    val vendorId: Int,
    val productId: Int,
)

data class ResolvedControllerLayout(
    val layout: ControllerFaceLayout,
    val reason: String,
)

object ControllerLayoutResolver {
    fun resolve(
        requested: ControllerFaceLayout,
        source: ControllerSourceIdentity?,
    ): ResolvedControllerLayout {
        if (requested != ControllerFaceLayout.Auto) {
            return ResolvedControllerLayout(requested, "Selected manually")
        }
        if (source == null) {
            return ResolvedControllerLayout(ControllerFaceLayout.Xbox, "No input source selected; using positional order")
        }

        // No portable platform property exposes the printed legend. These are the
        // bounded, hardware-audited built-in controller identities; they select labels
        // only and never gate whether the device is usable. The manual override remains
        // authoritative.
        val knownNintendoHandheld =
            source.vendorId == 0x2020 && source.productId in setOf(0x0111, 0x0112) ||
                source.vendorId == 0x2022 && source.productId == 0x3001 ||
                source.name.contains("Odin Controller", ignoreCase = true) ||
                source.name.contains("Retroid Pocket Controller", ignoreCase = true)
        return if (knownNintendoHandheld) {
            ResolvedControllerLayout(ControllerFaceLayout.Nintendo, "Known Nintendo-labeled handheld controller")
        } else {
            ResolvedControllerLayout(ControllerFaceLayout.Xbox, "Platforms expose positions, not printed labels")
        }
    }

    /** Positional face button -> logical bridge button under [resolved]. */
    fun mapFaceButton(button: ControllerButton, resolved: ControllerFaceLayout): ControllerButton {
        if (resolved != ControllerFaceLayout.Nintendo) return button
        return when (button) {
            ControllerButton.A -> ControllerButton.B
            ControllerButton.B -> ControllerButton.A
            ControllerButton.X -> ControllerButton.Y
            ControllerButton.Y -> ControllerButton.X
            else -> button
        }
    }
}

/**
 * Per-source layout persistence, implemented by the platform (preferences file,
 * registry, dotfile). The bridge only needs a stable string keyed by descriptor.
 */
interface ControllerLayoutStore {
    fun load(descriptor: String?): ControllerFaceLayout
    fun save(descriptor: String, layout: ControllerFaceLayout)

    /** For hosts with no persistence, and for tests. */
    object None : ControllerLayoutStore {
        override fun load(descriptor: String?) = ControllerFaceLayout.Auto
        override fun save(descriptor: String, layout: ControllerFaceLayout) = Unit
    }
}
