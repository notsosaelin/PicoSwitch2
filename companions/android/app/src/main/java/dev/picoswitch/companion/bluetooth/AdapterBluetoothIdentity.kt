package dev.picoswitch.companion.bluetooth

/**
 * Names used only to discover the adapter. Bonds remain address/link-key
 * relationships; a product-name change does not migrate or replace them.
 */
object AdapterBluetoothIdentity {
    const val CURRENT_NAME = "PicoSwitch2"

    // Legacy firmware advertised this Classic name. Keep it only for finding
    // an already-installed adapter during migration to PicoSwitch2.
    const val LEGACY_NAME = "Joypad Adapter"

    // Retained for legacy-name compatibility tests and any future user-visible filtering.
    const val CHOOSER_NAME_PATTERN = "^(?:PicoSwitch2|Joypad Adapter)$"

    fun isCurrentName(name: String?): Boolean =
        name?.trim()?.equals(CURRENT_NAME, ignoreCase = true) == true

    fun isKnownAdapterName(name: String?): Boolean {
        val candidate = name?.trim() ?: return false
        return candidate.equals(CURRENT_NAME, ignoreCase = true) ||
            candidate.equals(LEGACY_NAME, ignoreCase = true)
    }
}
