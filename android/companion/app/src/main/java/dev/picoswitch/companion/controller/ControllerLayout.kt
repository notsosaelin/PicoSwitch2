package dev.picoswitch.companion.controller

import android.content.Context
import androidx.core.content.edit

enum class ControllerFaceLayout(
    val key: String,
    val title: String,
    val description: String,
) {
    Auto("auto", "Auto", "Use a known handheld profile, otherwise Android's standard button positions"),
    Nintendo("nintendo", "Nintendo", "Match Nintendo-style A/B and X/Y printed labels"),
    Xbox("xbox", "Xbox", "Match Android and Xbox-style face-button positions"),
    ;

    companion object {
        fun fromKey(value: String?): ControllerFaceLayout = entries.firstOrNull { it.key == value } ?: Auto
    }
}

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
            return ResolvedControllerLayout(ControllerFaceLayout.Xbox, "No input source selected; using Android positions")
        }

        // Android exposes no portable physical-label-layout property. These are the bounded,
        // hardware-audited built-in controller identities; they select labels only and never
        // gate whether the device is usable. The manual override remains authoritative.
        val knownNintendoHandheld =
            source.vendorId == 0x2020 && source.productId in setOf(0x0111, 0x0112) ||
                source.vendorId == 0x2022 && source.productId == 0x3001 ||
                source.name.contains("Odin Controller", ignoreCase = true) ||
                source.name.contains("Retroid Pocket Controller", ignoreCase = true)
        return if (knownNintendoHandheld) {
            ResolvedControllerLayout(ControllerFaceLayout.Nintendo, "Known Nintendo-labeled handheld controller")
        } else {
            ResolvedControllerLayout(ControllerFaceLayout.Xbox, "Android exposes positions, not printed labels")
        }
    }

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

class ControllerLayoutStore(context: Context) {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(descriptor: String?): ControllerFaceLayout {
        if (descriptor.isNullOrBlank()) return ControllerFaceLayout.Auto
        return ControllerFaceLayout.fromKey(preferences.getString(key(descriptor), null))
    }

    fun save(descriptor: String, layout: ControllerFaceLayout) {
        if (descriptor.isBlank()) return
        preferences.edit { putString(key(descriptor), layout.key) }
    }

    private fun key(descriptor: String) = "source_$descriptor"

    companion object {
        internal const val FILE_NAME = "controller_layouts"
    }
}
